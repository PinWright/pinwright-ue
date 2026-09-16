// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/World.h"
#include "IAssetViewport.h"
#include "LevelEditor.h"
// FRandomStream -- the engine's own deterministic stream, used for the seeded jitter offset so
// the same seed reproduces the same shot set on every machine and every run.
#include "Math/RandomStream.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"
#include "Slate/SceneViewport.h"

// The `padding` parameter's wire description, shared by the verbs that let a caller reach
// ComputeFitDistance's margin. Defined beside PINWRIGHT_EXPOSURE_PARAM_DESC's file-scope
// neighbours rather than inside the namespace, matching PreviewViewportCaptureUtils.h.
//
// Each verb appends its OWN default sentence by adjacent string-literal concatenation: the
// padding defaults in this tree are genuinely three different numbers (1.15 here, 1.25 on
// render.capture_asset_preview, 1.4 on render.capture_animation_preview) and one shared
// sentence naming a single default would be false on two of the verbs.
#define PINWRIGHT_FIT_PADDING_PARAM_DESC \
    "Bounds-fit margin multiplier; >1 pulls the camera back for more headroom. Applies only " \
    "when the camera distance is being SOLVED from the subject's bounds - an explicit distance " \
    "override skips the fit entirely and this parameter then has nothing to modify. On an " \
    "orthographic shot it also widens the world-space frame, because the same margin decides " \
    "orthoWidth."

// Shot planning and shot serialization shared by every multi-shot capture verb.
//
// This started as a file-local namespace inside CameraFrameHandler.cpp. It moved out when a
// second and third verb needed the same maths (camera.animation_shots, which crosses the view
// axis with a time axis, and render.capture_animation_preview, which reuses the shot
// serialization on an asset-editor viewport). Copying ComputeFitDistance / PlaceOrbitCamera /
// AddShotFields per handler is how one of them ends up framing differently from the others, or
// stops emitting imageStats after the others gained it — the exact defect AddShotFields exists
// to prevent.
//
// File-unique NAMED namespace, not anonymous, so these symbols cannot collide with the
// same-named anonymous-namespace helpers in sibling Render/*.cpp files when Unity merges
// translation units (IsValidProjectionMode also exists in PreviewViewportCaptureUtils.cpp).
// Each handler body pulls it in with a function-local using-directive that stays confined to
// that function; there is deliberately no file-scope using-directive in this header.
namespace PinWrightCameraFrame
{
    using namespace PinWrightRenderCapture;

    // Hard ceiling on shots per multi-shot call. Each shot moves a real editor camera, resizes
    // the viewport and does a full offscreen readback, so an unbounded count would stall the
    // editor; beyond this we reject with a typed TOO_MANY_SHOTS.
    constexpr int32 GMaxOrbitShots = 24;

    // Matches the util's own bound (PreviewViewportCaptureUtils MaxCaptureDimension).
    constexpr int32 GMaxCaptureDimension = 16384;

    // The policy names remain distinct even though their current pixel budgets intentionally
    // converge on one default. Reporting which policy selected the size is a wire contract; it
    // must not be inferred by comparing equal integers.
    constexpr int32 GMultiImageBudgetEdge = DefaultCaptureEdge;
    constexpr int32 GSingleStillEdge = DefaultCaptureEdge;
    constexpr int32 GLegacyDefaultEdge = DefaultCaptureEdge;

    // camera.orbit_shots has two compatibility budgets that predate the shared 768 default and
    // are documented on the verb (docs/wiki-src/camera.md). They stay distinct from the policy
    // constants above so animation bursts and ordinary stills can use the shared default without
    // changing archived orbit comparisons: shrinking the legacy edge would rescale
    // world-units-per-pixel for every orthographic measurement recipe built on the old size.
    // They live here, not in the handler .cpp, because the regression floor asserts them and a
    // test with its own literal copy is how the two drifted apart in the first place.
    constexpr int32 GOrbitLegacyDefaultEdge = 1024;
    constexpr int32 GOrbitViewsBudgetEdge = 640;

    // ---- ONE `resolutionSource` vocabulary, for every capture verb ----
    //
    // The field answers a single question - which rule picked the pixel size - and it had three
    // different answers. camera.orbit_shots said "caller"|"budget"|"default"; camera.animation_shots
    // and render.capture_animation_preview said "caller"|"burstBudget"|"singleStill";
    // camera.frame_actor said nothing at all. Same concept, same namespace, three answers, so a
    // caller comparing two capture sets had to know which verb produced each string.
    //
    // The vocabulary is the UNION of the two existing word sets, not a new one:
    //   * "budget" and "burstBudget" were the same rule under two
    //     spellings, so they collapse into "budget";
    //   * "singleStill" and "default" remain different reasons even when their pixel values are
    //     equal, so both survive - collapsing them would lose the one thing this field exists to say.
    // Consequence: every string the regression floor already asserts is unchanged
    // (Tests/Render/TestCameraFrameHandlers.cpp asserts "budget", "caller" and "default"), and
    // exactly one string moves on the wire in the whole convergence wave: "burstBudget" ->
    // "budget" on the two animation verbs.
    enum class EResolutionSource : uint8
    {
        Caller,      // width and/or height came from the caller; that always wins
        Budget,      // this call produces a SET
        SingleStill, // this call produces one image
        Default,     // the verb's ordinary default
    };

