// Copyright (c) 2026 Alexander Penkin. MIT License.

// AnnotatedCaptureHandler.cpp - render.capture_annotated: capture a viewport, then paint
// MEASURED overlays (world X/Y/Z axes, a Z=0 ground grid, and per-actor AABB wireframes +
// size labels) onto the PNG so an AI agent gets a metrically-annotated view of the scene
// rather than a bare screenshot.
//
// WHICH VIEWPORT. The active Level Editor one by default; an ASSET EDITOR's preview viewport
// when `subject` names an asset (staticMesh / skeletalMesh / animation / niagara). Both the
// frame and the overlay projection come from the SAME client - see step 2 - because an overlay
// projected through a different viewport than the one that rendered the pixels lands on
// coordinates that frame never had while still producing a perfectly plausible PNG.
//
// Reuse (nothing here reinvents the wheel):
//   * PreviewViewportCaptureUtils - ParseViewportCaptureRequest (base capture args +
//     validation) and CaptureEditorViewportToPng (pose apply / offscreen render /
//     ReadPixels / PNG encode / camera restore). Same viewport acquisition as
//     render.capture_open_level for the level path, and the CaptureSubject resolver's for the
//     asset path - the same acquisition render.capture_asset_preview uses.
//   * CaptureSubject - the subject resolver. For an asset kind it opens (or reuses) the asset
//     editor, hands back its preview viewport client + scene viewport and the asset's bounds,
//     and closes what it opened when the resolved subject goes out of scope.
//   * ViewProjectionUtils::FViewProjectionSession - pose-coherent world->screen projection.
//     Opened ONCE against the SAME client and the SAME FViewportCaptureRequest used for the
//     capture, so overlay pixels line up with the rendered frame. TOP-LEFT pixel origin. One
//     session for the whole paint pass: rebuilding the view is the expensive part (viewport
//     save / resize / CalcSceneView / restore) and nesting two sessions on one viewport would
//     make the inner one restore it to the outer one's mutated pose.
//   * ActorLabelOverlay (PinWrightActorLabels) - the pure selection/ordering/cap/de-overlap math
//     behind the opt-in `actorLabels` discovery mode. No UObject or drawing dependency, so it is
//     unit-testable on synthetic candidates.
//   * PinWrightBitmapPaint (Handlers/Render/BitmapPaint.h) - the shared CPU rasterizer
//     (clamped pixel / rect / stroked box / Bresenham line / badged 3x5-font label, all
//     alpha-composited). This file used to carry its own private copy of those primitives
//     because DriveSetOfMarkRenderer.cpp's copy was file-local and unreachable; both are
//     now folded into that one header, and the A-Z font and line routine this file added
//     went with them.

#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Render/ActorLabelOverlay.h"
#include "Handlers/Render/BitmapPaint.h"
#include "Handlers/Render/CaptureSubject.h"
#include "Handlers/Render/PreviewSceneRig.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Handlers/Render/ViewProjectionUtils.h"
#include "Utils/ActorUtils.h"
#include "Utils/ClassUtils.h"
#include "Utils/ScreenshotUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "IAssetViewport.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "LevelEditor.h"
#include "Math/Box2D.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"
#include "Slate/SceneViewport.h"

// Uniquely-named namespace (not anonymous) so a Unity-build TU merge cannot ODR-clash
// these overlay constants and paint adapters with same-named symbols elsewhere (same guard
// the Drive renderer uses for its own file-local constants).
namespace PinWrightAnnotatedCapture
{
    using PinWrightRenderCapture::FViewportCaptureRequest;

    // ---- Overlay colors ----
    static const FColor GAxisXColor(220, 40, 40, 255);   // world +X (red)
    static const FColor GAxisYColor(40, 200, 40, 255);   // world +Y (green)
    static const FColor GAxisZColor(60, 120, 240, 255);  // world +Z (blue)
    static const FColor GGridColor(150, 150, 150, 255);  // Z=0 grid lines
    static const FColor GBoundsColor(255, 216, 40, 255); // per-actor AABB wireframe
    // Discovery labels (the `actorLabels` overlay). Cyan, deliberately unlike GBoundsColor's
    // yellow: a yellow name means "you asked for this actor by name in `bounds`", a cyan one means
    // "this actor was found for you".
    static const FColor GActorLabelColor(80, 220, 240, 255);
    static const FColor GLabelInk(255, 255, 255, 255);   // label glyph ink
    static const FColor GLabelBadge(16, 16, 16, 255);    // label backing plate

    // ---- Stroke thickness / font metrics (px) ----
    static constexpr int32 GAxisThickness = 3;
    static constexpr int32 GGridThickness = 1;
    static constexpr int32 GBoxThickness = 2;
    static constexpr int32 GLabelScale = PinWrightBitmapPaint::DefaultLabelScale; // 3x5 cell -> 6x10 glyph

    // Readability cap on grid density: past ~41 lines per axis the grid stops being a measuring
    // aid and becomes noise, and overlays.grid.clamped reports when it bites. (This used to be a
    // COST cap - every projection rebuilt the whole viewport view. The paint pass now shares one
    // FViewProjectionSession, so projecting a point is a single matrix transform and the number
    // here is purely an image-quality decision. The value is unchanged so painted output is not.)
    static constexpr int32 GMaxGridHalfLines = 20; // up to 41 lines per axis

    // Font metrics, re-exported from the shared rasterizer so the label-placement maths below
    // (and PinWrightActorLabels' spacing default, which cites this line height) keep reading the
    // same numbers they always did.
    static constexpr int32 GGlyphW = PinWrightBitmapPaint::GlyphCellWidth;
    static constexpr int32 GGlyphH = PinWrightBitmapPaint::GlyphCellHeight;

    // Adapters, not a second implementation: they only bind the loose (Pixels, W, H) triple this
    // handler carries around into a PinWrightBitmapPaint::FSurface and pick the overlay's label
    // style. All drawing lives in BitmapPaint.cpp.
    inline PinWrightBitmapPaint::FSurface Surface(TArray<FColor>& Px, int32 W, int32 H)
    {
        return PinWrightBitmapPaint::FSurface(Px, W, H);
    }

    void DrawLine(TArray<FColor>& Px, int32 W, int32 H, int32 X0, int32 Y0, int32 X1, int32 Y1,
        int32 Thickness, const FColor& C)
    {
        PinWrightBitmapPaint::DrawLine(Surface(Px, W, H), X0, Y0, X1, Y1, Thickness, C);
    }

    // Text label (dark backing plate + glyphs) whose ANCHOR is the top-left of the TEXT; the
    // plate extends one font-pixel further out on every side. Letters are case-folded.
    void DrawLabel(TArray<FColor>& Px, int32 W, int32 H, const FString& Text,
        int32 AnchorX, int32 AnchorY, int32 Scale, const FColor& Ink)
    {
        PinWrightBitmapPaint::FLabelStyle Style;
        Style.Scale = Scale;
        Style.PlatePaddingPx = Scale;
        Style.Anchor = PinWrightBitmapPaint::ELabelAnchor::TextTopLeft;
        Style.Ink = Ink;
        Style.Plate = GLabelBadge;
        Style.bDrawPlate = true;
        PinWrightBitmapPaint::DrawLabel(Surface(Px, W, H), Text, AnchorX, AnchorY, Style);
    }

    // A projected world point in top-left pixel space.
    struct FProjectedPoint
    {
        int32 X = 0;
        int32 Y = 0;
        bool bBehind = true; // point is behind the camera plane -> pixel unreliable
        bool bOk = false;    // the view could be built and the point projected
    };

    // Projects one world point through the ALREADY-BUILT, pose-coherent view the caller opened
    // with the SAME request the capture used, so pixels align with the rendered PNG. An invalid
    // session (headless host, no active Level Editor viewport) yields bOk=false for every point and
    // the overlays simply do not paint - the same degradation the per-point projector produced,
    // reached in one check instead of one failed view rebuild per point.
    FProjectedPoint Project(const PinWrightViewProjection::FViewProjectionSession& Session,
        const FVector& WorldPoint)
    {
        FProjectedPoint Out;
        float PixelX = 0.0f;
        float PixelY = 0.0f;
        bool bBehind = false;
        if (Session.Project(WorldPoint, PixelX, PixelY, bBehind))
        {
            Out.X = FMath::RoundToInt(PixelX);
            Out.Y = FMath::RoundToInt(PixelY);
            Out.bBehind = bBehind;
            Out.bOk = true;
        }
        return Out;
    }