    // The stable machine key for one rule, spelled the way `ViewModeKey` is: a string a caller
    // compares against, not a label it reads. This is the ONLY place any of the four words is
    // written, so the vocabulary cannot fork again by a verb hand-typing one of them.
    inline FString ResolutionSourceKey(EResolutionSource Source)
    {
        switch (Source)
        {
        case EResolutionSource::Caller:      return TEXT("caller");
        case EResolutionSource::Budget:      return TEXT("budget");
        case EResolutionSource::SingleStill: return TEXT("singleStill");
        default:                             return TEXT("default");
        }
    }

    // The caller supplies the semantic policy explicitly. Pixel values cannot encode it because
    // all omitted-size policies currently resolve to the same 768 edge.
    inline EResolutionSource ClassifyResolutionSource(bool bCallerSizeProvided,
        EResolutionSource DefaultSource)
    {
        if (bCallerSizeProvided)
        {
            return EResolutionSource::Caller;
        }
        return DefaultSource;
    }

    // The one line an adopting verb writes, right where it already knows both facts:
    //     Result->SetStringField(TEXT("resolutionSource"),
    //         ResolveResolutionSource(bWidthProvided || bHeightProvided, DefaultSource));
    inline FString ResolveResolutionSource(bool bCallerSizeProvided,
        EResolutionSource DefaultSource)
    {
        return ResolutionSourceKey(ClassifyResolutionSource(bCallerSizeProvided, DefaultSource));
    }

    inline bool IsValidProjectionMode(const FString& Mode)
    {
        return Mode == TEXT("perspective") || Mode == TEXT("orthographic");
    }

    inline bool AreDimensionsValid(int32 Width, int32 Height)
    {
        return Width > 0 && Height > 0 &&
            Width <= GMaxCaptureDimension && Height <= GMaxCaptureDimension;
    }

    // Distance that fits a bounding sphere of the given radius inside the vertical
    // FOV, with a floor and a padding margin. Mirrors render.capture_asset_preview's
    // no-args framing math so all capture verbs frame consistently.
    inline float ComputeFitDistance(float Radius, float Fov, float Padding)
    {
        const float HalfFovRadians = FMath::DegreesToRadians(FMath::Clamp(Fov, 1.0f, 170.0f) * 0.5f);
        const float FitDistance = Radius / FMath::Max(FMath::Tan(HalfFovRadians), KINDA_SMALL_NUMBER);
        return FMath::Max(FitDistance, Radius * 1.5f) * FMath::Max(Padding, 0.01f);
    }

    // ---- the two inputs of ComputeFitDistance, and who is allowed to reach them ----
    //
    // ComputeFitDistance takes a RADIUS, a FOV and a PADDING, and the caller-facing surface over
    // it was inverted between the two camera verbs: camera.orbit_shots exposed an explicit
    // `radius` (the distance) and WELDED `constexpr float Padding = 1.15f`, while
    // camera.frame_actor exposed `padding` and had no distance override at all. Same helper, same
    // file, opposite halves. camera.animation_shots already exposed BOTH, which is the proof this
    // was oversight rather than design - the union already shipped.
    //
    // Both halves now live here so a third verb adopting the helper inherits the whole surface
    // instead of picking one end of it again.
    //
    // NOT CONVERGED, deliberately: render.capture_asset_preview welds 1.25
    // (RenderHandler.cpp, with a comment recording that the divergence was noticed and accepted)
    // and render.capture_animation_preview defaults 1.4 for limb reach outside the rest bounds.
    // Moving either would change what every archived capture of those verbs looks like, so they
    // stay as they are and the class of gap is caught by the cross-verb parameter-parity walk
    // rather than by a silent convergence here.
    constexpr float GDefaultFitPadding = 1.15f;

    // The `padding` wire description is PINWRIGHT_FIT_PADDING_PARAM_DESC, defined at file scope
    // above this namespace with the other parameter-description macros.

    // How far the camera sits from the subject, in ONE spelling for both camera verbs.
    //
    // Precedence: an explicit caller-supplied distance wins, then the subject's own radius, then
    // a bounds fit through ComputeFitDistance. A non-positive override is treated as ABSENT
    // rather than as a request to put the camera inside the subject - that is the rule
    // camera.orbit_shots' `radius` already shipped with (`RadiusParam > 0.0f`), kept verbatim so
    // no call shape moves.
    //
    // `subject.radius` MEANS TWO DIFFERENT THINGS and the code keeps both meanings on purpose.
    // It is parsed once (CaptureSubject.cpp) into one FSubjectRequest::Radius, and then:
    //   * camera.orbit_shots reads it as a CAMERA DISTANCE - a bare world point has no size, so
    //     the orbit radius IS its framing extent, and that is the rule the legacy point path has
    //     always used. Pass bSubjectRadiusProvided=true here.
    //   * camera.frame_actor reads it as a SPHERE RADIUS - it feeds the bounds the fit is solved
    //     FROM, so consuming it a second time as a distance would double-count it and move
    //     pixels every existing caller already gets. Pass bSubjectRadiusProvided=false here.
    // For subject:{kind:"world", point, radius:R} at fov 50 that puts orbit's camera at R and
    // frame_actor's at roughly 2.5*R. Neither verb says so on the wire; this comment is here so
    // the next reader does not lose an hour rediscovering it.
    inline float ResolveCameraDistance(bool bDistanceProvided, float DistanceOverride,
        bool bSubjectRadiusProvided, float SubjectRadius,
        float BoundsRadius, float Fov, float Padding)
    {
        if (bDistanceProvided && DistanceOverride > 0.0f)
        {
            return DistanceOverride;
        }
        if (bSubjectRadiusProvided && SubjectRadius > 0.0f)
        {
            return SubjectRadius;
        }
        return ComputeFitDistance(BoundsRadius, Fov, Padding);
    }

    // World-space width (cm) the orthographic frame must span to hold the bounds sphere.
    // Request.OrthoWidth is world centimetres; PreviewViewportCaptureUtils converts it to the
    // editor's ortho zoom against the engine's own GetOrthoUnitsPerPixel, so no CAMERA_ZOOM_DIV
    // factor belongs here (the old `* 15.0f` was also wrong on its own terms: with
    // r.Editor.AlignedOrthoZoom on - the default - the zoom->world scale depends on the viewport
    // pixel width, so a fixed divisor under-zoomed roughly 2x at 1024 px). An aspect factor keeps
    // the sphere in-frame on landscape captures where the vertical span is the tighter of the two.
    inline float ComputeOrthoWorldWidth(float Radius, float Padding, int32 Width, int32 Height)
    {
        const float WorldDiameter = 2.0f * Radius * FMath::Max(Padding, 0.01f);
        const float AspectFactor = (Height > 0)
            ? FMath::Max(1.0f, static_cast<float>(Width) / static_cast<float>(Height))
            : 1.0f;
        return FMath::Max(WorldDiameter * AspectFactor, 1.0f);
    }

    // Snap an orbit pose onto the nearest cardinal world axis, in place. Returns true when it moved.
    //
    // Orthographic editor rendering ignores the camera rotation: FEditorViewportClient::CalcSceneView
    // builds the ortho view matrix from the VIEWPORT TYPE (UE 5.8 EditorViewportClient.cpp:1341-1401),
    // so only the six cardinal directions are renderable. This has to happen BEFORE the camera is
    // placed, not after: an orthographic frame is centred on the camera POSITION (ViewOrigin), not on
    // a look-at point, so snapping only the view direction would leave the camera off-axis and push
    // the subject out of frame. Ties (azimuth exactly 45 deg) resolve by FMath::RoundToFloat.
    inline bool SnapOrbitAnglesToOrthographicAxis(float& Azimuth, float& Elevation)
    {
        const float OriginalAzimuth = Azimuth;
        const float OriginalElevation = Elevation;

        const float ClampedElevation = FMath::Clamp(Elevation, -90.0f, 90.0f);
        if (FMath::Abs(ClampedElevation) >= 45.0f)
        {
            // Top / bottom: the engine fixes the in-plane orientation, so azimuth is meaningless.
            Elevation = (ClampedElevation >= 0.0f) ? 90.0f : -90.0f;
            Azimuth = 0.0f;
        }
        else
        {
            Elevation = 0.0f;
            Azimuth = FMath::RoundToFloat(FRotator::NormalizeAxis(Azimuth) / 90.0f) * 90.0f;
        }

        return !FMath::IsNearlyEqual(Azimuth, OriginalAzimuth, 0.01f) ||
            !FMath::IsNearlyEqual(Elevation, OriginalElevation, 0.01f);
    }

    // Place the camera on a sphere of the given radius around Center, at the given
    // azimuth (degrees, around +Z) and elevation (degrees, above the horizon), then
    // point it back at Center. Azimuth 0 sits on +X; elevation +90 is straight above.
    inline void PlaceOrbitCamera(const FVector& Center, float AzimuthDeg, float ElevationDeg, float Distance,
        FVector& OutLocation, FRotator& OutRotation)
    {
        const float Az = FMath::DegreesToRadians(AzimuthDeg);
        const float El = FMath::DegreesToRadians(ElevationDeg);
        const FVector Offset(
            FMath::Cos(El) * FMath::Cos(Az),
            FMath::Cos(El) * FMath::Sin(Az),
            FMath::Sin(El));
        OutLocation = Center + Offset * FMath::Max(Distance, 1.0f);
        OutRotation = (Center - OutLocation).Rotation();
    }