    // Draws a segment between two projected points, skipping any element flagged as
    // behind the camera (MVP: no near-plane clipping, so a behind-camera endpoint drops
    // the whole segment rather than producing a wrapped-around pixel).
    void DrawSegment(TArray<FColor>& Px, int32 W, int32 H,
        const FProjectedPoint& A, const FProjectedPoint& B, int32 Thickness, const FColor& C)
    {
        if (!A.bOk || !B.bOk || A.bBehind || B.bBehind)
        {
            return;
        }
        DrawLine(Px, W, H, A.X, A.Y, B.X, B.Y, Thickness, C);
    }
}

// ---- render.capture_annotated ----
REGISTER_RPC_HANDLER("render.capture_annotated", "render",
    "Capture a viewport - the active Level Editor one, or an asset editor's preview when 'subject' names an asset - then paint measured overlays onto the PNG: world X/Y/Z axes at the origin, a Z=0 ground grid, and per-actor AABB wireframes with world-size labels. Gives an agent a metrically-annotated view instead of a bare screenshot.",
    RPC_PARAMS(
        RPC_PARAM_OPT("filename", "filepath", "Output filename inside Saved/Screenshots/AnnotatedCapture. The '.png' extension is appended if missing."),
        RPC_PARAM_OPT("width", "number", "Output width in pixels. Default 768."),
        RPC_PARAM_OPT("height", "number", "Output height in pixels. Default 768."),
        RPC_PARAM_OPT("location", "object", "Level viewport camera location {x, y, z} in cm."),
        RPC_PARAM_OPT("rotation", "object", "Level viewport camera rotation {pitch, yaw, roll} in degrees."),
        RPC_PARAM_OPT("projectionMode", "string", "'perspective' (default) or 'orthographic'. Orthographic requires a rotation that looks along a world axis (e.g. pitch -90 for top-down)."),
        RPC_PARAM_OPT("fov", "number", "Perspective field of view in degrees. Default 50."),
        FParamSpec{TEXT("orthoWidth"), TEXT("number"),
            TEXT("Orthographic frame width in WORLD CENTIMETRES (the world span the image covers left to right). Default 2000. Alias: orthoWorldWidth."),
            false, TEXT("2000"), TArray<FString>({TEXT("orthoWorldWidth")})},
        RPC_PARAM_OPT("viewDistanceScale", "number", "Force r.ViewDistanceScale for this capture and restore it afterwards. Must be > 0. Omit on an orthographic capture to derive it from the scene so distant foliage is not culled; omit on a perspective capture to leave the cvar alone."),
        RPC_PARAM_OPT("exposure", "object|number", PINWRIGHT_EXPOSURE_PARAM_DESC),
        RPC_PARAM_OPT("hideEditorSprites", "boolean", PINWRIGHT_HIDE_EDITOR_SPRITES_PARAM_DESC),
        RPC_PARAM_OPT("viewMode", "string", PINWRIGHT_VIEW_MODE_PARAM_DESC),
        RPC_PARAM_OPT("previewScene", "object", PINWRIGHT_PREVIEW_SCENE_PARAM_DESC " ASSET SUBJECTS ONLY on this verb. It captures the Level Editor viewport unless 'subject' names an asset kind, and a level viewport has no preview scene to rig - a level is lit by its own actors. Passing it with a world/actor subject, or with no subject at all, is refused with UNSUPPORTED_ASSET_EDITOR rather than accepted and quietly ignored."),
        RPC_PARAM_OPT("subject", "object",
            "What this frame is OF - and, for an asset kind, WHICH VIEWPORT it is taken from. Fields: kind ('world' | 'actor' | 'staticMesh' | 'skeletalMesh' | 'animation' | 'niagara'; inferred from the one present key when omitted), name (actor kind - the same display-label / object-name / object-path spellings the bounds array accepts), path (asset kinds), animation (skeletalMesh kind), point ({x,y,z}) and radius (cm) for the world kind, closeAfterCapture (asset kinds). Omit it and the response carries no subject and no framing block, exactly as before. A world or actor subject annotates the level viewport. An ASSET subject opens (or reuses) that asset's editor and BOTH the frame and the overlay projection come from its preview viewport, so the painted axes and grid measure the preview scene - where the asset sits at the origin - and not the open level. The two LEVEL-ONLY overlays are refused rather than drawn against the wrong world: 'bounds' and 'actorLabels' name level actors, which a preview scene does not contain, so combining either with an asset subject is INVALID_ARGUMENT."),
        RPC_PARAM_OPT("axes", "boolean", "Draw the world X/Y/Z axes at the origin (red/green/blue). Default true."),
        RPC_PARAM_OPT_NESTED("grid", "object", "Ground grid on the Z=0 plane: { spacing: cm between lines, extent: cm half-size from origin }. Omit to skip the grid. These two keys are the whole schema and any other key inside 'grid' is refused with UNKNOWN_NESTED_PARAMS - the brace above is the contract, not an example.",
            TEXT("spacing"), TEXT("extent")),
        RPC_PARAM_OPT("bounds", "array", "Actor names (display label / internal object name / object path) to outline with a projected world AABB wireframe."),
        RPC_PARAM_OPT("labels", "boolean", "Draw text labels: axis letters, grid spacing, and each actor's name + world size in cm. Default true."),
        RPC_PARAM_OPT("actorLabels", "object",
            "OPT-IN actor discovery. Omit for the previous behavior. When present, every actor matching the filters is projected to screen space, ranked by visible size, labelled on the image, and returned as a machine-readable actor->pixel map under the response's actorLabels.actors[]. Fields: folder (outliner folder PREFIX, case-insensitive), tag (exact), className (short name or path, subclasses included; alias class), filter (name/label substring, or wildcard when it contains * or ?), minScreenArea (px^2 floor, default 256), maxLabels (cap, default 50, 0 = uncapped up to 500), minSpacing (px between painted anchors, default 24), draw (boolean, default true - false returns the map without painting). Pass true as shorthand for {}. Actors are ranked by visible projected area, so the cap keeps the most prominent ones; drops are reported in actorLabels.dropped."),
        RPC_PARAM_OPT("inline", "boolean", "When true, also embed base64 PNG bytes of the annotated image in a 'base64' field. Default false.")
    ))
{
    using namespace PinWrightRenderCapture;
    using namespace PinWrightAnnotatedCapture;

    // 1) Parse + validate the base capture request (dims, projection mode, fov, orthoWidth).
    FViewportCaptureRequest Request;
    FString ErrCode;
    FString ErrMsg;
    if (!ParseViewportCaptureRequest(Ctx.GetRawPayload(), Request, ErrCode, ErrMsg))
    {
        Ctx.SendError(ErrCode, ErrMsg);
        return true;
    }

    const bool bAxes = Ctx.GetBool(TEXT("axes"), true);
    const bool bLabels = Ctx.GetBool(TEXT("labels"), true);
    const bool bInline = Ctx.GetBool(TEXT("inline"), false);

    // Grid params (optional object). Validate spacing/extent when supplied.
    const TSharedPtr<FJsonObject> GridObj = Ctx.GetObject(TEXT("grid"));
    const bool bGrid = GridObj.IsValid();
    double GridSpacing = 100.0;
    double GridExtent = 1000.0;
    if (bGrid)
    {
        GridObj->TryGetNumberField(TEXT("spacing"), GridSpacing);
        GridObj->TryGetNumberField(TEXT("extent"), GridExtent);
        if (GridSpacing <= 0.0 || GridExtent <= 0.0)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("grid.spacing and grid.extent must be greater than zero"));
            return true;
        }
    }

    // Bounds targets (optional array of actor names).
    TArray<FString> BoundsNames;
    if (const TArray<TSharedPtr<FJsonValue>>* BoundsArr = Ctx.GetArray(TEXT("bounds")))
    {
        for (const TSharedPtr<FJsonValue>& Val : *BoundsArr)
        {
            FString Name;
            if (Val.IsValid() && Val->TryGetString(Name) && !Name.IsEmpty())
            {
                BoundsNames.Add(Name);
            }
        }
    }

    // Actor-label discovery overlay (optional object, opt-in by PRESENCE - the same contract the
    // `grid` param uses above). Absent => this whole feature is inert and both the painted pixels
    // and the response JSON are byte-identical to a build without it.
    const TSharedPtr<FJsonObject>& RawPayload = Ctx.GetRawPayload();
    TSharedPtr<FJsonObject> ActorLabelsObj = Ctx.GetObject(TEXT("actorLabels"));
    bool bActorLabels = ActorLabelsObj.IsValid();
    if (!bActorLabels && RawPayload.IsValid() &&
        RawPayload->HasTypedField<EJson::Boolean>(TEXT("actorLabels")))
    {
        // `actorLabels: true` is shorthand for `actorLabels: {}` (all defaults); false is off.
        bActorLabels = RawPayload->GetBoolField(TEXT("actorLabels"));
        if (bActorLabels)
        {
            ActorLabelsObj = MakeShared<FJsonObject>();
        }
    }

    PinWrightActorLabels::FActorLabelOptions LabelOptions;
    UClass* LabelClassFilter = nullptr;
    if (bActorLabels)
    {
        ActorLabelsObj->TryGetStringField(TEXT("folder"), LabelOptions.Folder);
        ActorLabelsObj->TryGetStringField(TEXT("tag"), LabelOptions.Tag);
        if (!ActorLabelsObj->TryGetStringField(TEXT("className"), LabelOptions.ClassName))
        {
            // Same className / class alias pair actor.find_by_class accepts.
            ActorLabelsObj->TryGetStringField(TEXT("class"), LabelOptions.ClassName);
        }
        ActorLabelsObj->TryGetStringField(TEXT("filter"), LabelOptions.Filter);

        double NumberValue = 0.0;
        if (ActorLabelsObj->TryGetNumberField(TEXT("minScreenArea"), NumberValue))
        {
            LabelOptions.MinScreenAreaPx = NumberValue;
        }
        if (ActorLabelsObj->TryGetNumberField(TEXT("maxLabels"), NumberValue))
        {
            LabelOptions.MaxLabels = FMath::RoundToInt(NumberValue);
        }
        if (ActorLabelsObj->TryGetNumberField(TEXT("minSpacing"), NumberValue))
        {
            LabelOptions.MinLabelSpacingPx = FMath::RoundToInt(NumberValue);
        }
        bool bDrawLabels = true;
        if (ActorLabelsObj->TryGetBoolField(TEXT("draw"), bDrawLabels))
        {
            LabelOptions.bDraw = bDrawLabels;
        }
        // `labels:false` suppresses every painted string in this call. The actor MAP is data, not
        // paint, so it is still returned - which is also the cheapest way to ask for the map alone.
        LabelOptions.bDraw = LabelOptions.bDraw && bLabels;

        if (LabelOptions.MinScreenAreaPx < 0.0 || LabelOptions.MinLabelSpacingPx < 0)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("actorLabels.minScreenArea and actorLabels.minSpacing must not be negative"));
            return true;
        }

        if (!LabelOptions.ClassName.IsEmpty())
        {
            // Resolve BEFORE the capture: an unresolvable class must be a typed CLASS_NOT_FOUND
            // that writes no PNG, not a silently empty actor list. Same helper and same message
            // actor.find_by_class uses, so one class string behaves identically in both verbs.
            LabelClassFilter = ResolveUClass(LabelOptions.ClassName);
            if (!LabelClassFilter)
            {
                Ctx.SendError(ErrorCodes::ERR_CLASS_NOT_FOUND, FString::Printf(
                    TEXT("Could not resolve actorLabels.className '%s'. Pass a short class name ")
                    TEXT("(e.g. 'StaticMeshActor') or a full asset/script path ")
                    TEXT("(e.g. '/Script/Engine.StaticMeshActor', '/Game/Foo/BP_Bar')."),
                    *LabelOptions.ClassName));
                return true;
            }
        }
    }

    // 1c) OPTIONAL subject: what this frame is supposed to be OF. Opt-in, like `grid` and
    // `actorLabels` - name no subject and the whole feature is inert and the response is
    // byte-identical to a build without it (no `subject` key, no `framing` key).
    //
    // What it buys is the one question the overlays cannot answer: axes, grid and AABB wireframes
    // are painted from the requested pose whether or not the thing the caller cares about projects
    // into the frame, and `blank` only catches BLACK frames - so a brightly lit picture of empty
    // backdrop comes back a clean success today. The `framing` verdict in step 6 is measured
    // against the EFFECTIVE pose, so it also catches a viewport that ignored the aim.
    //
    // EVERY KIND IS SERVED. The asset kinds used to be a blanket UNSUPPORTED_ASSET_EDITOR refusal,
    // because FViewProjectionSession rebuilt its view from the active Level Editor viewport by
    // construction and took no client argument - so an overlay painted over an asset-editor preview
    // frame would have been projected for a viewport that frame was never drawn in. The session now
    // takes an FViewProjectionTarget (ViewProjectionUtils.h) and step 2 hands it the SAME client the
    // capture rendered through, which is what makes an asset subject correct rather than plausible.
    //
    // WHAT STAYS REFUSED, and it is an argument combination rather than a kind: `bounds` and
    // `actorLabels` are LEVEL overlays. Both resolve names against the editor world - McpActorUtils
    // ::FindActorByName and TActorIterator(EditorWorld, ...) - and an asset preview is an
    // FPreviewScene holding registered components with no AActor of the level in it, so the honest
    // outcomes would be "every name reported found:false" and "an empty actor map". Either reads as
    // "your level has nothing in it" rather than "you asked the wrong world", so the pair is a typed
    // refusal instead (rpc-design.md: a capability a subject cannot support is an answer, not a
    // pretence). No new error code: this is a payload whose two halves contradict each other, which
    // is what INVALID_ARGUMENT already means everywhere else in this file.
    //
    // Parsed and refused HERE, beside the other argument validation and before anything is
    // acquired or rendered, for the same reason the actorLabels className is resolved here: a
    // refusal must write no PNG.
    //
    // The opt-in test is SubjectRequest.bProvided rather than a HasField probe on the payload:
    // ParseSubject also normalises the legacy top-level spellings into the same request, so it is
    // the one place that knows which keys count as naming a subject. It returns true with
    // Kind=World and bProvided=false when the payload names none - this verb's behaviour to date.
    PinWrightCaptureSubject::FSubjectRequest SubjectRequest;
    if (!PinWrightCaptureSubject::ParseSubject(RawPayload, SubjectRequest, ErrCode, ErrMsg))
    {
        Ctx.SendError(ErrCode, ErrMsg);
        return true;
    }
    // Everything that is not the level itself or an actor in it comes from an asset editor's
    // preview viewport instead of the Level Editor one. One predicate, used by every branch below,
    // so "which viewport" is decided once.
    const bool bAssetSubject = SubjectRequest.bProvided &&
        SubjectRequest.Kind != PinWrightCaptureSubject::ESubjectKind::World &&
        SubjectRequest.Kind != PinWrightCaptureSubject::ESubjectKind::Actor;

    if (bAssetSubject && (BoundsNames.Num() > 0 || bActorLabels))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
            TEXT("'%s' cannot be combined with a '%s' subject: it names LEVEL actors, and an ")
            TEXT("asset-editor preview scene contains none - every name would come back ")
            TEXT("found:false and the actor map would be empty, which reads as an empty level ")
            TEXT("rather than as the wrong world. Capture the asset without %s, or drop the ")
            TEXT("subject to annotate the level viewport instead."),
            BoundsNames.Num() > 0 ? TEXT("bounds") : TEXT("actorLabels"),
            PinWrightCaptureSubject::ToWireName(SubjectRequest.Kind),
            BoundsNames.Num() > 0 ? TEXT("bounds") : TEXT("actorLabels")));
        return true;
    }

    // The mirror of the refusal above, in the other direction and for the same reason: a
    // parameter is being asked for against the world it cannot reach.
    //
    // `previewScene` rigs the key light, sky and backdrop of an asset editor's
    // FAdvancedPreviewScene. The Level Editor viewport has no FPreviewScene at all --
    // FLevelEditorViewportClient passes nullptr for it (UE 5.8
    // Editor/UnrealEd/Private/LevelEditorViewport.cpp:2335), so GetPreviewScene() is null there --
    // and a level is lit by its own actors. There is nothing to write and nothing to restore.
    //
    // REFUSED here, CLEARED in render.capture_open_level, and the difference is not an
    // inconsistency. There the parameter is undeclared, so the dispatcher's unknown-param gate is
    // what answers the caller and the clear only covers direct invocation. Here it is a declared,
    // reachable parameter on a verb that serves both worlds: accepting it and silently doing
    // nothing is the "call succeeded, nothing happened" shape this parameter exists to remove.
    //
    // UNSUPPORTED_ASSET_EDITOR rather than a new code (plan decision 6): the request named a
    // viewport that has no advanced preview scene, which is exactly what that code says.
    if (!bAssetSubject && Request.PreviewSceneRig.bRequested)
    {
        Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR,
            TEXT("'previewScene' rigs the key light, sky and backdrop of an ASSET EDITOR preview "
                 "scene, and this call annotates the Level Editor viewport, which has none - a "
                 "level is lit by the actors in it. Name an asset subject (subject: {kind: "
                 "'staticMesh', path: '/Game/...'}) to rig the preview viewport that opens, or "
                 "drop 'previewScene' to annotate the level as it is already lit."));
        return true;
    }

    if (!GEditor)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
        return true;
    }
    UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
    // An asset capture never touches the editor world: it renders a preview scene, its overlays are
    // projected against that scene, and the two level-world overlays were refused above. Requiring
    // a loaded level for it would be a level-domain assumption dressed up as a precondition.
    if (!EditorWorld && !bAssetSubject)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_EDITOR_WORLD, TEXT("No active editor world"));
        return true;
    }

    // 2) Acquire the viewport, and the subject that may choose it.
    //
    // THE CAPTURE AND THE OVERLAY PROJECTION SHARE ONE CLIENT. That is the invariant this step
    // exists to make structural: `ViewportClient` / `SceneViewport` are read once here and then
    // handed BOTH to CaptureEditorViewportToPng and to the FViewProjectionSession below (through
    // FViewProjectionTarget), so there is no arrangement of arguments that can render one frame and
    // annotate a different one.
    //
    // The subject is resolved BEFORE the level viewport is looked up on the asset path, because on
    // that path the level viewport is not needed at all and demanding one would refuse a perfectly
    // capturable asset on a host whose level viewport is closed. On the level path the order is
    // unchanged: viewport first, then the subject, so its typed errors arrive in the order they
    // always have.
    PinWrightCaptureSubject::FResolvedSubject ResolvedSubject;
    bool bSubjectResolved = false;
    FEditorViewportClient* ViewportClient = nullptr;
    TSharedPtr<FSceneViewport> SceneViewport;

    // This verb takes no time argument, so the setter is never invoked; a kind with no time axis
    // therefore never reaches its refusal through this call.
    PinWrightCaptureSubject::FSubjectTimeSetter SubjectTimeSetter;

    if (bAssetSubject)
    {
        if (!PinWrightCaptureSubject::Resolve(SubjectRequest, ResolvedSubject, SubjectTimeSetter,
            ErrCode, ErrMsg))
        {
            Ctx.SendError(ErrCode, ErrMsg);
            return true;
        }
        bSubjectResolved = true;
        // The resolver's own contract: never null / never invalid on success. Checked anyway, since
        // everything below dereferences it and a broken provider must not be a crash.
        if (!ResolvedSubject.ViewportClient || !ResolvedSubject.SceneViewport.IsValid())
        {
            Ctx.SendError(ErrorCodes::ERR_PREVIEW_VIEWPORT_NOT_FOUND, FString::Printf(
                TEXT("The '%s' subject resolved without a preview viewport, so there is nothing to ")
                TEXT("capture or to project overlays against."),
                PinWrightCaptureSubject::ToWireName(SubjectRequest.Kind)));
            return true;
        }
        ViewportClient = ResolvedSubject.ViewportClient;
        SceneViewport = ResolvedSubject.SceneViewport;
    }
    else
    {
        // The active Level Editor viewport (identical path to render.capture_open_level).
        FLevelEditorModule* LevelEditorModule = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
        if (!LevelEditorModule)
        {
            Ctx.SendError(ErrorCodes::ERR_NO_ACTIVE_LEVEL_VIEWPORT, TEXT("LevelEditor module is not loaded"));
            return true;
        }
        TSharedPtr<IAssetViewport> ActiveViewport = LevelEditorModule->GetFirstActiveViewport();
        if (!ActiveViewport.IsValid())
        {
            Ctx.SendError(ErrorCodes::ERR_NO_ACTIVE_LEVEL_VIEWPORT, TEXT("No active Level Editor viewport"));
            return true;
        }
        ViewportClient = &ActiveViewport->GetAssetViewportClient();
        SceneViewport = ActiveViewport->GetSharedActiveViewport();
        if (!SceneViewport.IsValid())
        {
            Ctx.SendError(ErrorCodes::ERR_NO_ACTIVE_LEVEL_VIEWPORT, TEXT("Active Level Editor viewport has no SceneViewport"));
            return true;
        }

        // 2b) Acquire the subject parsed in step 1c. On this path it supplies bounds and identity
        // only: the world and actor providers resolve to this same active Level Editor viewport, so
        // there is nothing to re-point.
        if (SubjectRequest.bProvided)
        {
            if (!PinWrightCaptureSubject::Resolve(SubjectRequest, ResolvedSubject, SubjectTimeSetter,
                ErrCode, ErrMsg))
            {
                Ctx.SendError(ErrCode, ErrMsg);
                return true;
            }
            bSubjectResolved = true;
        }
    }

    // 3) Base capture. An orthographic pose must look along a world axis (the capture util maps it
    // onto the matching ELevelViewportType); a tilted orthographic pose is rejected there with
    // UNSUPPORTED_ORTHOGRAPHIC_ROTATION. The overlay projection below reuses `Request` AND the same
    // client through the same helper, so the painted geometry lands on the same pixels the base
    // frame rendered.
    //
    // VIEW DISTANCE, on the level path only. A wide orthographic frame of a LEVEL culls its distant
    // instanced geometry, which would leave the annotated overlay measured against a frame missing
    // the very content it annotates - the same handling render.capture_open_level gets. A preview
    // scene has no distance-culled content (its components carry no LDMaxDrawDistance), so forcing
    // the global r.ViewDistanceScale there would mutate editor-wide state for no pixel change;
    // render.capture_asset_preview refuses the parameter outright for that reason
    // (RenderHandler.cpp, bAutoViewDistanceScale = false).
    Request.bAutoViewDistanceScale = !bAssetSubject;
    FViewportCaptureOutput Capture;
    if (!CaptureEditorViewportToPng(*ViewportClient, SceneViewport, Request,
        TEXT("Annotated"), TEXT("AnnotatedCapture"), Capture, ErrCode, ErrMsg))
    {
        Ctx.SendError(ErrCode, ErrMsg);
        return true;
    }

    // 4) Load the base PNG back into an editable FColor buffer (the capture util only
    // hands back a path, not pixels). Mirrors the test-side loader in TestRenderHandlers.
    FImage Loaded;
    if (!FImageUtils::LoadImage(*Capture.Path, Loaded))
    {
        // The base file exists on disk (capture just wrote it) but could not be decoded.
        IFileManager::Get().Delete(*Capture.Path, false, true);
        Ctx.SendError(ErrorCodes::ERR_DECODE_FAILED,
            FString::Printf(TEXT("Failed to decode base capture for annotation: %s"), *Capture.Path));
        return true;
    }
    Loaded.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
    const int32 W = Loaded.SizeX;
    const int32 H = Loaded.SizeY;
    TArrayView64<FColor> SrcView = Loaded.AsBGRA8();
    if (W <= 0 || H <= 0 || SrcView.Num() < static_cast<int64>(W) * H)
    {
        IFileManager::Get().Delete(*Capture.Path, false, true);
        Ctx.SendError(ErrorCodes::ERR_DECODE_FAILED, TEXT("Decoded base capture had no pixels"));
        return true;
    }
    TArray<FColor> Pixels;
    Pixels.Append(SrcView.GetData(), static_cast<int32>(static_cast<int64>(W) * H));

    // ---- Paint the overlays. Every projection reuses `Request` for pose coherence. ----
    TSharedPtr<FJsonObject> Overlays = MakeShared<FJsonObject>();
    // Filled by the actorLabels pass below; attached to the response in step 6.
    TSharedPtr<FJsonObject> ActorLabelsResult;

    // ONE rebuilt view for the whole paint pass (axes, grid, AABB corners, actor labels), against
    // THE CLIENT THE CAPTURE JUST RENDERED THROUGH. Passing the target explicitly - rather than
    // letting the session resolve the active Level Editor viewport for itself, as it did when the
    // level was the only path - is what keeps the painted geometry on the pixels of this frame:
    // a session pointed anywhere else still produces a plausible PNG, which is exactly why the
    // pairing must be structural instead of documented.
    //
    // Held through a TUniquePtr, and a reference is taken so the painting code below reads exactly
    // as it did when this was a plain local. The indirection buys ONE thing: the session can be
    // closed at a chosen point rather than at the end of the handler, which is what lets the asset
    // editor be closed before the response is built (see the release below). One session for the
    // whole pass either way - rebuilding the view is the expensive part, and this is strictly less
    // viewport churn than the save/apply/restore per projected point it replaced.
    TUniquePtr<PinWrightViewProjection::FViewProjectionSession> SessionHolder =
        MakeUnique<PinWrightViewProjection::FViewProjectionSession>(Request,
            PinWrightViewProjection::FViewProjectionTarget(*ViewportClient, SceneViewport));
    const PinWrightViewProjection::FViewProjectionSession& Session = *SessionHolder;

    // Axis length: span the (clamped) grid when a grid is present, else a default 5 m.
    const double DrawnGridExtent = bGrid
        ? FMath::Min(GridExtent, GMaxGridHalfLines * GridSpacing)
        : 0.0;
    const float AxisLen = static_cast<float>(bGrid ? FMath::Max(DrawnGridExtent, 100.0) : 500.0);

    // Axes: origin + endpoints along +X/+Y/+Z.
    Overlays->SetBoolField(TEXT("axes"), bAxes);
    if (bAxes)
    {
        const FProjectedPoint O = Project(Session, FVector::ZeroVector);
        const FProjectedPoint PX = Project(Session, FVector(AxisLen, 0, 0));
        const FProjectedPoint PY = Project(Session, FVector(0, AxisLen, 0));
        const FProjectedPoint PZ = Project(Session, FVector(0, 0, AxisLen));
        DrawSegment(Pixels, W, H, O, PX, GAxisThickness, GAxisXColor);
        DrawSegment(Pixels, W, H, O, PY, GAxisThickness, GAxisYColor);
        DrawSegment(Pixels, W, H, O, PZ, GAxisThickness, GAxisZColor);
        if (bLabels)
        {
            if (PX.bOk && !PX.bBehind) { DrawLabel(Pixels, W, H, TEXT("X"), PX.X + 4, PX.Y - GGlyphH * GLabelScale / 2, GLabelScale, GAxisXColor); }
            if (PY.bOk && !PY.bBehind) { DrawLabel(Pixels, W, H, TEXT("Y"), PY.X + 4, PY.Y - GGlyphH * GLabelScale / 2, GLabelScale, GAxisYColor); }
            if (PZ.bOk && !PZ.bBehind) { DrawLabel(Pixels, W, H, TEXT("Z"), PZ.X + 4, PZ.Y - GGlyphH * GLabelScale / 2, GLabelScale, GAxisZColor); }
        }
    }

    // Grid: lines parallel to X and to Y on the Z=0 plane, centered on the origin.
    if (bGrid)
    {
        const int32 HalfLines = FMath::Min(GMaxGridHalfLines,
            FMath::FloorToInt(static_cast<float>(GridExtent / GridSpacing)));
        const double Span = HalfLines * GridSpacing; // clamped drawn half-extent
        int32 LinesDrawn = 0;
        for (int32 I = -HalfLines; I <= HalfLines; ++I)
        {
            const double Coord = I * GridSpacing;
            // Line parallel to Y (constant X = Coord).
            {
                const FProjectedPoint A = Project(Session, FVector(Coord, -Span, 0));
                const FProjectedPoint B = Project(Session, FVector(Coord, Span, 0));
                if (A.bOk && B.bOk && !A.bBehind && !B.bBehind)
                {
                    DrawLine(Pixels, W, H, A.X, A.Y, B.X, B.Y, GGridThickness, GGridColor);
                    ++LinesDrawn;
                }
            }
            // Line parallel to X (constant Y = Coord).
            {
                const FProjectedPoint A = Project(Session, FVector(-Span, Coord, 0));
                const FProjectedPoint B = Project(Session, FVector(Span, Coord, 0));
                if (A.bOk && B.bOk && !A.bBehind && !B.bBehind)
                {
                    DrawLine(Pixels, W, H, A.X, A.Y, B.X, B.Y, GGridThickness, GGridColor);
                    ++LinesDrawn;
                }
            }
        }
        if (bLabels)
        {
            DrawLabel(Pixels, W, H,
                FString::Printf(TEXT("GRID %dCM"), FMath::RoundToInt(static_cast<float>(GridSpacing))),
                6, 6, GLabelScale, GLabelInk);
        }

        TSharedPtr<FJsonObject> GridEcho = MakeShared<FJsonObject>();
        GridEcho->SetNumberField(TEXT("spacing"), GridSpacing);
        GridEcho->SetNumberField(TEXT("extent"), GridExtent);
        GridEcho->SetNumberField(TEXT("drawnExtent"), Span);
        GridEcho->SetNumberField(TEXT("linesDrawn"), LinesDrawn);
        GridEcho->SetBoolField(TEXT("clamped"), Span < GridExtent - KINDA_SMALL_NUMBER);
        Overlays->SetObjectField(TEXT("grid"), GridEcho);
    }

    // Bounds: an axis-aligned world AABB wireframe (12 edges) + name/size label per actor.
    // The 12 edges of the box connecting the 8 corners (indexed by the sign bits of x,y,z).
    static const int32 EdgePairs[12][2] =
    {
        {0,1},{0,2},{0,4},{1,3},{1,5},{2,3},{2,6},{3,7},{4,5},{4,6},{5,7},{6,7}
    };
    TArray<TSharedPtr<FJsonValue>> BoundsEcho;
    for (const FString& Name : BoundsNames)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Name);

        AActor* Actor = McpActorUtils::FindActorByName(EditorWorld, Name);
        if (!Actor)
        {
            Entry->SetBoolField(TEXT("found"), false);
            BoundsEcho.Add(MakeShared<FJsonValueObject>(Entry));
            continue;
        }
        Entry->SetBoolField(TEXT("found"), true);

        FVector Origin = FVector::ZeroVector;
        FVector Extent = FVector::ZeroVector;
        Actor->GetActorBounds(false, Origin, Extent);

        // 8 corners; corner index bit0=+X, bit1=+Y, bit2=+Z.
        FProjectedPoint Corners[8];
        for (int32 C = 0; C < 8; ++C)
        {
            const FVector Corner(
                Origin.X + ((C & 1) ? Extent.X : -Extent.X),
                Origin.Y + ((C & 2) ? Extent.Y : -Extent.Y),
                Origin.Z + ((C & 4) ? Extent.Z : -Extent.Z));
            Corners[C] = Project(Session, Corner);
        }
        for (const int32(&Pair)[2] : EdgePairs)
        {
            DrawSegment(Pixels, W, H, Corners[Pair[0]], Corners[Pair[1]], GBoxThickness, GBoundsColor);
        }

        // Size echo (full world size = 2 * half-extent, in cm).
        const FVector Size = Extent * 2.0;
        TSharedPtr<FJsonObject> SizeObj = MakeShared<FJsonObject>();
        SizeObj->SetNumberField(TEXT("x"), Size.X);
        SizeObj->SetNumberField(TEXT("y"), Size.Y);
        SizeObj->SetNumberField(TEXT("z"), Size.Z);
        Entry->SetObjectField(TEXT("sizeCm"), SizeObj);

        // Label at the topmost (smallest screen Y) in-front corner.
        if (bLabels)
        {
            int32 AnchorX = 0;
            int32 AnchorY = 0;
            bool bHaveAnchor = false;
            for (const FProjectedPoint& P : Corners)
            {
                if (P.bOk && !P.bBehind && (!bHaveAnchor || P.Y < AnchorY))
                {
                    AnchorX = P.X;
                    AnchorY = P.Y;
                    bHaveAnchor = true;
                }
            }
            if (bHaveAnchor)
            {
                const int32 LineH = GGlyphH * GLabelScale + 2 * GLabelScale;
                DrawLabel(Pixels, W, H, Name.ToUpper(), AnchorX, AnchorY - 2 * LineH, GLabelScale, GBoundsColor);
                DrawLabel(Pixels, W, H,
                    FString::Printf(TEXT("%dX%dX%dCM"),
                        FMath::RoundToInt(static_cast<float>(Size.X)),
                        FMath::RoundToInt(static_cast<float>(Size.Y)),
                        FMath::RoundToInt(static_cast<float>(Size.Z))),
                    AnchorX, AnchorY - LineH, GLabelScale, GLabelInk);
            }
        }
        BoundsEcho.Add(MakeShared<FJsonValueObject>(Entry));
    }
    if (BoundsNames.Num() > 0)
    {
        Overlays->SetArrayField(TEXT("bounds"), BoundsEcho);
    }

    // Actor labels: the opt-in discovery overlay. Projects every filtered actor's bounds through
    // the SAME session the geometry above used, ranks by visible screen area, caps, paints the
    // survivors, and returns the actor->pixel map. See ActorLabelOverlay.h for the pure half.
    if (bActorLabels)
    {
        using namespace PinWrightActorLabels;

        TArray<FActorLabelCandidate> Candidates;
        int32 ScanCeilingSkipped = 0;

        // The frame rect every projected box is clamped against.
        const FBox2D FrameRect(FVector2D::ZeroVector,
            FVector2D(static_cast<double>(W), static_cast<double>(H)));

        // A class filter becomes the iterator's own class, so the engine skips non-matching actors
        // without materializing them - the same TActorIterator(World, Class) form actor.find_by_class
        // uses. No `world` param here: this verb annotates the EDITOR viewport it just captured, so
        // the editor world is the only pose-coherent choice.
        if (Session.IsValid())
        {
            for (TActorIterator<AActor> It(EditorWorld,
                     LabelClassFilter ? LabelClassFilter : AActor::StaticClass()); It; ++It)
            {
                AActor* Actor = *It;
                if (!Actor)
                {
                    continue;
                }

                // Folder: outliner PREFIX match, case-insensitive, so "Blockout/Towers" also
                // catches "Blockout/Towers/North". AActor::GetFolderPath() is stable across
                // UE 5.3-5.8 and needs no version guard (see ActorDescribeBuilder.cpp:155).
                if (!LabelOptions.Folder.IsEmpty() &&
                    !Actor->GetFolderPath().ToString().StartsWith(
                        LabelOptions.Folder, ESearchCase::IgnoreCase))
                {
                    continue;
                }
                // Tag: exact FName equality, matching actor.find_by_tag's default matchType.
                if (!LabelOptions.Tag.IsEmpty() && !Actor->ActorHasTag(FName(*LabelOptions.Tag)))
                {
                    continue;
                }

                const FString ActorName = Actor->GetName();
                const FString ActorLabel = Actor->GetActorLabel();
                if (!MatchesNameFilter(ActorName, ActorLabel, LabelOptions.Filter))
                {
                    continue;
                }

                if (Candidates.Num() >= MaxProjectedCandidates)
                {
                    // Pathological world: stop projecting, keep counting, report the shortfall.
                    ++ScanCeilingSkipped;
                    continue;
                }

                // BOUNDS CENTRE, not GetActorLocation()'s pivot - the same accessor and the same
                // bOnlyCollidingComponents=false the `bounds` overlay above uses. A floor slab's
                // pivot can sit metres from its visual centre, which would anchor the label off the
                // thing it names; and including non-colliding components is what lets this verb see
                // actors spatial.raycast_screen's physics trace never can.
                FVector Origin = FVector::ZeroVector;
                FVector Extent = FVector::ZeroVector;
                Actor->GetActorBounds(false, Origin, Extent);

                FActorLabelCandidate Candidate;
                Candidate.Name = ActorName;
                Candidate.Label = ActorLabel;
                Candidate.ClassName = Actor->GetClass() ? Actor->GetClass()->GetName() : FString();
                Candidate.Folder = Actor->GetFolderPath().ToString();
                Candidate.Distance = FVector::Distance(Request.Location, Origin);
                // Capture.EffectiveRotation, NOT Request.Rotation - see IsBehindCamera's contract.
                Candidate.bBehindCamera =
                    IsBehindCamera(Request.Location, Capture.EffectiveRotation, Origin);

                if (Candidate.bBehindCamera)
                {
                    // Do not spend 9 projections on a point whose pixels would be mirrored garbage;
                    // BuildLayout rejects it on the flag alone.
                    Candidates.Add(MoveTemp(Candidate));
                    continue;
                }

                float CentroidX = 0.0f;
                float CentroidY = 0.0f;
                bool bCentroidBehind = false;
                if (!Session.Project(Origin, CentroidX, CentroidY, bCentroidBehind))
                {
                    continue;
                }
                // Belt and braces: the projector's own W<=0 flag is authoritative for perspective.
                Candidate.bBehindCamera = bCentroidBehind;
                Candidate.CentroidPixelX = CentroidX;
                Candidate.CentroidPixelY = CentroidY;
                Candidate.bCentroidOnScreen =
                    CentroidX >= 0.0f && CentroidY >= 0.0f &&
                    CentroidX < static_cast<float>(W) && CentroidY < static_cast<float>(H);

                // Screen rect from the 8 AABB corners (same corner indexing as the bounds overlay),
                // clamped to the frame. Behind-camera corners are dropped rather than wrapped, the
                // same no-near-plane-clip rule DrawSegment obeys.
                FBox2D ProjectedRect(ForceInit);
                for (int32 C = 0; C < 8; ++C)
                {
                    const FVector Corner(
                        Origin.X + ((C & 1) ? Extent.X : -Extent.X),
                        Origin.Y + ((C & 2) ? Extent.Y : -Extent.Y),
                        Origin.Z + ((C & 4) ? Extent.Z : -Extent.Z));
                    float CornerX = 0.0f;
                    float CornerY = 0.0f;
                    bool bCornerBehind = false;
                    if (Session.Project(Corner, CornerX, CornerY, bCornerBehind) && !bCornerBehind)
                    {
                        ProjectedRect += FVector2D(static_cast<double>(CornerX),
                            static_cast<double>(CornerY));
                    }
                }
                if (ProjectedRect.bIsValid)
                {
                    const FBox2D Clamped = ProjectedRect.Overlap(FrameRect);
                    if (Clamped.bIsValid)
                    {
                        Candidate.ClampedRect = Clamped;
                        Candidate.ScreenArea = Clamped.GetArea();
                    }
                }

                Candidates.Add(MoveTemp(Candidate));
            }
        }

        const FActorLabelLayout Layout = BuildLayout(Candidates, LabelOptions);

        TArray<TSharedPtr<FJsonValue>> LabelRows;
        LabelRows.Reserve(Layout.Placements.Num());
        for (const FActorLabelPlacement& Placement : Layout.Placements)
        {
            const FActorLabelCandidate& Candidate = Candidates[Placement.CandidateIndex];

            if (Placement.bDrawn)
            {
                DrawLabel(Pixels, W, H, Candidate.Name.ToUpper(),
                    Placement.AnchorX, Placement.AnchorY, GLabelScale, GActorLabelColor);
            }

            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            // `name` is the unique internal object name - the collision-safe key for actor.select.
            Row->SetStringField(TEXT("name"), Candidate.Name);
            // `label` is omitted when it equals `name` (the common case for programmatically
            // spawned actors). At ~30 bytes a row over a 50-row default that is a third of the
            // 10,000-char inline budget spent restating the same string.
            if (!Candidate.Label.Equals(Candidate.Name, ESearchCase::CaseSensitive))
            {
                Row->SetStringField(TEXT("label"), Candidate.Label);
            }
            // Short class name (MakeInspectActorRow's convention), not the verbose class path.
            Row->SetStringField(TEXT("class"), Candidate.ClassName);
            if (!Candidate.Folder.IsEmpty())
            {
                Row->SetStringField(TEXT("folder"), Candidate.Folder);
            }
            Row->SetNumberField(TEXT("px"), FMath::RoundToInt(Candidate.CentroidPixelX));
            Row->SetNumberField(TEXT("py"), FMath::RoundToInt(Candidate.CentroidPixelY));
            Row->SetNumberField(TEXT("screenArea"), FMath::RoundToInt(Candidate.ScreenArea));
            Row->SetNumberField(TEXT("distance"), FMath::RoundToInt(Candidate.Distance));
            Row->SetBoolField(TEXT("onScreen"), Candidate.bCentroidOnScreen);
            Row->SetBoolField(TEXT("drawn"), Placement.bDrawn);
            // The visible in-frame rect, emitted ONLY when the centroid projected outside the
            // frame. That is exactly when px/py is not a usable pixel and the caller needs a
            // guaranteed-in-frame point (this rect's centre) to hand to spatial.raycast_screen.
            if (!Candidate.bCentroidOnScreen)
            {
                const FVector2D RectSize = Candidate.ClampedRect.GetSize();
                TSharedPtr<FJsonObject> RectObj = MakeShared<FJsonObject>();
                RectObj->SetNumberField(TEXT("x"), FMath::RoundToInt(Candidate.ClampedRect.Min.X));
                RectObj->SetNumberField(TEXT("y"), FMath::RoundToInt(Candidate.ClampedRect.Min.Y));
                RectObj->SetNumberField(TEXT("w"), FMath::RoundToInt(RectSize.X));
                RectObj->SetNumberField(TEXT("h"), FMath::RoundToInt(RectSize.Y));
                Row->SetObjectField(TEXT("rect"), RectObj);
            }
            LabelRows.Add(MakeShared<FJsonValueObject>(Row));
        }

        ActorLabelsResult = MakeShared<FJsonObject>();
        ActorLabelsResult->SetArrayField(TEXT("actors"), LabelRows);
        // count / totalMatches / truncated is actor.list's vocabulary (QueryHandler.cpp:117-119):
        // count = rows returned, totalMatches = everything that passed every visibility filter
        // BEFORE the cap, truncated flips when the cap elided rows.
        ActorLabelsResult->SetNumberField(TEXT("count"), LabelRows.Num());
        ActorLabelsResult->SetNumberField(TEXT("totalMatches"), Layout.Stats.TotalMatches);
        ActorLabelsResult->SetBoolField(TEXT("truncated"),
            LabelRows.Num() < Layout.Stats.TotalMatches);
        ActorLabelsResult->SetNumberField(TEXT("scanned"), Layout.Stats.Scanned);
        ActorLabelsResult->SetNumberField(TEXT("drawn"), Layout.Stats.Drawn);
        ActorLabelsResult->SetNumberField(TEXT("overlapSkipped"), Layout.Stats.OverlapSkipped);
        // Why the scanned candidates did not all become rows, so "where did my actor go" is
        // answerable from the response alone:
        //   scanned      == behindCamera + offScreen + belowMinScreenArea + totalMatches
        //   totalMatches == count + dropped.cap
        TSharedPtr<FJsonObject> DroppedObj = MakeShared<FJsonObject>();
        DroppedObj->SetNumberField(TEXT("behindCamera"), Layout.Stats.BehindCamera);
        DroppedObj->SetNumberField(TEXT("offScreen"), Layout.Stats.OffScreen);
        DroppedObj->SetNumberField(TEXT("belowMinScreenArea"), Layout.Stats.BelowMinScreenArea);
        DroppedObj->SetNumberField(TEXT("cap"), Layout.Stats.CapDropped);
        ActorLabelsResult->SetObjectField(TEXT("dropped"), DroppedObj);
        if (ScanCeilingSkipped > 0)
        {
            ActorLabelsResult->SetNumberField(TEXT("scanCeilingSkipped"), ScanCeilingSkipped);
        }
        if (!Session.IsValid())
        {
            // Unreachable in practice (the capture above needs the same viewport), but an empty map
            // must never read as "this level has no actors".
            ActorLabelsResult->SetBoolField(TEXT("viewportUnavailable"), true);
        }
        // Effective options, so a defaulted or clamped value is never invisible.
        ActorLabelsResult->SetNumberField(TEXT("maxLabels"), Layout.Stats.ResolvedCap);
        ActorLabelsResult->SetNumberField(TEXT("minScreenArea"), LabelOptions.MinScreenAreaPx);
        ActorLabelsResult->SetNumberField(TEXT("minSpacing"), LabelOptions.MinLabelSpacingPx);
        if (!LabelOptions.Folder.IsEmpty())
        {
            ActorLabelsResult->SetStringField(TEXT("folder"), LabelOptions.Folder);
        }
        if (!LabelOptions.Tag.IsEmpty())
        {
            ActorLabelsResult->SetStringField(TEXT("tag"), LabelOptions.Tag);
        }
        if (!LabelOptions.ClassName.IsEmpty())
        {
            ActorLabelsResult->SetStringField(TEXT("className"), LabelOptions.ClassName);
        }
        if (!LabelOptions.Filter.IsEmpty())
        {
            ActorLabelsResult->SetStringField(TEXT("filter"), LabelOptions.Filter);
        }

        Overlays->SetBoolField(TEXT("actorLabels"), true);
    }

    Overlays->SetBoolField(TEXT("labels"), bLabels);

    // 4b) The paint pass is over, so close the session and then the subject - IN THAT ORDER, and
    // BEFORE the response is built.
    //
    // ORDER: the session holds the viewport at the capture pose and must put it back before the
    // resolver closes the asset editor that owns that viewport. Nothing below projects another
    // point, and PNG encoding touches no viewport state.
    //
    // WHY BEFORE THE RESPONSE, and not simply left to the destructors at the end of the handler:
    // `subject.assetEditorClosed` is emitted for every asset kind (CaptureSubject.cpp,
    // MakeSubjectInfoObject), and a subject still holding its window when the block is built can
    // only ever publish `false` - a predicted value dressed as a measured one. ReleaseSubject
    // records the MEASURED outcome into the subject and is idempotent, so the destructor that runs
    // later is a no-op; releasing here is the difference between reporting what happened and
    // reporting what was intended. On the world/actor path the release only restores a Level
    // Sequence playhead, which this verb never moves, so running it here changes nothing.
    SessionHolder.Reset();
    // Drop this function's own references to the viewport BEFORE the release, so closing an asset
    // editor destroys its preview viewport on the release's own stack rather than leaving the
    // FSceneViewport alive on our shared pointer until the handler returns. Neither is read again;
    // the raw client would be dangling from here on either way.
    ViewportClient = nullptr;
    SceneViewport.Reset();
    if (bSubjectResolved)
    {
        PinWrightCaptureSubject::ReleaseSubject(ResolvedSubject);
    }

    // 5) Re-encode the annotated buffer to a NEW PNG in the same subdirectory.
    TArray64<uint8> AnnotatedPng;
    FImageUtils::PNGCompressImageArray(W, H,
        TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), AnnotatedPng);
    if (AnnotatedPng.Num() == 0)
    {
        IFileManager::Get().Delete(*Capture.Path, false, true);
        Ctx.SendError(ErrorCodes::ERR_ENCODE_FAILED, TEXT("Failed to encode annotated capture as PNG"));
        return true;
    }

    // Derive the annotated filename from the base capture filename (insert _annotated
    // before the extension) and route it through the same Saved/Screenshots helper.
    FString AnnotatedRequested = Capture.Filename;
    if (AnnotatedRequested.EndsWith(TEXT(".png"), ESearchCase::IgnoreCase))
    {
        AnnotatedRequested = AnnotatedRequested.LeftChop(4) + TEXT("_annotated.png");
    }
    else
    {
        AnnotatedRequested += TEXT("_annotated");
    }
    FString AnnotatedFilename;
    const FString AnnotatedPath = PinWrightScreenshotUtils::MakeScreenshotOutputPath(
        AnnotatedRequested, TEXT("Annotated"), TEXT("AnnotatedCapture"), AnnotatedFilename);
    if (!FFileHelper::SaveArrayToFile(AnnotatedPng, *AnnotatedPath))
    {
        IFileManager::Get().Delete(*Capture.Path, false, true);
        Ctx.SendError(ErrorCodes::ERR_SAVE_FAILED,
            FString::Printf(TEXT("Failed to save annotated capture: %s"), *AnnotatedPath));
        return true;
    }
    // The base capture was only an intermediate for the annotated frame; drop it so a
    // call leaves exactly one PNG behind.
    IFileManager::Get().Delete(*Capture.Path, false, true);

    // 6) Build the AddCaptureFields-style result for the annotated PNG + overlay echo.
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("path"), AnnotatedPath);
    Result->SetStringField(TEXT("filename"), AnnotatedFilename);
    Result->SetNumberField(TEXT("width"), W);
    Result->SetNumberField(TEXT("height"), H);
    Result->SetNumberField(TEXT("sizeBytes"), static_cast<double>(AnnotatedPng.Num()));
    Result->SetStringField(TEXT("projectionMode"), Request.ProjectionMode);
    // The pose the pixels show, measured off the client after the camera was applied (see
    // RenderHandler::AddCaptureFields). It matters more here than anywhere else: the overlay is
    // projected against this same pose, so a response quoting the REQUEST would disagree with the
    // labels drawn on the frame.
    Result->SetObjectField(TEXT("cameraLocation"), MakeVectorObject(Capture.EffectiveLocation));
    Result->SetObjectField(TEXT("cameraRotation"), MakeRotatorObject(Capture.EffectiveRotation));
    if (Capture.bOrthoRotationSnapped || !Capture.bCameraAimApplied)
    {
        Result->SetObjectField(TEXT("requestedRotation"), MakeRotatorObject(Request.Rotation));
    }
    if (!Capture.bCameraAimApplied)
    {
        Result->SetObjectField(TEXT("requestedLocation"), MakeVectorObject(Request.Location));
    }
    if (Request.ProjectionMode == TEXT("orthographic"))
    {
        Result->SetNumberField(TEXT("orthoWidth"), Request.OrthoWidth);
        if (!Capture.OrthoView.IsEmpty())
        {
            Result->SetStringField(TEXT("orthoView"), Capture.OrthoView);
        }
    }
    else
    {
        Result->SetNumberField(TEXT("fov"), Request.Fov);
    }
    Result->SetStringField(TEXT("renderer"), Capture.Renderer);
    Result->SetStringField(TEXT("mimeType"), TEXT("image/png"));

    // What the FRAME looked like. These were computed by CaptureEditorViewportToPng for every
    // capture and then thrown away by this verb alone - it published the annotated PNG's byte
    // count and nothing about its pixels, on the one verb whose entire purpose is a frame a human
    // will read. Same field names and same shapes as RenderHandler's AddCaptureFields, so one
    // reader parses every capture response.
    //
    // MEASURED ON THE BASE FRAME, before a single overlay pixel is painted, and that is the point:
    // the axes, grid and labels are synthetic ink at fixed, known colors, so folding them in would
    // lift the mean, invent tone levels and turn a blank render non-blank. `blank` here therefore
    // still answers "did the SCENE draw anything", which is the question a caller staring at an
    // annotated frame of pure overlay actually has.
    TSharedPtr<FJsonObject> ImageStats = MakeShared<FJsonObject>();
    ImageStats->SetNumberField(TEXT("meanLuminance"), Capture.ImageStats.MeanLuminance);
    ImageStats->SetNumberField(TEXT("luminanceVariance"), Capture.ImageStats.LuminanceVariance);
    ImageStats->SetNumberField(TEXT("minLuminance"), Capture.ImageStats.MinLuminance);
    ImageStats->SetNumberField(TEXT("maxLuminance"), Capture.ImageStats.MaxLuminance);
    ImageStats->SetNumberField(TEXT("litPixelCount"),
        static_cast<double>(Capture.ImageStats.LitPixelCount));
    ImageStats->SetNumberField(TEXT("litPixelFraction"), Capture.ImageStats.LitPixelFraction);
    ImageStats->SetNumberField(TEXT("litLuminanceThreshold"),
        PinWrightRenderCapture::BlankLitLuminanceThreshold);
    PinWrightRenderCapture::AddToneRangeStatsFields(Capture.ImageStats, ImageStats);
    Result->SetObjectField(TEXT("imageStats"), ImageStats);
    Result->SetBoolField(TEXT("blank"), Capture.ImageStats.bBlank);
    // Siblings of `blank` at the same prominence, through the same helper AddCaptureFields uses:
    // an unmeasured frame reports no range rather than the most collapsed one, a non-lit frame
    // reports `toneRangeApplicable: false` rather than a false collapse, and `rangeWarning` is
    // present if and only if there is something to warn about.
    PinWrightRenderCapture::AddToneRangeVerdictFields(Capture, Result);
    Result->SetNumberField(TEXT("redrawRetries"), Capture.RedrawRetries);
    // The view mode the underlying frame was rendered in. The annotation pass draws on top of that
    // frame, so an overlay of crisp labels over a wireframe still shows no materials or lighting.
    Result->SetObjectField(TEXT("viewport"),
        PinWrightRenderCapture::MakeViewportInfoObject(Capture));
    Result->SetObjectField(TEXT("viewDistance"),
        PinWrightRenderCapture::MakeViewDistanceInfoObject(Capture));
    // `framing` and `subject`: emitted IF AND ONLY IF the caller named a subject and it resolved.
    // Not a zeroed block on every call - a framing verdict with no bounds behind it has nothing to
    // say, and an empty `subject` would be one more absence for a caller to discover from a diff.
    // The check runs against the MEASURED pose, so it also catches a viewport that ignored the
    // requested aim, and it is conservative in one direction: boundsInFrame is false only when the
    // subject provably cannot project into the frame.
    if (bSubjectResolved)
    {
        const PinWrightRenderCapture::FBoundsFramingCheck Framing =
            PinWrightRenderCapture::EvaluateBoundsFraming(
                Request, Capture.EffectiveLocation, Capture.EffectiveRotation,
                ResolvedSubject.BoundsOrigin, ResolvedSubject.BoundsRadius);
        Result->SetObjectField(TEXT("framing"),
            PinWrightRenderCapture::MakeBoundsFramingObject(Framing));
        Result->SetObjectField(TEXT("subject"),
            PinWrightCaptureSubject::MakeSubjectInfoObject(ResolvedSubject));
    }
    // WHERE THE PIXELS CAME FROM. The level string is unchanged and unconditional on the level
    // path. On the asset path it is the resolver's own capture-source vocabulary - the same strings
    // render.capture_asset_preview publishes ("staticMeshEditorPreview", "personaPreviewViewport",
    // "niagaraSystemEditorPreview") - with the verb's existing "Annotated" suffix, so one reader
    // parses every capture response and no caller can mistake a preview frame for a level one.
    Result->SetStringField(TEXT("captureSource"),
        bAssetSubject && !ResolvedSubject.CaptureSource.IsEmpty()
            ? ResolvedSubject.CaptureSource + TEXT("Annotated")
            : FString(TEXT("levelEditorViewportAnnotated")));
    // `levelPath` names the level these pixels are OF, so it is omitted on the asset path rather
    // than reporting whatever level happens to be loaded behind an asset-editor window.
    if (!bAssetSubject && EditorWorld)
    {
        Result->SetStringField(TEXT("levelPath"),
            EditorWorld->GetOutermost() ? EditorWorld->GetOutermost()->GetName() : FString());
    }
    Result->SetObjectField(TEXT("overlays"), Overlays);
    // The actor->pixel map is DATA, not an overlay echo, so it sits at the top level next to
    // `path` (drive.observe puts `elements[]` beside `screenshot` for the same reason).
    // overlays.actorLabels stays a plain bool alongside overlays.axes / overlays.labels. Absent
    // entirely unless the caller opted in.
    if (ActorLabelsResult.IsValid())
    {
        Result->SetObjectField(TEXT("actorLabels"), ActorLabelsResult);
    }

    if (bInline)
    {
        TArray<uint8> Bytes;
        if (FFileHelper::LoadFileToArray(Bytes, *AnnotatedPath) && Bytes.Num() > 0)
        {
            Result->SetStringField(TEXT("base64"), FBase64::Encode(Bytes.GetData(), Bytes.Num()));
        }
    }

    Ctx.SendSuccess(Result);
    return true;
}