    // Resolve the active Level Editor viewport client + scene viewport, or fail with a
    // typed error. Same acquisition path as render.capture_open_level.
    inline bool GetActiveLevelViewport(FEditorViewportClient*& OutClient,
        TSharedPtr<FSceneViewport>& OutSceneViewport, FString& OutErrCode, FString& OutErrMsg)
    {
        OutClient = nullptr;
        OutSceneViewport.Reset();

        if (!GEditor)
        {
            OutErrCode = ErrorCodes::ERR_EDITOR_NOT_AVAILABLE;
            OutErrMsg = TEXT("Editor not available");
            return false;
        }
        if (!GEditor->GetEditorWorldContext().World())
        {
            OutErrCode = ErrorCodes::ERR_NO_EDITOR_WORLD;
            OutErrMsg = TEXT("No active editor world");
            return false;
        }

        FLevelEditorModule* LevelEditorModule =
            FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
        if (!LevelEditorModule)
        {
            OutErrCode = ErrorCodes::ERR_NO_ACTIVE_LEVEL_VIEWPORT;
            OutErrMsg = TEXT("LevelEditor module is not loaded");
            return false;
        }

        TSharedPtr<IAssetViewport> ActiveViewport = LevelEditorModule->GetFirstActiveViewport();
        if (!ActiveViewport.IsValid())
        {
            OutErrCode = ErrorCodes::ERR_NO_ACTIVE_LEVEL_VIEWPORT;
            OutErrMsg = TEXT("No active Level Editor viewport");
            return false;
        }

        OutSceneViewport = ActiveViewport->GetSharedActiveViewport();
        if (!OutSceneViewport.IsValid())
        {
            OutErrCode = ErrorCodes::ERR_NO_ACTIVE_LEVEL_VIEWPORT;
            OutErrMsg = TEXT("Active Level Editor viewport has no SceneViewport");
            return false;
        }

        OutClient = &ActiveViewport->GetAssetViewportClient();
        return true;
    }

    // Serialize a completed capture into the shared shot fields (path, filename, dims,
    // camera pose, projection-specific fov/orthoWidth, renderer, mimeType, image stats).
    // Mirrors RenderHandler's AddCaptureFields, reusing the exported vector/rotator builders.
    inline void AddShotFields(const FViewportCaptureOutput& Capture, const FViewportCaptureRequest& Request,
        TSharedPtr<FJsonObject>& Out)
    {
        Out->SetStringField(TEXT("path"), Capture.Path);
        Out->SetStringField(TEXT("filename"), Capture.Filename);
        Out->SetNumberField(TEXT("width"), Capture.Width);
        Out->SetNumberField(TEXT("height"), Capture.Height);
        Out->SetNumberField(TEXT("sizeBytes"), static_cast<double>(Capture.SizeBytes));
        Out->SetStringField(TEXT("projectionMode"), Request.ProjectionMode);
        // The pose the pixels show, measured off the client after the camera was applied; for
        // orthographic shots the engine quantises the in-plane orientation to the viewport type it
        // renders (see RenderHandler::AddCaptureFields). An orbit set is the exact workload the
        // measured form exists for: every shot in one is aimed relative to every other.
        Out->SetObjectField(TEXT("cameraLocation"), MakeVectorObject(Capture.EffectiveLocation));
        Out->SetObjectField(TEXT("cameraRotation"), MakeRotatorObject(Capture.EffectiveRotation));
        if (Capture.bOrthoRotationSnapped || !Capture.bCameraAimApplied)
        {
            Out->SetObjectField(TEXT("requestedRotation"), MakeRotatorObject(Request.Rotation));
        }
        if (!Capture.bCameraAimApplied)
        {
            Out->SetObjectField(TEXT("requestedLocation"), MakeVectorObject(Request.Location));
        }
        if (Request.ProjectionMode == TEXT("orthographic"))
        {
            Out->SetNumberField(TEXT("orthoWidth"), Request.OrthoWidth);
            if (!Capture.OrthoView.IsEmpty())
            {
                Out->SetStringField(TEXT("orthoView"), Capture.OrthoView);
            }
        }
        else
        {
            Out->SetNumberField(TEXT("fov"), Request.Fov);
        }
        Out->SetStringField(TEXT("renderer"), Capture.Renderer);
        Out->SetStringField(TEXT("mimeType"), TEXT("image/png"));

        // Per-shot luminance stats and the blank verdict. CaptureEditorViewportToPng already
        // computes these (CalculateCaptureImageStats) for every path; camera.* simply never
        // emitted them, so an orbit set could come back six-for-six black and still read as a
        // clean success. These verbs deliberately do NOT set bRejectBlankCapture — an actor
        // legitimately reviewed against a dark backdrop must not hard-fail — so the caller is the
        // one who decides, and it needs the numbers to decide with. Checking file size instead
        // (the advice camera.md gives today) is a proxy that mis-reads on flat-lit subjects.
        // redrawRetries is not emitted here: with rejection off it is always 0 and would only
        // suggest a retry happened.
        TSharedPtr<FJsonObject> ImageStats = MakeShared<FJsonObject>();
        ImageStats->SetNumberField(TEXT("meanLuminance"), Capture.ImageStats.MeanLuminance);
        ImageStats->SetNumberField(TEXT("luminanceVariance"), Capture.ImageStats.LuminanceVariance);
        ImageStats->SetNumberField(TEXT("minLuminance"), Capture.ImageStats.MinLuminance);
        ImageStats->SetNumberField(TEXT("maxLuminance"), Capture.ImageStats.MaxLuminance);
        // The two numbers `blank` is MADE OF, plus the threshold they were counted against, so a
        // caller can judge a near-empty shot without inheriting this verb's threshold. Emitted
        // here for the same reason AddCaptureFields emits them, and with the same values: a
        // per-shot stats block that carries a verdict but not its inputs is exactly the drift
        // AddToneRangeStatsFields' own declaration comment predicts in
        // PreviewViewportCaptureUtils.h - and it had already happened: AddCaptureFields gained the
        // lit pair and the tone range while every shot of every multi-shot set kept reporting four
        // numbers.
        ImageStats->SetNumberField(TEXT("litPixelCount"),
            static_cast<double>(Capture.ImageStats.LitPixelCount));
        ImageStats->SetNumberField(TEXT("litPixelFraction"), Capture.ImageStats.LitPixelFraction);
        ImageStats->SetNumberField(TEXT("litLuminanceThreshold"),
            PinWrightRenderCapture::BlankLitLuminanceThreshold);
        // How many 8-bit luminance levels this shot resolves, and the per-level floor it was
        // counted against. Writes NOTHING when the frame was never measured, so an unmeasured shot
        // reports no range rather than the most collapsed one possible - the absence rule the
        // shared helper already keeps.
        PinWrightRenderCapture::AddToneRangeStatsFields(Capture.ImageStats, ImageStats);
        Out->SetObjectField(TEXT("imageStats"), ImageStats);
        Out->SetBoolField(TEXT("blank"), Capture.ImageStats.bBlank);
        // Per shot, for exactly the reason the lit pair is emitted per shot above: a multi-angle
        // set is how an asset gets reviewed, so a subject reading that existed only on the
        // top-level block would leave every angle but the first unable to see a black subject.
        // Emitted with the same values AddCaptureFields uses.
        PinWrightSubjectRegion::AddSubjectRegionFields(Capture.SubjectRegion, Out);
    }

    // Opt-in: read the just-saved PNG back off disk and embed it as base64. Kept off by
    // default because a capture set of full-res PNGs is large on the wire.
    inline void MaybeAddBase64(const FString& Path, bool bInline, TSharedPtr<FJsonObject>& Out)
    {
        if (!bInline)
        {
            return;
        }
        TArray<uint8> Bytes;
        if (FFileHelper::LoadFileToArray(Bytes, *Path) && Bytes.Num() > 0)
        {
            Out->SetStringField(TEXT("base64"), FBase64::Encode(Bytes));
        }
    }

    // One planned orbit pose before capture.
    struct FPlannedShot
    {
        float Azimuth = 0.0f;
        float Elevation = 0.0f;
        FString ProjectionMode = TEXT("perspective");
    };

    // How many views MakeSideViews plans. Named so a shot-budget check can be written against the
    // table instead of against a 6 sitting next to a table the caller does not own.
    constexpr int32 GSideViewCount = 6;

    // The six axis-aligned views, in the order every verb has always emitted them:
    // front, back, left, right, top, bottom.
    //
    // ONE table. It was written out verbatim in three handlers - CameraFrameHandler.cpp,
    // AnimationShotsHandler.cpp and AnimationPreviewCaptureHandler.cpp - and a fourth copy was one
    // verb away. Three copies of a table are three chances for one of them to be reordered, and a
    // reordered `sides` set is six individually correct images that no longer line up with the
    // archived set they are compared against. (No `file:line` citation for the old copies on
    // purpose: this function is what deletes them, so any line number here would stop landing on
    // the code it names as each adopter switches over.)
    //
    // Two spellings are load-bearing and neither is cosmetic. `back` uses 180, not -180, because
    // FRotator::NormalizeAxis(180) returns 180 and SnapOrbitAnglesToOrthographicAxis would
    // otherwise report a spurious move; `right` uses -90, not 270, for the same reason. The angles
    // are chosen so each pose already lands exactly on a cardinal world axis and the snap is a
    // no-op: the camera forward is -Offset (see PlaceOrbitCamera), which resolves against the
    // viewport-type table in PreviewViewportCaptureUtils.cpp as front/back/left/right/top/bottom
    // in this order.
    //
    // ProjectionMode is passed in rather than defaulted here: each verb decides whether an unsized
    // `views` call means orthographic (the current default at all three call sites) or honours an
    // explicit projectionMode, and that precedence is a verb-level argument rule, not a property
    // of the table.
    inline TArray<FPlannedShot> MakeSideViews(const FString& ProjectionMode)
    {
        TArray<FPlannedShot> Views;
        Views.Reserve(GSideViewCount);
        Views.Add(FPlannedShot{   0.0f,   0.0f, ProjectionMode }); // front
        Views.Add(FPlannedShot{ 180.0f,   0.0f, ProjectionMode }); // back
        Views.Add(FPlannedShot{  90.0f,   0.0f, ProjectionMode }); // left
        Views.Add(FPlannedShot{ -90.0f,   0.0f, ProjectionMode }); // right
        Views.Add(FPlannedShot{   0.0f,  90.0f, ProjectionMode }); // top
        Views.Add(FPlannedShot{   0.0f, -90.0f, ProjectionMode }); // bottom
        return Views;
    }

    // ---- how N shots are spread over the viewing sphere ----
    //
    // `Ring` is what `count` has always produced: evenly-spaced AZIMUTH at ONE fixed elevation, so
    // N shots sample a horizontal circle rather than a sphere. Every shot in such a set misses the
    // underside and the top, which is exactly where this project's recent geometry defects hid - a
    // detached head visible only from certain angles, a self-intersection legible only at a
    // grazing elevation, a door on a face no ring shot reached.
    //
    // `Sphere` is a golden-angle (Fibonacci) spiral. It is the right answer instead of a random
    // mode for two reasons, both measured rather than aesthetic:
    //   * uniformly random points on a sphere CLUMP - eight of them routinely give two
    //     near-duplicate views and a bald patch, so random is worse at coverage than an even
    //     spread, which is the only thing it would be asked for;
    //   * random destroys reproducibility, and this project has already paid for a
    //     non-deterministic subsystem making a sampling artifact read as a regression. Acceptance
    //     shots that cannot be retaken identically make every before/after comparison worthless.
    enum class EShotDistribution : uint8
    {
        Ring,
        Sphere,
    };

    // The golden angle in degrees, 360 * (1 - 1/phi) = 180 * (3 - sqrt(5)). Written as the closed
    // form rather than a literal so it cannot be transcribed wrong.
    inline double GoldenAngleDegrees()
    {
        return 180.0 * (3.0 - FMath::Sqrt(5.0));
    }

    // N poses spread over the sphere by the standard Fibonacci-spiral construction:
    //     z = 1 - 2*(i + 0.5)/N          -> elevation = asin(z), equal-area bands in z
    //     azimuth = i * goldenAngle      -> no two shots share a great circle
    // Deterministic for a given (Count, AzimuthOffsetDegrees). N == 1 gives the single pose at
    // z = 0, i.e. the horizon.
    //
    // ELEVATION IS NOT AN INPUT HERE. The construction derives every elevation from the index, so
    // an `elevation` argument has nothing to modify - which is why the verb reports that it was
    // IGNORED rather than quietly dropping it. A silently discarded parameter is the same defect
    // class as an argument that changes nothing and echoes itself back.
    inline TArray<FPlannedShot> MakeSphereDistribution(int32 Count, double AzimuthOffsetDegrees,
        const FString& ProjectionMode)
    {
        TArray<FPlannedShot> Shots;
        if (Count <= 0)
        {
            return Shots;
        }
        Shots.Reserve(Count);
        const double Golden = GoldenAngleDegrees();
        for (int32 Index = 0; Index < Count; ++Index)
        {
            const double Z = 1.0 - 2.0 * (static_cast<double>(Index) + 0.5) / static_cast<double>(Count);
            FPlannedShot Shot;
            Shot.Elevation = static_cast<float>(FMath::RadiansToDegrees(
                FMath::Asin(FMath::Clamp(Z, -1.0, 1.0))));
            Shot.Azimuth = static_cast<float>(
                FMath::Fmod(static_cast<double>(Index) * Golden + AzimuthOffsetDegrees, 360.0));
            if (Shot.Azimuth < 0.0f)
            {
                Shot.Azimuth += 360.0f;
            }
            Shot.ProjectionMode = ProjectionMode;
            Shots.Add(Shot);
        }
        return Shots;
    }

    // The current `count` behaviour, unchanged: evenly-spaced azimuth at one fixed elevation.
    // Kept as a named function so `ring` and `sphere` sit side by side and the default is
    // obviously the pre-existing one.
    inline TArray<FPlannedShot> MakeRingDistribution(int32 Count, float Elevation,
        double AzimuthOffsetDegrees, const FString& ProjectionMode)
    {
        TArray<FPlannedShot> Shots;
        if (Count <= 0)
        {
            return Shots;
        }
        Shots.Reserve(Count);
        for (int32 Index = 0; Index < Count; ++Index)
        {
            FPlannedShot Shot;
            // Written as the ORIGINAL single-precision expression, not as a double computation
            // narrowed afterwards. The two can differ by one ulp through double rounding, and one
            // ulp of degrees is still a different camera rotation and therefore a different PNG -
            // the whole safety argument for this refactor is that the default path's pixels do
            // not move. The offset branch is skipped entirely when no seed was given.
            Shot.Azimuth = (360.0f * Index) / static_cast<float>(Count);
            if (AzimuthOffsetDegrees != 0.0)
            {
                Shot.Azimuth = static_cast<float>(FMath::Fmod(
                    static_cast<double>(Shot.Azimuth) + AzimuthOffsetDegrees, 360.0));
                if (Shot.Azimuth < 0.0f)
                {
                    Shot.Azimuth += 360.0f;
                }
            }
            Shot.Elevation = Elevation;
            Shot.ProjectionMode = ProjectionMode;
            Shots.Add(Shot);
        }
        return Shots;
    }

    // Azimuth offset in degrees derived from a caller-supplied seed, for jitter ONLY.
    //
    // The contract this exists to keep: without a seed the output is deterministic (offset 0), and
    // WITH a seed it is still deterministic - the same seed always yields the same set. The seed
    // is echoed in the response and accepted back, so a set produced with jitter can be retaken
    // exactly. Randomness is never the default and is never unseedable, because an acceptance shot
    // that cannot be reproduced cannot be compared against.
    inline double SeedToAzimuthOffsetDegrees(int32 Seed)
    {
        // FRandomStream is the engine's own deterministic stream: the same seed gives the same
        // sequence on every machine and every run, which a hash of wall-clock time would not.
        FRandomStream Stream(Seed);
        return static_cast<double>(Stream.FRandRange(0.0f, 360.0f));
    }

    // Everything the `shotDistribution` response block reports, gathered so one serializer emits
    // it and every multi-shot verb can publish it. A struct rather than six positional arguments
    // because two of the six are bools that mean opposite things and would transpose silently.
    struct FShotDistributionPlan
    {
        EShotDistribution Distribution = EShotDistribution::Ring;
        // A seed was supplied BY THE CALLER. Without one the output is still deterministic, so an
        // absent seed is not an absent contract - which is why `seeded` is published either way.
        bool bSeeded = false;
        int32 Seed = 0;
        double AzimuthOffsetDegrees = 0.0;
        // The distribution applies to THIS call's plan. `angles` and `views` name their own poses
        // and neither reads `distribution` at all.
        bool bAppliesToPlan = false;
        // The caller passed `elevation`. Read to decide whether a construction that cannot honour
        // one has to say out loud that it ignored it.
        bool bElevationProvided = false;
        // The elevation the ring plan was BUILT from - the caller's value when they gave one, the
        // verb's documented default when they did not. Published so "the default was 30" is a fact
        // in the response rather than a claim on a wiki page: the ONLY reason this field exists is
        // that a default silently rewritten to something else is indistinguishable, from the
        // caller's side, from the default having held.
        float ElevationDegrees = 0.0f;
        // MEASURED, not predicted: how many of this plan's shots had their elevation moved by the
        // orthographic cardinal-axis snap, counted by comparing each shot's requested angles
        // against the angles that were actually placed. Zero on every perspective plan.
        int32 ElevationSnappedShots = 0;
        // How many cameras the plan named at all, so `elevationSnappedShots: 6` reads as "all of
        // them" rather than as a bare number.
        int32 PlannedShots = 0;
    };

    // Why an `elevation` argument did not reach the pixels, as a stable machine key. Three
    // constructions can each swallow it, and a caller correcting a set needs to know WHICH: the
    // remedy for one is a different `distribution`, for another a different `projectionMode`, and
    // for the third dropping `views` in favour of `count`. One bare `elevationIgnored: true` would
    // have made all three look like the same problem.
    namespace ElevationIgnoredReason
    {
        // distribution:'sphere' derives every elevation from the shot index.
        inline constexpr TCHAR SphereDistribution[]   = TEXT("sphereDistribution");
        // An orthographic editor viewport renders only the six cardinal directions.
        inline constexpr TCHAR OrthographicProjection[] = TEXT("orthographicProjection");
        // `views` names its own poses and never reads `elevation`.
        inline constexpr TCHAR NamedViewPlan[]        = TEXT("namedViewPlan");
    }

    // ---- how the poses were distributed, and the seed that reproduces them ----
    // Published unconditionally so a set can always be retaken: `distribution` names the
    // construction, `seed` is the exact value to pass back, and `seeded` says whether one was in
    // force at all. Lifted verbatim from camera.orbit_shots, which is where it was built and
    // where it was the only copy, so every adopting verb emits the block byte-for-byte as
    // orbit_shots already did rather than growing its own near-miss spelling of it.
    inline TSharedPtr<FJsonObject> MakeShotDistributionObject(const FShotDistributionPlan& Plan)
    {
        TSharedPtr<FJsonObject> DistributionInfo = MakeShared<FJsonObject>();
        DistributionInfo->SetStringField(TEXT("distribution"),
            Plan.Distribution == EShotDistribution::Sphere ? TEXT("sphere") : TEXT("ring"));
        DistributionInfo->SetBoolField(TEXT("seeded"), Plan.bSeeded);
        if (Plan.bSeeded)
        {
            DistributionInfo->SetNumberField(TEXT("seed"), Plan.Seed);
            DistributionInfo->SetNumberField(TEXT("azimuthOffsetDegrees"), Plan.AzimuthOffsetDegrees);
        }
        // Applies only to `count`; `angles` and `views` name their own poses and neither reads
        // `distribution` at all. Said out loud rather than left for the reader to infer from which
        // fields moved.
        DistributionInfo->SetBoolField(TEXT("appliesToPlan"), Plan.bAppliesToPlan);

        // ---- WHAT ELEVATION THE PLAN WAS BUILT FROM, AND WHETHER IT SURVIVED ------------------
        //
        // Published on every ring plan, supplied or not. A caller cannot otherwise tell the
        // documented default holding from the default having been rewritten on the way to the
        // camera: both look like an absent field. `elevationRequestedDegrees` is what the plan
        // asked for; the per-shot `angle.elevation` values are what was placed; and when the two
        // disagree the three fields below name the construction that changed it.
        if (Plan.bAppliesToPlan && Plan.Distribution == EShotDistribution::Ring)
        {
            DistributionInfo->SetNumberField(TEXT("elevationRequestedDegrees"), Plan.ElevationDegrees);
        }
        if (Plan.PlannedShots > 0)
        {
            DistributionInfo->SetNumberField(TEXT("plannedShots"), Plan.PlannedShots);
        }

        // Three constructions can each swallow `elevation`, and until now only the first said so.
        // The other two dropped it in silence, which is the shape rpc-design.md section 21 calls
        // the worst one: the caller receives positive confirmation of an angle that never reached
        // a camera. Ordered most-specific first - the orthographic snap is a MEASURED outcome and
        // outranks the two request-shape deductions, because it is the only one that can also
        // rewrite a default the caller never touched.
        if (Plan.ElevationSnappedShots > 0)
        {
            DistributionInfo->SetBoolField(TEXT("elevationIgnored"), true);
            DistributionInfo->SetStringField(TEXT("elevationIgnoredReason"),
                ElevationIgnoredReason::OrthographicProjection);
            DistributionInfo->SetNumberField(TEXT("elevationSnappedShots"), Plan.ElevationSnappedShots);
            DistributionInfo->SetStringField(TEXT("elevationWarning"), FString::Printf(
                TEXT("`elevation` did NOT reach the camera on %d of %d shots: this is an "
                     "ORTHOGRAPHIC plan, and an orthographic editor viewport renders only the six "
                     "cardinal directions - FEditorViewportClient::CalcSceneView builds the ortho "
                     "view matrix from the viewport TYPE and ignores the camera rotation entirely "
                     "(UE 5.8 EditorViewportClient.cpp:1341-1401), and LVT_OrthoFreelook is a "
                     "seventh fixed matrix rather than a free one. Every shot was therefore snapped "
                     "to the nearest cardinal axis before the camera was placed: |elevation| < 45 "
                     "flattens to 0, >= 45 goes to +/-90. Read each shot's angle.elevation for what "
                     "was actually placed and angle.requestedElevation for what was asked. To hold "
                     "an elevation, use projectionMode:'perspective'; to keep the orthographic "
                     "projection, ask for the elevation the snap would have produced."),
                Plan.ElevationSnappedShots, FMath::Max(Plan.PlannedShots, Plan.ElevationSnappedShots)));
        }
        else if (Plan.Distribution == EShotDistribution::Sphere && Plan.bElevationProvided)
        {
            // The sphere construction derives every elevation from the shot index, so there is
            // nothing for an `elevation` argument to modify. Saying so is the whole difference
            // between a parameter that does not apply and a parameter that was silently dropped -
            // which is the same defect class as this verb's old inert `viewMode`.
            DistributionInfo->SetBoolField(TEXT("elevationIgnored"), true);
            DistributionInfo->SetStringField(TEXT("elevationIgnoredReason"),
                ElevationIgnoredReason::SphereDistribution);
            DistributionInfo->SetStringField(TEXT("elevationWarning"),
                TEXT("`elevation` was supplied and IGNORED: distribution:'sphere' derives each shot's "
                     "elevation from its index by the golden-angle construction, so a single fixed "
                     "elevation has nothing to modify. Use distribution:'ring' to pin every shot to "
                     "one elevation, or `angles` to name each pose."));
        }
        else if (Plan.bElevationProvided && !Plan.bAppliesToPlan)
        {
            // `views` (and `angles`) name their own poses and never read `elevation`. Same silent
            // acceptance as the two above, one request shape over.
            DistributionInfo->SetBoolField(TEXT("elevationIgnored"), true);
            DistributionInfo->SetStringField(TEXT("elevationIgnoredReason"),
                ElevationIgnoredReason::NamedViewPlan);
            DistributionInfo->SetStringField(TEXT("elevationWarning"),
                TEXT("`elevation` was supplied and IGNORED: this shot plan names its own poses, so "
                     "there is no orbiting circle for an elevation to sit on. `elevation` applies to "
                     "`count` only. Use count with distribution:'ring' to pin every shot to one "
                     "elevation."));
        }
        return DistributionInfo;
    }
}
