// Copyright (c) 2026 Alexander Penkin. MIT License.

// render.capture_animation_preview — a frame burst of a skinned mesh IN ISOLATION: no level, no
// placed actor, no Level Sequence. It opens the asset's own Persona-family editor (Skeletal Mesh
// / Animation / Skeleton / Animation Blueprint), poses the preview component at each requested
// instant, and captures that preview viewport.
//
// This is the isolated half of animated review; camera.animation_shots is the in-level half.
// Both return the same per-shot fields and the same pose evidence, so a set moves between them.
// render.capture_asset_preview remains the Static Mesh single-still verb and is untouched.
//
// WHY A SEPARATE VERB RATHER THAN A MODE ON render.capture_asset_preview. That verb returns ONE
// image and a flat set of capture fields. A burst returns `shots[]` crossed over a time axis
// plus a pose verdict, and folding the two together would make the response shape depend on
// whether a `times` argument happened to be present — a mode flag that changes every other
// guarantee of the verb. Extending capture_asset_preview to a second ASSET EDITOR would have
// been fine; extending it to a second RESPONSE SHAPE is not.
//
// THE DETERMINISM CONTRACT, and what each line of it is defending against:
//
//  * SetPlaying(false) BEFORE SetPosition. FAnimPreviewInstanceProxy::Update advances the clock
//    on every preview-scene tick, and the capture path deliberately pumps Slate three times per
//    shot. A playing preview would therefore be at a different time in each of those three
//    pumps and the burst would photograph three unrelated instants.
//  * SetPosition alone does NOT evaluate anything. UAnimSingleNodeInstance::SetPosition only
//    writes the proxy's current time (Engine/Private/Animation/AnimSingleNodeInstance.cpp) —
//    the pose does not move until something ticks the anim instance and refreshes the bone
//    transforms. Both are driven explicitly here rather than left to the preview scene's own
//    tick, so the pose is correct before the first pump rather than one tick later.
//  * bUpdateAnimationInEditor is NOT set, on purpose. That flag gates evaluation only in an
//    EWorldType::Editor world; a preview scene is EWorldType::EditorPreview
//    (Engine/Private/PreviewScene.cpp), and Persona itself never touches the flag. Setting it
//    here would be cargo cult.
//  * The orbit camera is turned OFF for the capture. This is the one that silently ruins the
//    output: a Persona viewport client runs with bUsingOrbitCamera true (it is set from
//    SetCameraFollowMode, whose default mode still enables orbit), and
//    FEditorViewportClient::CalcViewRotationMatrix DISCARDS the camera rotation entirely in that
//    mode, deriving the view from the orbit look-at instead. Every requested azimuth/elevation
//    would be thrown away and every shot in a six-side set would come back from the same angle,
//    with nothing in the response to say so. ToggleOrbitCamera(false) converts the orbit pose to
//    an equivalent free pose, so disabling it does not move the picture.
//
//    THAT SUPPRESSION IS NO LONGER WRITTEN HERE. PinWrightRenderCapture::CaptureEditorViewportToPng
//    performs the identical save / suppress / restore around every single capture
//    (PreviewViewportCaptureUtils.cpp:1412, :1431-1461) and REPORTS it as
//    viewport.aim.orbitCameraAtEntry / orbitCameraSuppressed. This file used to hand-roll a second,
//    outer copy of it; two copies of one restore discipline is how the two disagree. What survives
//    here is the one thing the util cannot know - the flag's value at ENTRY to the whole burst,
//    published as `orbitCameraWasOn`.
//  * The camera is framed from the MESH ASSET's bounds, not the posed component's. Posed bounds
//    change every frame, so framing from them would move the camera under the subject and
//    destroy the very comparison a burst exists to support.
//
// WHAT DRIVES THE SHOTS. Every camera in this burst goes through
// PinWrightPoseCapture::CaptureCameraPoses, the shared pose-list primitive, ONE CALL PER SAMPLED
// INSTANT: the preview is scrubbed and evaluated here, then the whole view plan for that instant is
// handed over as a pose list. That is what buys this verb the throwaway warm-up frame, the honest
// pose bound and the `poseSet` report without a second implementation of any of them. The time axis
// stays in this file because the pose primitive's own time driver takes the SUBJECT step as a
// parameter and this verb's subject step is inseparable from the pose evidence it samples between
// the scrub and the shot.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Render/AnimationPoseEvidence.h"
#include "Handlers/Render/CameraShotPlanUtils.h"
#include "Handlers/Render/CaptureSubject.h"
#include "Handlers/Render/CaptureSubjectProviders_Animation.h"
#include "Handlers/Render/PoseListCapture.h"
#include "Handlers/Render/PreviewSceneRig.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Utils/JsonUtils.h"

#include "Animation/AnimationAsset.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Animation/DebugSkelMeshComponent.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/ScopeExit.h"
#include "SEditorViewport.h"
#include "Slate/SceneViewport.h"

// File-unique named namespace so Unity merges cannot collide these with the same-shaped helpers
// in sibling Render/*.cpp files. The collision this was originally made for - two copies of one
// widget-tree walk under the same name - is gone: both call the shared one now.
namespace PinWrightAnimationPreview
{
    int64 CountMeaningfulPixelChanges(
        const TArray<FColor>& Previous, const TArray<FColor>& Current)
    {
        if (Previous.Num() == 0 || Previous.Num() != Current.Num())
        {
            return 0;
        }

        constexpr int32 ChannelThreshold = 8;
        int64 ChangedPixels = 0;
        for (int32 Index = 0; Index < Previous.Num(); ++Index)
        {
            const FColor& A = Previous[Index];
            const FColor& B = Current[Index];
            const int32 MaxDelta = FMath::Max(
                FMath::Max(FMath::Abs(static_cast<int32>(A.R) - static_cast<int32>(B.R)),
                    FMath::Abs(static_cast<int32>(A.G) - static_cast<int32>(B.G))),
                FMath::Abs(static_cast<int32>(A.B) - static_cast<int32>(B.B)));
            if (MaxDelta > ChannelThreshold)
            {
                ++ChangedPixels;
            }
        }
        return ChangedPixels;
    }

    // Persona-family asset editors. Every one of these is an IHasPersonaToolkit and owns a
    // preview scene with a UDebugSkelMeshComponent in it, which is all this verb needs — so the
    // list is matched by toolkit NAME rather than by casting to any of their interfaces (UE builds
    // with RTTI off, and a static downcast through their multiple-inheritance hierarchies from
    // IAssetEditorInstance* is not a safe thing to write).
    //
    // Handed to PinWrightCaptureSubject::AcquireAssetEditorViewport, which refuses any toolkit not
    // on this list BEFORE it casts anything — the name gate is what makes the one cast it does
    // perform defined. Every entry is also in that resolver's own verified
    // GetSupportedAssetEditorToolkitNames(), which is checked at acquire time, so an entry added
    // here without the engine citation that proves the chain to FAssetEditorToolkit is refused
    // rather than trusted.
    inline TArrayView<const FName> PersonaFamilyToolkitNames()
    {
        static const FName Names[] = {
            FName(TEXT("SkeletalMeshEditor")),
            FName(TEXT("AnimationEditor")),
            FName(TEXT("SkeletonEditor")),
            FName(TEXT("AnimationBlueprintEditor")),
        };
        return MakeArrayView(Names);
    }

    // ---- THE WIDGET-TREE WALK USED TO LIVE HERE, AND IT WAS UNSAFE ----
    //
    // It matched a widget by `GetTypeAsString().EndsWith("EditorViewport")` and then did
    // StaticCastSharedRef<SEditorViewport> on the match, and it reached the toolkit through an
    // unchecked static_cast<FAssetEditorToolkit*>(EditorInstance). SEditorViewport carries no
    // SLATE_DECLARE_WIDGET, so there is no runtime hierarchy check behind either cast: any widget
    // whose type name merely ENDS IN "EditorViewport" was undefined behaviour, and any
    // IAssetEditorInstance that is not an FAssetEditorToolkit was undefined behaviour twice over.
    // Both are now PinWrightCaptureSubject::AcquireAssetEditorViewport, which gates the toolkit on
    // a verified name BEFORE the one cast it performs, and identifies the viewport widget by the
    // engine's own client registry plus an exact-name allow-list of verified SEditorViewport
    // subclasses - refusing an unknown widget type BY NAME instead of casting to it. There was a second, identical copy
    // of this walk in RenderHandler.cpp; the shared one replaces both, which is also why the
    // Niagara preview viewport becomes reachable without a NiagaraEditor symbol
    // (SNiagaraSystemViewport derives from SEditorViewport but does not end in "EditorViewport").
    //
    // Persona registers FOUR preview viewport tabs, so a layout can hold up to four SEditorViewport
    // widgets and the walk returns whichever it reaches first. That is deliberate and safe: all
    // four are views onto the SAME preview scene, and this verb sets the camera itself, so which
    // one is captured does not change the pixels.

    // The preview-component lookup that used to live here is now
    // PinWrightCaptureSubjectAnimation::FindPreviewMeshComponent, with the same signature and the
    // same OutCandidateCount contract. It moved for the same reason the widget walk did: this file
    // and the animation-subject provider had byte-identical copies, and two copies of one rule is
    // how the two stop agreeing about which mesh got photographed.

    // Everything this verb changes on the preview component, so it can be put back. A review
    // verb that leaves an artist's Persona tab scrubbed to a different frame with a different
    // animation loaded is not read-only in any sense that matters.
    struct FPreviewInstanceState
    {
        bool bCaptured = false;
        // Whether the component was in single-node PREVIEW mode at all, as distinct from which
        // asset was loaded. An Animation Blueprint editor's component is normally not, and
        // restoring it as "preview on with a null asset" would leave the artist looking at a
        // reference pose where their graph used to be running.
        bool bPreviewOn = false;
        TWeakObjectPtr<UAnimationAsset> Asset;
        float Position = 0.0f;
        bool bPlaying = false;
        bool bLooping = true;
    };
}

// ---- render.capture_animation_preview ----
REGISTER_RPC_HANDLER("render.capture_animation_preview", "render",
    "Capture a skinned mesh IN ISOLATION at one or more instants of an animation: opens the asset's Persona-family editor, freezes the "
    "preview at each requested frame, and captures that preview viewport from one or more angles. No level, no placed actor, no Level Sequence. "
    "Returns per-shot image statistics and numeric proof of whether the pose changed between instants.",
    RPC_PARAMS(
        RPC_PARAM_OPT("assetPath", "path", "Skeletal Mesh, Anim Sequence, or other animation asset object path. An animation asset supplies both the animation and (through its preview mesh) the mesh. Required unless a `subject` object carries the path instead. The two cannot be combined: a `subject` object does not absorb the legacy key beside it, so passing both is refused naming both, never ranked."),
        RPC_PARAM_OPT("subject", "object", "The capture subject in the shared cross-verb spelling: {kind, path, animation, closeAfterCapture}. Every key is an alternative name for this verb's existing argument of the same meaning - `path` is `assetPath`, `animation` is `animation`, `closeAfterCapture` is `closeAfterCapture` - so it exists for uniformity with the other capture verbs, not for new capability. `kind` is optional and inferred; supplying one this verb cannot serve (staticMesh, world, actor, niagara) is a typed refusal naming the verb that can, never a silent reinterpretation. Passing `subject` alongside a legacy target key such as `assetPath` is refused naming both - drop one."),
        RPC_PARAM_OPT("animation", "path", "Animation asset to preview on the mesh. Required when assetPath is a Skeletal Mesh and you want it posed; omit to capture the bind pose."),
        RPC_PARAM_OPT("frames", "array", "Explicit frame numbers to sample, at the animation's own sampling frame rate. Wins over every other time option."),
        RPC_PARAM_OPT("frameCount", "integer", "Number of instants to sample. Defaults to filling the 24-shot budget at the requested view count, capped at 5."),
        RPC_PARAM_OPT("frameStart", "integer", "First frame of the burst (default 0)."),
        RPC_PARAM_OPT("frameStep", "integer", "Frames between instants. Omit to spread frameCount instants evenly across the animation length."),
        RPC_PARAM_OPT("intervalSeconds", "number", "Seconds between instants, converted to whole frames up front. frameStep wins when both are given."),
        RPC_PARAM_OPT("time", "number", "Single instant in seconds. Shorthand for a one-frame burst; converted to a frame at the animation's sampling rate."),
        RPC_PARAM_OPT("angles", "array", "Explicit camera poses, each {azimuth, elevation} in degrees. Crossed with every sampled instant."),
        RPC_PARAM_OPT("count", "integer", "Number of camera poses per instant. Spread by `distribution`: 'ring' (default) puts them at evenly-spaced azimuths all at 'elevation'; 'sphere' spreads them over the whole viewing sphere. Alternative to angles."),
        RPC_PARAM_DEF("distribution", "string", "How `count` spreads its poses. 'ring' (default, and the pre-existing behaviour) puts them at evenly-spaced azimuths on ONE horizontal circle at `elevation`, which never sees the top or the underside of the mesh - the exact blind spot a limb-penetration or a detached accessory hides in. 'sphere' uses a golden-angle (Fibonacci) spiral over the whole viewing sphere - deterministic, near-optimal even coverage for any N - and IGNORES `elevation`, which the response says out loud rather than dropping silently. Only affects `count`; `angles` and `views` name their own poses.", "ring"),
        RPC_PARAM_OPT("seed", "number", "Jitter the pose distribution by a seeded azimuth offset. Omitted, output is deterministic (offset 0). Supplied, output is STILL deterministic - the same seed always gives the same poses - and the seed is echoed in shotDistribution so a burst can be retaken exactly. There is deliberately no unseeded random mode."),
        RPC_PARAM_OPT("views", "string", "'sides' captures the six axis-aligned views at every instant. Cannot be combined with count or angles."),
        RPC_PARAM_OPT("azimuth", "number", "Single-angle horizontal orbit angle in degrees (default 45)."),
        RPC_PARAM_OPT("elevation", "number", "Vertical orbit angle in degrees above the horizon (default 15)."),
        RPC_PARAM_OPT("padding", "number", "Bounds-fit margin multiplier (default 1.4, wider than the static-mesh default to allow for limb reach outside the rest bounds)."),
        RPC_PARAM_OPT("fov", "number", "Perspective field of view in degrees (default 50)."),
        RPC_PARAM_OPT("projectionMode", "string", "'perspective' (default) or 'orthographic'."),
        RPC_PARAM_OPT("exposure", "object|number", PINWRIGHT_EXPOSURE_PARAM_DESC " Read once for the whole burst, so two instants of the animation are exposed identically and a pose difference cannot be confused with a brightness one."),
        RPC_PARAM_OPT("viewMode", "string", PINWRIGHT_VIEW_MODE_PARAM_DESC " Read once for the whole burst, so every instant is rendered in the same mode, and restored once after the last shot."),
        RPC_PARAM_OPT("previewScene", "object", PINWRIGHT_PREVIEW_SCENE_PARAM_DESC " Read once for the whole burst, so every instant is lit by the same rig and restored once after the last shot - a rig that moved between instants would make a lighting difference and a pose difference read the same."),
        RPC_PARAM_OPT("width", "number", "Output width in pixels. Default 768 for both a burst and a single image."),
        RPC_PARAM_OPT("height", "number", "Output height in pixels. Same default rule as width."),
        RPC_PARAM_OPT("closeAfterCapture", "boolean", "Close the asset editor afterwards. Defaults to TRUE, which closes only a window this call opened; pass true explicitly to close one that was already open, or false to leave it open. The response reports the measured assetEditorClosed / assetEditorWasAlreadyOpen. `subject.closeAfterCapture` is the same argument and keeps all three states; a top-level value wins when both are given."),
        RPC_PARAM_OPT("inline", "boolean", "When true, also embed base64 PNG bytes per shot (default false).")
    ))
{
    using namespace PinWrightCameraFrame;
    using namespace PinWrightAnimationPreview;

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    const bool bHasPayload = Payload.IsValid();
    const auto HasField = [&](const TCHAR* Key) { return bHasPayload && Payload->HasField(Key); };

    // Validated before any asset is loaded or any editor opened, alongside the view plan below:
    // one exposure for the whole burst is what lets two instants be compared. Read through the
    // shared parser so this verb's `exposure` means exactly what every other capture verb's does.
    PinWrightRenderCapture::FExposurePin ExposurePin;
    {
        FString ExposureErrCode;
        FString ExposureErrMsg;
        if (!PinWrightRenderCapture::ParseExposurePin(Payload, ExposurePin,
                ExposureErrCode, ExposureErrMsg))
        {
            Ctx.SendError(ExposureErrCode, ExposureErrMsg);
            return true;
        }
    }

    // ---- the scoped preview-scene rig, read ONCE for the whole burst ----
    //
    // Parsed HERE, beside the exposure pin and before any asset is loaded or any editor opened,
    // rather than beside the view mode further down. ParsePreviewSceneRigPin needs no viewport --
    // unlike ParseViewModePin, which is handed the client so a mode whose companion
    // sub-visualisation is already selected can be let through -- so a malformed `previewScene`
    // is refused BEFORE a window is opened rather than after, and the caller does not pay for an
    // asset-editor open to learn they mistyped a field name.
    //
    // One rig for the whole burst, for the same reason as one exposure: two instants lit
    // differently are not comparable, and a lighting difference and a pose difference would read
    // identically in the per-shot statistics.
    PinWrightPreviewSceneRig::FPreviewSceneRigPin PreviewSceneRigPin;
    {
        FString RigErrCode;
        FString RigErrMsg;
        if (!PinWrightPreviewSceneRig::ParsePreviewSceneRigPin(Payload, PreviewSceneRigPin,
                RigErrCode, RigErrMsg))
        {
            Ctx.SendError(RigErrCode, RigErrMsg);
            return true;
        }
    }

    // ---- the subject, in the shared cross-verb spelling ----
    //
    // Additive. Every key of `subject` this verb reads is an alternative name for an argument it
    // already had, so the legacy spellings stay first-class and nothing on the wire moves. The
    // block is parsed ONLY when it is present: PinWrightCaptureSubject::ParseSubject infers an
    // asset kind from the loaded UClass, and running it on every call would load the asset before
    // the view plan is validated - which is exactly the ordering
    // TestAnimationCaptureHandlers.cpp's RejectsViewsCombinedWithAngles pins.
    PinWrightCaptureSubject::FSubjectRequest SubjectRequest;
    const TSharedPtr<FJsonObject> SubjectObj = Ctx.GetObject(TEXT("subject"));
    const bool bSubjectProvided = SubjectObj.IsValid();
    if (bSubjectProvided)
    {
        FString SubjectErrCode;
        FString SubjectErrMsg;
        if (!PinWrightCaptureSubject::ParseSubject(Payload, SubjectRequest,
                SubjectErrCode, SubjectErrMsg))
        {
            Ctx.SendError(SubjectErrCode, SubjectErrMsg);
            return true;
        }
        // An EXPLICIT kind this verb cannot serve is refused BY NAME, and the refusal points at the
        // verb that can - the same two-way pointer the static-mesh rejection already carries.
        // bKindProvided is what separates that from an INFERRED kind, which is deliberately not
        // checked here: the asset-class resolution below produces the same refusal with a better
        // message, because by then the UClass is known.
        if (SubjectRequest.bKindProvided &&
            SubjectRequest.Kind != PinWrightCaptureSubject::ESubjectKind::SkeletalMesh &&
            SubjectRequest.Kind != PinWrightCaptureSubject::ESubjectKind::Animation)
        {
            Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR,
                FString::Printf(
                    TEXT("subject.kind '%s' is not served by render.capture_animation_preview, which drives a ")
                    TEXT("Persona-family preview and therefore takes only kind 'skeletalMesh' or 'animation'. ")
                    TEXT("Use render.capture_asset_preview for a static mesh or a Niagara system, and ")
                    TEXT("camera.orbit_shots / camera.frame_actor for a world point or a placed actor."),
                    PinWrightCaptureSubject::ToWireName(SubjectRequest.Kind)));
            return true;
        }
    }

    // `assetPath` is the historical spelling and stays the one this verb validates through
    // RequireAssetPath, so its rejection messages do not move. `subject.path` is accepted in its
    // place; both present and DIFFERENT is refused naming both keys rather than ranked, because
    // silently picking one is how a caller ends up reviewing an asset they did not name.
    FString AssetPath;
    const bool bAssetPathProvided = HasField(TEXT("assetPath"));
    if (bAssetPathProvided)
    {
        if (!Ctx.RequireAssetPath(TEXT("assetPath"), AssetPath))
        {
            return true;
        }
        // BACKSTOP, and normally unreachable: PinWrightCaptureSubject::ParseSubject already refuses
        // a `subject` object presented beside ANY legacy target key - `assetPath` included - and it
        // refuses on PRESENCE, not on disagreement, because a `subject` object does not absorb the
        // key beside it. This repeats that rule rather than a weaker one so the two cannot diverge:
        // if the shared list ever stopped covering `assetPath`, silently preferring one of the two
        // is how a caller reviews an asset they did not name.
        if (bSubjectProvided)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("Ambiguous capture subject: 'subject' and 'assetPath' both name a target, and a "
                     "`subject` object does not absorb the legacy key beside it. Pass one - drop "
                     "'assetPath' to capture the subject, or drop 'subject' to keep the legacy target."));
            return true;
        }
    }
    else if (!SubjectRequest.AssetPath.IsEmpty())
    {
        AssetPath = SubjectRequest.AssetPath;
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_REQUIRED_PARAM,
            TEXT("Missing required string field: assetPath (or subject.path). Pass a Skeletal Mesh, "
                 "an Anim Sequence, or another animation asset object path."));
        return true;
    }
    if (!GEditor)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
        return true;
    }

    // ---- pose distribution + seeded jitter ----
    // Pure pose-generation inputs, resolved above the view plan because the plan is the only thing
    // they touch. `ring` is the default and reproduces the previous behaviour byte-for-byte, so no
    // existing caller's pixels move.
    const FString DistributionArg = Ctx.GetString(TEXT("distribution"), TEXT("ring")).ToLower();
    EShotDistribution Distribution = EShotDistribution::Ring;
    if (DistributionArg == TEXT("ring"))
    {
        Distribution = EShotDistribution::Ring;
    }
    else if (DistributionArg == TEXT("sphere"))
    {
        Distribution = EShotDistribution::Sphere;
    }
    else
    {
        // Rejected rather than defaulted: a typo that quietly becomes the default is
        // indistinguishable from the default having been asked for.
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Unrecognised distribution '%s'. Valid: ring (default), sphere."),
                *DistributionArg));
        return true;
    }
    const bool bSeedProvided = HasField(TEXT("seed"));
    const int32 Seed = Ctx.GetInt(TEXT("seed"), 0);
    const double AzimuthOffsetDegrees = bSeedProvided ? SeedToAzimuthOffsetDegrees(Seed) : 0.0;

    // ---- `hideEditorSprites` is deliberately NOT a parameter here, and is not read either ----
    //
    // Verified against the engine on 2026-08-21, and the reason matters because the shared header's
    // old one-liner ("no AActor anywhere") is FALSE for Persona: FPersonaToolkit spawns an
    // AAnimationEditorPreviewActor into the preview world for every preview (PersonaToolkit.cpp:163),
    // and toggling wind on spawns an AWindDirectionalSource whose editor-only UArrowComponent
    // (bTreatAsASprite) and UBillboardComponent DO gate on EngineShowFlags.BillboardSprites
    // (AnimationEditorPreviewScene.cpp:1121, :1166-1180; ArrowComponent.cpp:170;
    // BillboardComponent.cpp:214), which the Persona viewport client leaves at its editor default of
    // on (AnimationEditorViewportClient.cpp:217-240 never touches that flag).
    //
    // So the flag is not inert here - and it is still not offered, for a different reason. The only
    // thing it could ever hide in this scene is a user-toggled wind gizmo; the light bulbs, audio
    // icons, player start and note icons its shared wire description promises do not exist in a
    // preview world at all. Publishing a knob whose stated purpose is unreachable is worse than not
    // publishing it, so the parameter is undeclared and the dispatcher's unknown-param gate refuses
    // it BY NAME rather than this file reading it and silently doing nothing.
    // See PreviewViewportCaptureUtils.h above PINWRIGHT_HIDE_EDITOR_SPRITES_PARAM_DESC.

    // ---- View plan. Identical shapes to camera.animation_shots, and validated BEFORE any asset
    // is loaded or any editor opened: a malformed request must not leave a Persona tab open on
    // its way to being rejected. ----
    const float DefaultElevation = static_cast<float>(Ctx.GetNumber(TEXT("elevation"), 15.0));
    const bool bElevationProvided = HasField(TEXT("elevation"));
    const float Fov = static_cast<float>(Ctx.GetNumber(TEXT("fov"), 50.0));
    const float Padding = static_cast<float>(Ctx.GetNumber(TEXT("padding"), 1.4));
    const bool bInline = Ctx.GetBool(TEXT("inline"), false);

    const bool bProjectionProvided = HasField(TEXT("projectionMode"));
    const FString RequestedProjection =
        Ctx.GetString(TEXT("projectionMode"), TEXT("perspective")).ToLower();
    if (bProjectionProvided && !IsValidProjectionMode(RequestedProjection))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("projectionMode must be 'perspective' or 'orthographic'"));
        return true;
    }
    if (Fov <= 0.0f)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("fov must be greater than zero"));
        return true;
    }

    const bool bViewsProvided = HasField(TEXT("views"));
    const FString Views = Ctx.GetString(TEXT("views")).ToLower();
    const bool bCountProvided = HasField(TEXT("count"));
    const TArray<TSharedPtr<FJsonValue>>* Angles = Ctx.GetArray(TEXT("angles"));

    TArray<FPlannedShot> ViewPlan;
    if (bViewsProvided)
    {
        if (bCountProvided || (Angles && Angles->Num() > 0))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("views cannot be combined with count or angles; pass one view plan"));
            return true;
        }
        if (Views != TEXT("sides"))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("views must be 'sides' (the six axis-aligned views); use count/angles for arbitrary poses"));
            return true;
        }
        // ONE table, shared with camera.orbit_shots and camera.animation_shots. The literals used
        // to be written out here as a third copy; three copies of a six-row table are three
        // chances for one of them to be reordered, and a reordered `sides` set is six individually
        // correct images that no longer line up with the archived set they are compared against.
        const FString SideProjection = bProjectionProvided ? RequestedProjection : TEXT("orthographic");
        ViewPlan = MakeSideViews(SideProjection);
    }
    else if (Angles && Angles->Num() > 0)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Angles)
        {
            const TSharedPtr<FJsonObject>* AngleObj = nullptr;
            if (Value.IsValid() && Value->TryGetObject(AngleObj) && AngleObj && (*AngleObj).IsValid())
            {
                FPlannedShot Shot;
                Shot.Azimuth = static_cast<float>(GetJsonNumberField(*AngleObj, TEXT("azimuth"), 0.0));
                Shot.Elevation = static_cast<float>(GetJsonNumberField(*AngleObj, TEXT("elevation"), DefaultElevation));
                Shot.ProjectionMode = RequestedProjection;
                ViewPlan.Add(Shot);
            }
        }
        if (ViewPlan.Num() == 0)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("angles must be a non-empty array of {azimuth, elevation} objects"));
            return true;
        }
    }
    else if (bCountProvided)
    {
        const int32 Count = Ctx.GetInt(TEXT("count"), 4);
        if (Count < 1)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("count must be at least 1"));
            return true;
        }
        // MakeRingDistribution with no seed is the previous expression verbatim - the same
        // single-precision `(360.0f * Index) / Count`, not a double narrowed afterwards - so the
        // default path's poses, and therefore its pixels, do not move.
        ViewPlan = (Distribution == EShotDistribution::Sphere)
            ? MakeSphereDistribution(Count, AzimuthOffsetDegrees, RequestedProjection)
            : MakeRingDistribution(Count, DefaultElevation, AzimuthOffsetDegrees, RequestedProjection);
    }
    else
    {
        ViewPlan.Add(FPlannedShot{
            static_cast<float>(Ctx.GetNumber(TEXT("azimuth"), 45.0)), DefaultElevation, RequestedProjection});
    }

    UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
    if (!Asset)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
            FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
        return true;
    }

    // ---- Resolve the (mesh, animation) pair from whichever end the caller supplied. ----
    USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Asset);
    UAnimationAsset* AnimationAsset = Cast<UAnimationAsset>(Asset);

    // `subject.animation` is the same argument under the shared spelling. Both present and
    // different is refused naming both keys, for the same reason assetPath / subject.path is.
    FString AnimationPath = Ctx.GetString(TEXT("animation"));
    if (!SubjectRequest.AnimationPath.IsEmpty())
    {
        if (!AnimationPath.IsEmpty() &&
            !SubjectRequest.AnimationPath.Equals(AnimationPath, ESearchCase::IgnoreCase))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(
                    TEXT("'animation' and 'subject.animation' name different assets ('%s' vs '%s'). They are ")
                    TEXT("two spellings of one argument; pass one."),
                    *AnimationPath, *SubjectRequest.AnimationPath));
            return true;
        }
        AnimationPath = SubjectRequest.AnimationPath;
    }
    if (!AnimationPath.IsEmpty())
    {
        UAnimationAsset* Requested = LoadObject<UAnimationAsset>(nullptr, *AnimationPath);
        if (!Requested)
        {
            Ctx.SendError(ErrorCodes::ERR_ANIMATION_NOT_FOUND,
                FString::Printf(TEXT("Animation asset not found: %s"), *AnimationPath));
            return true;
        }
        AnimationAsset = Requested;
    }

    if (!SkeletalMesh && AnimationAsset)
    {
        // bFindIfNotSet=true falls back to the skeleton's preview mesh, which is what Persona
        // itself opens the animation with.
        SkeletalMesh = AnimationAsset->GetPreviewMesh(/*bFindIfNotSet=*/true);
    }
    if (!SkeletalMesh && !AnimationAsset)
    {
        Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR,
            FString::Printf(
                TEXT("'%s' is a %s. render.capture_animation_preview takes a Skeletal Mesh or an animation asset; ")
                TEXT("use render.capture_asset_preview for a Static Mesh."),
                *AssetPath, *Asset->GetClass()->GetName()));
        return true;
    }

    // A mismatched pairing produces a mesh posed by a skeleton it was not bound to — limbs in
    // the wrong places, and a picture that looks like a rigging defect. Refuse rather than
    // photograph it.
    if (SkeletalMesh && AnimationAsset && AnimationAsset->GetSkeleton() &&
        SkeletalMesh->GetSkeleton() && AnimationAsset->GetSkeleton() != SkeletalMesh->GetSkeleton())
    {
        Ctx.SendError(ErrorCodes::ERR_SKELETON_MISMATCH,
            FString::Printf(
                TEXT("Animation '%s' targets skeleton '%s' but mesh '%s' is bound to '%s'."),
                *AnimationAsset->GetPathName(),
                *AnimationAsset->GetSkeleton()->GetPathName(),
                *SkeletalMesh->GetPathName(),
                *SkeletalMesh->GetSkeleton()->GetPathName()));
        return true;
    }

    // ---- Time plan, in FRAMES at the animation's own sampling rate. Seconds are converted once,
    // here, so a burst sits on the animation's key grid instead of between two of its samples. ----
    const UAnimSequenceBase* SequenceBase = Cast<UAnimSequenceBase>(AnimationAsset);
    const double LengthSeconds = SequenceBase ? static_cast<double>(SequenceBase->GetPlayLength()) : 0.0;
    // An animation asset with no sampling grid (a blend space, or a montage whose rate does not
    // resolve) has nothing to quantise frames against. 30 fps is the conventional editor
    // fallback; it is echoed back as frameRate with frameRateAssumed:true so the caller can see
    // the grid was assumed rather than read off the asset.
    const bool bFrameRateIsAssetOwn = SequenceBase && SequenceBase->GetSamplingFrameRate().IsValid();
    const double FrameRate = bFrameRateIsAssetOwn
        ? SequenceBase->GetSamplingFrameRate().AsDecimal()
        : 30.0;
    const double LengthFrames = LengthSeconds * FrameRate;

    const bool bFramesProvided = HasField(TEXT("frames"));
    const bool bTimeProvided = HasField(TEXT("time"));
    const bool bFrameCountProvided = HasField(TEXT("frameCount"));
    const bool bFrameStepProvided = HasField(TEXT("frameStep"));
    const bool bIntervalProvided = HasField(TEXT("intervalSeconds"));

    const int32 DefaultFrameCount =
        FMath::Clamp(GMaxOrbitShots / FMath::Max(ViewPlan.Num(), 1), 1, 5);
    const int32 FrameCount = bFrameCountProvided ? Ctx.GetInt(TEXT("frameCount"), 5) : DefaultFrameCount;
    if (FrameCount < 1)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("frameCount must be at least 1"));
        return true;
    }

    TArray<double> FramePlan;
    FString FramePlanSource;
    if (bFramesProvided)
    {
        const TArray<TSharedPtr<FJsonValue>>* FrameValues = Ctx.GetArray(TEXT("frames"));
        if (FrameValues)
        {
            for (const TSharedPtr<FJsonValue>& Value : *FrameValues)
            {
                double FrameNumber = 0.0;
                if (Value.IsValid() && Value->TryGetNumber(FrameNumber) && FMath::IsFinite(FrameNumber))
                {
                    FramePlan.Add(FrameNumber);
                }
            }
        }
        if (FramePlan.Num() == 0)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("frames must be a non-empty array of finite frame numbers"));
            return true;
        }
        FramePlanSource = TEXT("frames");
    }
    else if (bTimeProvided)
    {
        const double TimeSeconds = Ctx.GetNumber(TEXT("time"), 0.0);
        if (!FMath::IsFinite(TimeSeconds))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("time must be a finite number of seconds"));
            return true;
        }
        FramePlan.Add(FMath::RoundToDouble(TimeSeconds * FrameRate));
        FramePlanSource = TEXT("time");
    }
    else
    {
        const double StartFrame = Ctx.GetNumber(TEXT("frameStart"), 0.0);
        double StepFrames = 0.0;
        if (bFrameStepProvided)
        {
            StepFrames = Ctx.GetNumber(TEXT("frameStep"), 0.0);
            FramePlanSource = TEXT("frameStep");
        }
        else if (bIntervalProvided)
        {
            StepFrames = FMath::RoundToDouble(FrameRate * Ctx.GetNumber(TEXT("intervalSeconds"), 0.0));
            FramePlanSource = TEXT("intervalSeconds");
        }
        else if (LengthFrames > 0.0 && FrameCount > 1)
        {
            // Endpoint-EXCLUSIVE across the animation length: a cyclic clip's last frame repeats
            // its first, and spending a fifth of a five-shot budget on a duplicate pose is worse
            // than losing the terminal frame. Pass `frames` when the terminal pose is the point.
            StepFrames = FMath::Max(
                FMath::RoundToDouble(LengthFrames / static_cast<double>(FrameCount)), 1.0);
            FramePlanSource = TEXT("animationLength");
        }
        else
        {
            StepFrames = 1.0;
            FramePlanSource = (LengthFrames > 0.0) ? TEXT("animationLength") : TEXT("singleFrame");
        }
        if (!FMath::IsFinite(StepFrames))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("frameStep / intervalSeconds must be finite"));
            return true;
        }
        for (int32 Index = 0; Index < FrameCount; ++Index)
        {
            FramePlan.Add(StartFrame + StepFrames * static_cast<double>(Index));
        }
    }

    // With no animation there is exactly one pose to photograph — the bind pose — so a
    // multi-instant plan would be N identical images charged to the caller's budget.
    if (!AnimationAsset && FramePlan.Num() > 1)
    {
        FramePlan.SetNum(1);
        FramePlanSource = TEXT("bindPose");
    }

    const int32 TotalShots = FramePlan.Num() * ViewPlan.Num();
    if (TotalShots > GMaxOrbitShots)
    {
        Ctx.SendError(ErrorCodes::ERR_TOO_MANY_SHOTS,
            FString::Printf(
                TEXT("%d instants x %d angles = %d shots exceeds the maximum of %d. Reduce frameCount, ")
                TEXT("reduce the view plan, or split the burst across calls."),
                FramePlan.Num(), ViewPlan.Num(), TotalShots, GMaxOrbitShots));
        return true;
    }

    // ---- Resolution: resolved once, held for the whole burst. ----
    const bool bWidthProvided = HasField(TEXT("width"));
    const bool bHeightProvided = HasField(TEXT("height"));
    const int32 DefaultEdge = (TotalShots > 1) ? GMultiImageBudgetEdge : GSingleStillEdge;
    const EResolutionSource DefaultSource =
        (TotalShots > 1) ? EResolutionSource::Budget : EResolutionSource::SingleStill;
    const int32 Width = Ctx.GetInt(TEXT("width"), DefaultEdge);
    const int32 Height = Ctx.GetInt(TEXT("height"), DefaultEdge);
    if (!AreDimensionsValid(Width, Height))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("width and height must be in (0, %d]"), GMaxCaptureDimension));
        return true;
    }

    // ---- Open the asset editor and find its preview viewport. ----
    //
    // Same three-state close contract as render.capture_asset_preview, for the same two reasons: a
    // per-call window leak is nobody's intent, and an asset editor still open at exit crashes UE
    // 5.8 in ~FStaticMeshEditor / its Persona siblings (docs/lessons.md). The default closes only a
    // window this call opened; an EXPLICIT closeAfterCapture:true closes one that was already open,
    // which the old !bWasAlreadyOpen guard silently refused to do.
    //
    // `subject.closeAfterCapture` is the same argument under the shared spelling, and it has to be
    // read for EXPLICITNESS as well as for value: bCloseAfterCapture defaults to true, so an
    // explicit subject.closeAfterCapture:true would otherwise be indistinguishable from the default
    // and would silently lose the third state. FSubjectRequest carries the flag that says which it
    // was; a top-level value wins when both are given.
    const bool bCloseAfterCaptureAtTopLevel = HasField(TEXT("closeAfterCapture"));
    const bool bCloseRequestedExplicitly =
        bCloseAfterCaptureAtTopLevel || SubjectRequest.bCloseAfterCaptureProvided;
    const bool bCloseAfterCapture =
        (!bCloseAfterCaptureAtTopLevel && SubjectRequest.bCloseAfterCaptureProvided)
            ? SubjectRequest.bCloseAfterCapture
            : Ctx.GetBool(TEXT("closeAfterCapture"), true);

    // ONE shared acquisition. It opens (or finds) the editor, gates the toolkit by NAME before any
    // cast, walks the widget tree with the verified-type rule, and hands back the client and the
    // scene viewport. It replaces this file's own copy of that walk, which cast off a
    // GetTypeAsString().EndsWith("EditorViewport") match and cast IAssetEditorInstance* to
    // FAssetEditorToolkit* with nothing checking either.
    PinWrightCaptureSubject::FAssetEditorViewportAcquisition Acquisition;
    FString AcquireErrCode;
    FString AcquireErrMsg;
    const bool bAcquired = PinWrightCaptureSubject::AcquireAssetEditorViewport(
        Asset, PersonaFamilyToolkitNames(), Acquisition, AcquireErrCode, AcquireErrMsg);

    // Installed AFTER the acquire and BEFORE the first return that follows it: the acquire is what
    // may have opened the window, and it measures bWasAlreadyOpen before opening, so the flag is
    // valid on its failure paths too. Without this ordering a refused toolkit would leak the window
    // it just opened - and a leaked asset editor is the shutdown-crash precondition.
    const bool bWasAlreadyOpen = Acquisition.bWasAlreadyOpen;
    bool bCloseHandled = false;
    bool bAssetEditorClosed = false;
    const auto CloseAssetEditorIfRequested = [&]()
    {
        if (bCloseHandled)
        {
            return;
        }
        bCloseHandled = true;
        // The acquisition's own references to the preview viewport die BEFORE the close, never
        // after. Closing an asset editor destroys its SEditorViewport, and that destructor asserts
        // check(SceneViewport.IsUnique()) (UE 5.8 SEditorViewport.cpp:65) - so a live
        // Acquisition.SceneViewport at this instant aborts the process rather than leaking.
        //
        // This USED to survive by accident: Acquisition.ViewportWidget kept the widget alive past
        // the close, so ~SEditorViewport ran later, at a point where the function's own copies
        // happened to be gone already. That is a property of declaration order in a 600-line
        // function, not a guarantee, and reordering two locals would have turned it into the same
        // crash camera.orbit_shots hit. Dropped explicitly instead. Nothing below reads them: the
        // captures are finished, and the `subject` block is built from measured values.
        Acquisition.ViewportClient = nullptr;
        Acquisition.SceneViewport.Reset();
        Acquisition.ViewportWidget.Reset();
        // Returns the MEASURED outcome - an asset editor can veto its own close, and a close
        // reported but not performed leaves the crash precondition in place while the response says
        // it is gone.
        bAssetEditorClosed = PinWrightCaptureSubject::CloseAssetEditor(
            Asset, bCloseAfterCapture, bCloseRequestedExplicitly, bWasAlreadyOpen);
    };
    ON_SCOPE_EXIT { CloseAssetEditorIfRequested(); };

    if (!bAcquired)
    {
        // The shared refusal names the toolkit and every widget type it declined to cast. This verb
        // adds the one thing only it knows: which OTHER verb serves the asset the caller named.
        // That pointer is half of the two-way link between the capture surfaces - without it an
        // agent told "not a Persona editor" concludes isolated capture does not exist for its asset.
        const bool bToolkitRefusal =
            AcquireErrCode == ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR;
        Ctx.SendError(AcquireErrCode, bToolkitRefusal
            ? AcquireErrMsg + TEXT(" render.capture_animation_preview drives the Persona family ")
                              TEXT("(SkeletalMeshEditor, AnimationEditor, SkeletonEditor, ")
                              TEXT("AnimationBlueprintEditor); use render.capture_asset_preview for a Static Mesh.")
            : AcquireErrMsg);
        return true;
    }

    const FName EditorName = Acquisition.ToolkitName;
    // NON-const, and deliberately so: both are cleared immediately before the close below, because
    // a copy of the preview viewport that outlives the close is what makes ~SEditorViewport's
    // check(SceneViewport.IsUnique()) fire (UE 5.8 SEditorViewport.cpp:65).
    FEditorViewportClient* ViewportClient = Acquisition.ViewportClient;
    TSharedPtr<FSceneViewport> SceneViewport = Acquisition.SceneViewport;

    int32 PreviewComponentCount = 0;
    UDebugSkelMeshComponent* PreviewComponent =
        PinWrightCaptureSubjectAnimation::FindPreviewMeshComponent(
            *ViewportClient, SkeletalMesh, PreviewComponentCount);
    if (!PreviewComponent)
    {
        Ctx.SendError(ErrorCodes::ERR_SKELETAL_MESH_NOT_FOUND,
            FString::Printf(
                TEXT("The '%s' editor for '%s' has no preview mesh component. Pass a Skeletal Mesh assetPath plus ")
                TEXT("an 'animation', or set a preview mesh on the skeleton first."),
                *EditorName.ToString(), *AssetPath));
        return true;
    }
    if (!SkeletalMesh)
    {
        SkeletalMesh = PreviewComponent->GetSkeletalMeshAsset();
    }
    if (!SkeletalMesh)
    {
        Ctx.SendError(ErrorCodes::ERR_SKELETAL_MESH_NOT_FOUND,
            FString::Printf(TEXT("No skeletal mesh could be resolved for '%s'"), *AssetPath));
        return true;
    }

    // ---- Freeze the preview, saving everything we touch. ----
    FPreviewInstanceState SavedState;
    SavedState.bPreviewOn = PreviewComponent->IsPreviewOn();
    if (UAnimSingleNodeInstance* Existing = PreviewComponent->GetSingleNodeInstance())
    {
        SavedState.bCaptured = true;
        SavedState.Asset = Existing->GetAnimationAsset();
        SavedState.Position = Existing->GetCurrentTime();
        SavedState.bPlaying = Existing->IsPlaying();
        SavedState.bLooping = Existing->IsLooping();
    }
    ON_SCOPE_EXIT
    {
        // Runs unconditionally, not only when a previous instance was captured: this verb turns
        // preview mode ON below, so a component that had none must be put back to having none.
        if (PreviewComponent)
        {
            UAnimationAsset* PreviousAsset = SavedState.Asset.Get();
            PreviewComponent->EnablePreview(SavedState.bPreviewOn, PreviousAsset);
            if (SavedState.bCaptured)
            {
                if (UAnimSingleNodeInstance* Restored = PreviewComponent->GetSingleNodeInstance())
                {
                    Restored->SetLooping(SavedState.bLooping);
                    Restored->SetPosition(SavedState.Position, /*bFireNotifies=*/false);
                    Restored->SetPlaying(SavedState.bPlaying);
                }
            }
        }
    };

    // bEnable is unconditionally true, including when no animation was supplied: EnablePreview
    // installs the single-node preview instance and hands it the asset, and a NULL asset is how
    // the engine's own preview shows the reference pose. Passing false instead would leave
    // whatever instance the editor already had running (an Animation Blueprint, say), which is
    // not the bind pose and is not deterministic.
    PreviewComponent->EnablePreview(true, AnimationAsset);
    UAnimSingleNodeInstance* SingleNode = PreviewComponent->GetSingleNodeInstance();
    if (AnimationAsset && !SingleNode)
    {
        Ctx.SendError(ErrorCodes::ERR_PREVIEW_NOT_FOUND,
            FString::Printf(
                TEXT("Preview component for '%s' has no single-node animation instance, so the animation cannot be ")
                TEXT("frozen at a given time and a burst would not be deterministic."),
                *AssetPath));
        return true;
    }
    if (SingleNode)
    {
        // Order matters: EnablePreview -> SetAnimationAsset restarts playback, so stopping has to
        // come after it, and before any position is written.
        SingleNode->SetPlaying(false);
    }

    // ---- Camera: framed once from the MESH ASSET's bounds so it is identical at every instant. ----
    const FBoxSphereBounds MeshBounds = SkeletalMesh->GetBounds();
    const FVector Center = MeshBounds.Origin;
    const float BoundsRadius = FMath::Max(static_cast<float>(MeshBounds.SphereRadius), 1.0f);
    const float BaseDistance = ComputeFitDistance(BoundsRadius, Fov, Padding);

    // Orbit mode discards the camera rotation entirely (see the file header), and the suppression
    // plus the full pose/pivot restore is performed by CaptureEditorViewportToPng around EVERY
    // capture (PreviewViewportCaptureUtils.cpp:1412, :1431-1461). This file used to hand-roll a
    // second, outer copy of the same save/suppress/restore; that copy is gone, because two
    // implementations of one restore discipline is how the two disagree — and the outer one also
    // masked the per-shot verdict, since every capture then saw orbit already off and reported
    // orbitCameraSuppressed:false for a viewport that genuinely was orbiting.
    //
    // What survives is the one thing the per-capture layer cannot know: the flag's value at ENTRY
    // to the whole burst. bUsingOrbitCamera is a public member of FEditorViewportClient on UE 5.8
    // (Editor/UnrealEd/Public/EditorViewportClient.h:2031). It is read from the flag rather than
    // inferred from the pose moving, because ToggleOrbitCamera no-ops when orbit is already off, so
    // a genuinely-suppressed orbit whose converted free pose happened to land on the same location
    // and rotation would read as "never orbiting".
    const bool bOrbitCameraWasOn = ViewportClient->bUsingOrbitCamera;

    // ---- Scoped view mode, read ONCE for the whole burst ----
    //
    // Parsed here rather than with the other arguments because ResolveForClient is given the
    // viewport that will render: that is what lets a mode whose companion sub-visualisation is
    // already selected on the Persona viewport through instead of refusing it. The cost is that a
    // malformed viewMode is refused after the asset editor has opened — which the three-state
    // close guard above already covers, since it runs on the error path too.
    PinWrightRenderCapture::FViewModePin ViewModePin;
    {
        FString ViewModeErrCode;
        FString ViewModeErrMsg;
        if (!PinWrightRenderCapture::ParseViewModePin(Payload, ViewModePin,
                ViewModeErrCode, ViewModeErrMsg, ViewportClient))
        {
            Ctx.SendError(ViewModeErrCode, ViewModeErrMsg);
            return true;
        }
    }

    // ---- Capture ----
    //
    // ONE call to the shared pose-list primitive for the whole burst: the view plan crossed with
    // the time plan, FRAME-MAJOR, so shots[] keeps the order it has always had
    // (frameIndex * viewCount + viewIndex) and no archived set re-indexes. Everything the
    // hand-rolled nested loop used to do per shot — applying the camera, suppressing orbit, the
    // exposure pin, the scoped view mode, sprite suppression, the aim verification, the blank
    // check — belongs to that primitive and to CaptureEditorViewportToPng under it, so a fix there
    // now reaches this verb instead of stopping one file short of it. What this verb keeps is the
    // one step no other verb has: the SUBJECT step, handed over as a time setter.
    const FString BurstStamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));

    TArray<PinWrightAnimationPose::FPoseSample> PoseSamples;
    PoseSamples.SetNum(FramePlan.Num());
    TArray<bool> InstantSampled;
    InstantSampled.Init(false, FramePlan.Num());
    TArray<double> AchievedTimes;
    AchievedTimes.Init(0.0, FramePlan.Num());
    TArray<bool> AchievedTimeMeasured;
    AchievedTimeMeasured.Init(false, FramePlan.Num());
    int32 UnsampledPoseCount = 0;

    // The subject step: scrub, evaluate, sample. In that order and all three explicitly, because
    // SetPosition only writes the clock (UAnimSingleNodeInstance::SetPosition) — the pose does not
    // move until something ticks the anim instance and refreshes the bone transforms, and leaving
    // that to the preview scene's own tick would photograph the previous instant.
    //
    // Idempotent by construction. The primitive drives the time once per POSE, so an instant
    // captured from six angles is scrubbed six times; re-scrubbing to the same instant is free of
    // consequence for an animation (unlike a simulation advance), and the pose evidence is sampled
    // only the first time an instant is entered so `poseSampled` counts instants, not shots.
    const auto ApplyInstant = [&](int32 FrameIndex, double TimeSeconds,
                                  FString& OutErrCode, FString& OutErrMsg) -> bool
    {
        if (SingleNode)
        {
            SingleNode->SetPosition(static_cast<float>(TimeSeconds), /*bFireNotifies=*/false);
            // TickAnimation runs the anim instance at the new time; RefreshBoneTransforms(nullptr)
            // evaluates the skeleton synchronously on the game thread (a null tick function is what
            // forces the non-threaded path — it is what UDebugSkelMeshComponent does internally).
            PreviewComponent->TickAnimation(0.0f, /*bNeedsValidRootMotion=*/false);
            // The time the instance actually landed on, which is not always the one asked for: a
            // position past the end of the asset is clamped. Measured, never echoed.
            AchievedTimes[FrameIndex] = SingleNode->GetCurrentTime();
            AchievedTimeMeasured[FrameIndex] = true;
        }
        PreviewComponent->RefreshBoneTransforms(nullptr);
        // RefreshBoneTransforms leaves the result in the editable component-space buffer when
        // the normal tick pipeline is bypassed. Finalize flips it for the render thread;
        // without this, pose telemetry moves but the PNG stays at bind pose.
        PreviewComponent->FinalizeBoneTransform();
        // Manual scrubs all happen before the editor advances GFrameCounter. UE's skeletal update
        // channel can coalesce those queued dynamic-data updates, so a dynamic-data dirty mark
        // alone can render the previous pose. Recreate the proxy for this isolated capture.
        PreviewComponent->MarkRenderStateDirty();
        if (!PinWrightCaptureSubjectAnimation::FlushPoseRenderState(
                *PreviewComponent, OutErrCode, OutErrMsg))
        {
            OutErrMsg = FString::Printf(
                TEXT("Frame index %d could not be synchronized for capture: %s"),
                FrameIndex, *OutErrMsg);
            return false;
        }

        if (!InstantSampled[FrameIndex])
        {
            InstantSampled[FrameIndex] = true;
            if (!PinWrightAnimationPose::SamplePose(PreviewComponent, PoseSamples[FrameIndex]))
            {
                ++UnsampledPoseCount;
            }
        }
        return true;
    };

    // A time axis exists only when there is an animation to scrub AND a single-node instance to
    // scrub it on. Without one there is exactly one pose to photograph, the bind pose, and the
    // poses must carry NO time: the primitive refuses a pose time with no setter bound, and it is
    // right to — a dropped instant is a valid PNG of the wrong moment.
    const bool bTimeAxis = (AnimationAsset != nullptr) && (SingleNode != nullptr);

    PinWrightPoseCapture::FPoseListCaptureRequest PoseRequest;
    PoseRequest.Width = Width;
    PoseRequest.Height = Height;
    PoseRequest.FilenamePrefix = TEXT("AnimPreview");
    PoseRequest.Subdirectory = TEXT("AnimPreview");
    PoseRequest.Exposure = ExposurePin;
    PoseRequest.ViewMode = ViewModePin;
    PoseRequest.bRetainPixels = true;
    // Applied and restored ONCE around the whole pose list by the primitive, not per shot -- see
    // the parse site above for why the burst gets one rig.
    PoseRequest.PreviewSceneRig = PreviewSceneRigPin;
    // This verb has allowed 24 shots (instants x angles) since it shipped and the primitive's own
    // default is 8. Keeping the verb's own bound is what stops the conversion silently shortening
    // bursts callers have been taking for months. TotalShots is already refused above the same
    // ceiling, so this never actually truncates; the value in force is published as
    // poseSet.maxPosesPerCall so the bound is never implicit.
    PoseRequest.MaxPoses = GMaxOrbitShots;
    // Bounds from the MESH ASSET, never the posed component — see the file header. Posed bounds
    // change every frame, so a framing verdict measured against them would move under the subject
    // and "the mesh left the frame" and "the bounds grew" would read the same.
    PoseRequest.BoundsOrigin = Center;
    PoseRequest.BoundsRadius = static_cast<double>(MeshBounds.SphereRadius);

    // Called ONCE PER POSE, in pose order, by the primitive — including once for pose 0 before the
    // throwaway warm-up frame, and never twice for the same pose. That contract is what makes this
    // cursor exact: the poses are laid out frame-major, so the instant is the pose index divided by
    // the view count. Deriving the instant from the time value handed in instead would mis-index a
    // `frames` list that names the same frame twice, which is a legal request.
    int32 SubjectDriveCursor = 0;
    if (bTimeAxis)
    {
        PoseRequest.SubjectTimeSetter =
            [&ApplyInstant, &SubjectDriveCursor, ViewCount = ViewPlan.Num(),
             InstantCount = FramePlan.Num()](
                double TimeSeconds, FString& OutErrCode, FString& OutErrMsg) -> bool
        {
            const int32 FrameIndex = FMath::Clamp(
                SubjectDriveCursor / FMath::Max(ViewCount, 1), 0, FMath::Max(InstantCount - 1, 0));
            ++SubjectDriveCursor;
            if (!ApplyInstant(FrameIndex, TimeSeconds, OutErrCode, OutErrMsg))
            {
                return false;
            }
            // SetPosition clamps rather than refusing, and the clamped result is reported as
            // achievedTime. A failed POSE SAMPLE is not a failure of the set either: it is
            // reported as poseSampled:false plus a warning after a real image is captured.
            OutErrCode.Reset();
            OutErrMsg.Reset();
            return true;
        };
    }

    PoseRequest.Poses.Reserve(TotalShots);
    // Parallel to PoseRequest.Poses. ViewPlan[] is left untouched and the RESOLVED angles travel
    // separately, so the response can publish both what was asked for and what was shot.
    TArray<int32> PoseFrameIndex;
    TArray<int32> PoseViewIndex;
    TArray<FPlannedShot> RequestedShots;
    TArray<FPlannedShot> ResolvedShots;
    TArray<bool> OrthoAxisSnapped;
    PoseFrameIndex.Reserve(TotalShots);
    PoseViewIndex.Reserve(TotalShots);
    RequestedShots.Reserve(TotalShots);
    ResolvedShots.Reserve(TotalShots);
    OrthoAxisSnapped.Reserve(TotalShots);

    for (int32 FrameIndex = 0; FrameIndex < FramePlan.Num(); ++FrameIndex)
    {
        const double FrameNumber = FramePlan[FrameIndex];
        const double TimeSeconds = (FrameRate > 0.0) ? (FrameNumber / FrameRate) : 0.0;

        for (int32 ViewIndex = 0; ViewIndex < ViewPlan.Num(); ++ViewIndex)
        {
            const FPlannedShot& Shot = ViewPlan[ViewIndex];
            float ShotAzimuth = Shot.Azimuth;
            float ShotElevation = Shot.Elevation;
            bool bOrthoAxisSnapped = false;
            if (Shot.ProjectionMode == TEXT("orthographic"))
            {
                bOrthoAxisSnapped = SnapOrbitAnglesToOrthographicAxis(ShotAzimuth, ShotElevation);
            }

            PinWrightPoseCapture::FCameraPose Pose;
            Pose.ProjectionMode = Shot.ProjectionMode;
            Pose.Fov = Fov;
            Pose.Filename = FString::Printf(TEXT("AnimPreview_%s_%s_f%02d_v%02d_az%d_el%d.png"),
                *Asset->GetName(), *BurstStamp, FrameIndex, ViewIndex,
                FMath::RoundToInt(ShotAzimuth), FMath::RoundToInt(ShotElevation));
            PlaceOrbitCamera(Center, ShotAzimuth, ShotElevation, BaseDistance,
                Pose.Location, Pose.Rotation);
            if (Shot.ProjectionMode == TEXT("orthographic"))
            {
                Pose.OrthoWidth = ComputeOrthoWorldWidth(BoundsRadius, Padding, Width, Height);
            }
            if (bTimeAxis)
            {
                Pose.SubjectTimeSeconds = static_cast<float>(TimeSeconds);
            }

            PoseRequest.Poses.Add(MoveTemp(Pose));
            PoseFrameIndex.Add(FrameIndex);
            PoseViewIndex.Add(ViewIndex);
            RequestedShots.Add(Shot);
            ResolvedShots.Add(FPlannedShot{ShotAzimuth, ShotElevation, Shot.ProjectionMode});
            OrthoAxisSnapped.Add(bOrthoAxisSnapped);
        }
    }

    if (!bTimeAxis && FramePlan.Num() > 0)
    {
        // The bind pose, applied here and exactly once, because no pose carries a time and the
        // setter is therefore never bound. The preview still has to be evaluated before the first
        // capture or the frame shows whatever the editor's own instance last left behind.
        FString PoseErrCode;
        FString PoseErrMsg;
        if (!ApplyInstant(0, (FrameRate > 0.0) ? (FramePlan[0] / FrameRate) : 0.0,
                PoseErrCode, PoseErrMsg))
        {
            Ctx.SendError(PoseErrCode, PoseErrMsg);
            return true;
        }
    }

    PinWrightPoseCapture::FPoseListCaptureOutput PoseResult;
    {
        FString CaptureErrCode;
        FString CaptureErrMsg;
        if (!PinWrightPoseCapture::CaptureCameraPoses(*ViewportClient, SceneViewport, PoseRequest,
                PoseResult, CaptureErrCode, CaptureErrMsg))
        {
            Ctx.SendError(CaptureErrCode, CaptureErrMsg);
            return true;
        }
    }

    // A game-thread pose sample is not enough: the render proxy can still publish an older
    // image. Compare each camera against its previous frame, and refuse the whole burst when a
    // materially changed pose produced no meaningful readback change.
    const auto DeleteCapturedFiles = [&PoseResult]()
    {
        for (const PinWrightRenderCapture::FViewportCaptureOutput& Capture : PoseResult.Captures)
        {
            if (!Capture.Path.IsEmpty())
            {
                IFileManager::Get().Delete(*Capture.Path, false, true, true);
            }
        }
    };
    TArray<int32> PreviousShotByView;
    PreviousShotByView.Init(INDEX_NONE, ViewPlan.Num());
    for (int32 ShotIndex = 0; ShotIndex < PoseResult.Captures.Num(); ++ShotIndex)
    {
        const int32 ViewIndex = PoseViewIndex[ShotIndex];
        const int32 FrameIndex = PoseFrameIndex[ShotIndex];
        const int32 PreviousShotIndex = PreviousShotByView[ViewIndex];
        if (PreviousShotIndex != INDEX_NONE)
        {
            const int32 PreviousFrameIndex = PoseFrameIndex[PreviousShotIndex];
            const PinWrightAnimationPose::FPoseDelta PoseDelta =
                PinWrightAnimationPose::ComparePose(
                    PoseSamples[PreviousFrameIndex], PoseSamples[FrameIndex]);
            if (PinWrightAnimationPose::PoseChanged(PoseDelta))
            {
                const PinWrightRenderCapture::FViewportCaptureOutput& PreviousCapture =
                    PoseResult.Captures[PreviousShotIndex];
                const PinWrightRenderCapture::FViewportCaptureOutput& Capture =
                    PoseResult.Captures[ShotIndex];
                const int64 ChangedPixels =
                    PinWrightAnimationPreview::CountMeaningfulPixelChanges(
                        PreviousCapture.Pixels, Capture.Pixels);
                const int64 RequiredPixels = FMath::Max<int64>(
                    32, FMath::Max(PreviousCapture.Pixels.Num(), Capture.Pixels.Num()) / 10000);
                if (ChangedPixels < RequiredPixels)
                {
                    DeleteCapturedFiles();
                    Ctx.SendError(ErrorCodes::ERR_CAPTURE_FAILED,
                        FString::Printf(
                            TEXT("Frame %g (index %d, view %d) changed %d bones "
                                 "(%.2f cm, %.2f degrees), but its rendered image changed only "
                                 "%lld pixels; required at least %lld. The frame was not published."),
                            FramePlan[FrameIndex], FrameIndex, ViewIndex,
                            PoseDelta.MovedBoneCount, PoseDelta.MaxTranslationCm,
                            PoseDelta.MaxRotationDegrees, ChangedPixels, RequiredPixels));
                    return true;
                }
            }
        }
        PreviousShotByView[ViewIndex] = ShotIndex;
    }

    // ---- Serialize the shots ----
    TArray<TSharedPtr<FJsonValue>> ShotsJson;
    ShotsJson.Reserve(PoseResult.Captures.Num());
    // Carries the viewport state out of the shot loop for the single top-level `viewport` block;
    // only meaningful once at least one shot succeeded.
    PinWrightRenderCapture::FViewportCaptureOutput LastCapture;
    int32 BlankShots = 0;

    for (int32 ShotIndex = 0; ShotIndex < PoseResult.Captures.Num(); ++ShotIndex)
    {
        const PinWrightRenderCapture::FViewportCaptureOutput& Capture = PoseResult.Captures[ShotIndex];
        LastCapture = Capture;

        const int32 FrameIndex = PoseFrameIndex[ShotIndex];
        const double FrameNumber = FramePlan[FrameIndex];
        const double TimeSeconds = (FrameRate > 0.0) ? (FrameNumber / FrameRate) : 0.0;

        TSharedPtr<FJsonObject> ShotObj = MakeShared<FJsonObject>();
        ShotObj->SetNumberField(TEXT("frameIndex"), FrameIndex);
        ShotObj->SetNumberField(TEXT("frame"), FrameNumber);
        ShotObj->SetNumberField(TEXT("time"), TimeSeconds);
        ShotObj->SetNumberField(TEXT("viewIndex"), PoseViewIndex[ShotIndex]);
        TSharedPtr<FJsonObject> AngleObj = MakeShared<FJsonObject>();
        AngleObj->SetNumberField(TEXT("azimuth"), ResolvedShots[ShotIndex].Azimuth);
        AngleObj->SetNumberField(TEXT("elevation"), ResolvedShots[ShotIndex].Elevation);
        ShotObj->SetObjectField(TEXT("angle"), AngleObj);
        if (OrthoAxisSnapped[ShotIndex])
        {
            ShotObj->SetBoolField(TEXT("orthoAxisSnapped"), true);
            ShotObj->SetNumberField(TEXT("requestedAzimuth"), RequestedShots[ShotIndex].Azimuth);
            ShotObj->SetNumberField(TEXT("requestedElevation"), RequestedShots[ShotIndex].Elevation);
        }
        AddShotFields(Capture, PoseResult.Requests[ShotIndex], ShotObj);
        // Was the mesh in THIS frame at all? `blank` cannot answer it — it catches BLACK frames,
        // and a Persona preview backdrop is not black. Written by the primitive, and it writes
        // NOTHING when the verdict could not be evaluated: the absence is the contract.
        PinWrightPoseCapture::AddPoseFramingField(PoseResult, ShotIndex, ShotObj);
        // The `viewport` block PER SHOT as well as once at the top level. It used to come only
        // from the last capture, so the per-shot aim verdict and the per-shot warm-up settle were
        // unavailable: a burst where shot 3 was the one the viewport refused to aim published a
        // single aim block belonging to shot N.
        ShotObj->SetObjectField(TEXT("viewport"),
            PinWrightRenderCapture::MakeViewportInfoObject(Capture));
        MaybeAddBase64(Capture.Path, bInline, ShotObj);
        if (Capture.ImageStats.bBlank)
        {
            ++BlankShots;
        }
        ShotsJson.Add(MakeShared<FJsonValueObject>(ShotObj));
    }

    // ---- Serialize the instants and the pose evidence ----
    TArray<TSharedPtr<FJsonValue>> FramesJson;
    FramesJson.Reserve(FramePlan.Num());
    bool bAnyPoseChanged = false;

    for (int32 FrameIndex = 0; FrameIndex < FramePlan.Num(); ++FrameIndex)
    {
        const double FrameNumber = FramePlan[FrameIndex];
        TSharedPtr<FJsonObject> FrameObj = MakeShared<FJsonObject>();
        FrameObj->SetNumberField(TEXT("index"), FrameIndex);
        FrameObj->SetNumberField(TEXT("frame"), FrameNumber);
        FrameObj->SetNumberField(TEXT("time"), (FrameRate > 0.0) ? (FrameNumber / FrameRate) : 0.0);
        FrameObj->SetBoolField(TEXT("poseSampled"), PoseSamples[FrameIndex].bValid);
        if (PoseSamples[FrameIndex].bValid)
        {
            FrameObj->SetNumberField(TEXT("boneCount"), PoseSamples[FrameIndex].BoneTransforms.Num());
        }
        if (AchievedTimeMeasured[FrameIndex])
        {
            FrameObj->SetNumberField(TEXT("achievedTime"), AchievedTimes[FrameIndex]);
        }
        if (FrameIndex > 0)
        {
            const PinWrightAnimationPose::FPoseDelta FromPrevious =
                PinWrightAnimationPose::ComparePose(PoseSamples[FrameIndex - 1], PoseSamples[FrameIndex]);
            const PinWrightAnimationPose::FPoseDelta FromFirst =
                PinWrightAnimationPose::ComparePose(PoseSamples[0], PoseSamples[FrameIndex]);

            TSharedPtr<FJsonObject> PrevObj = MakeShared<FJsonObject>();
            PinWrightAnimationPose::AddPoseDeltaFields(FromPrevious, PrevObj);
            FrameObj->SetObjectField(TEXT("poseDeltaFromPrevious"), PrevObj);

            TSharedPtr<FJsonObject> FirstObj = MakeShared<FJsonObject>();
            PinWrightAnimationPose::AddPoseDeltaFields(FromFirst, FirstObj);
            FrameObj->SetObjectField(TEXT("poseDeltaFromFirst"), FirstObj);

            bAnyPoseChanged = bAnyPoseChanged ||
                PinWrightAnimationPose::PoseChanged(FromPrevious) ||
                PinWrightAnimationPose::PoseChanged(FromFirst);
        }
        FramesJson.Add(MakeShared<FJsonValueObject>(FrameObj));
    }

    // ---- Response. ----
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("shots"), ShotsJson);
    Result->SetNumberField(TEXT("count"), ShotsJson.Num());
    Result->SetArrayField(TEXT("frames"), FramesJson);
    Result->SetNumberField(TEXT("frameCount"), FramePlan.Num());
    Result->SetNumberField(TEXT("viewCount"), ViewPlan.Num());
    Result->SetStringField(TEXT("framePlanSource"), FramePlanSource);
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetStringField(TEXT("skeletalMeshPath"), SkeletalMesh->GetPathName());
    Result->SetStringField(TEXT("animationPath"),
        AnimationAsset ? AnimationAsset->GetPathName() : FString());
    Result->SetNumberField(TEXT("animationLengthSeconds"), LengthSeconds);
    Result->SetNumberField(TEXT("frameRate"), FrameRate);
    Result->SetBoolField(TEXT("frameRateAssumed"), !bFrameRateIsAssetOwn);
    Result->SetStringField(TEXT("captureSource"), TEXT("personaPreviewViewport"));
    Result->SetStringField(TEXT("editorName"), EditorName.ToString());
    Result->SetNumberField(TEXT("previewComponentCount"), PreviewComponentCount);
    Result->SetBoolField(TEXT("orbitCameraWasOn"), bOrbitCameraWasOn);
    Result->SetNumberField(TEXT("width"), Width);
    Result->SetNumberField(TEXT("height"), Height);
    // ONE `resolutionSource` vocabulary across every capture verb, classified from the default
    // edge this call actually applied rather than from a flag maintained beside it - so the verb
    // cannot report "singleStill" while having defaulted to something else. Exactly one string
    // moves on the wire here: this verb's old "burstBudget" is now "budget", the spelling
    // camera.orbit_shots uses for the same rule. "singleStill" survives as a distinct reason even
    // though all omitted-size policies now resolve to the shared 768 edge.
    Result->SetStringField(TEXT("resolutionSource"),
        ResolveResolutionSource(bWidthProvided || bHeightProvided, DefaultSource));
    Result->SetNumberField(TEXT("cameraDistance"), BaseDistance);
    Result->SetObjectField(TEXT("framedCenter"), PinWrightRenderCapture::MakeVectorObject(Center));
    Result->SetNumberField(TEXT("framedRadius"), BoundsRadius);
    Result->SetNumberField(TEXT("blankShots"), BlankShots);

    // What the shared pose primitive did with the burst: how many poses were asked for, how many
    // were captured, how many the bound dropped, what the throwaway warm-up frame did, and - only
    // when the set had them - what the subject time driver and the framing verdicts did.
    Result->SetObjectField(TEXT("poseSet"),
        PinWrightPoseCapture::MakePoseSetInfoObject(PoseResult));

    // How the per-instant poses were spread, and the seed that reproduces them. Published
    // unconditionally so a burst can always be retaken; `appliesToPlan` is false for `angles` and
    // `views`, which name their own poses and never read `distribution` at all.
    {
        FShotDistributionPlan DistributionPlan;
        DistributionPlan.Distribution = Distribution;
        DistributionPlan.bSeeded = bSeedProvided;
        DistributionPlan.Seed = Seed;
        DistributionPlan.AzimuthOffsetDegrees = AzimuthOffsetDegrees;
        DistributionPlan.bAppliesToPlan = bCountProvided;
        DistributionPlan.bElevationProvided = bElevationProvided;
        Result->SetObjectField(TEXT("shotDistribution"),
            MakeShotDistributionObject(DistributionPlan));
    }

    const bool bPoseSampled = UnsampledPoseCount < FramePlan.Num();
    Result->SetBoolField(TEXT("poseSampled"), bPoseSampled);
    Result->SetBoolField(TEXT("poseChanged"), bAnyPoseChanged);

    TArray<TSharedPtr<FJsonValue>> Warnings;
    const auto AddWarning = [&Warnings](const FString& Text)
    {
        Warnings.Add(MakeShared<FJsonValueString>(Text));
    };
    if (ShotsJson.Num() > 0)
    {
        TSharedPtr<FJsonObject> ViewportInfo =
            PinWrightRenderCapture::MakeViewportInfoObject(LastCapture);
        Result->SetObjectField(TEXT("viewport"), ViewportInfo);
        // Mirrored into warnings[] for the same reason as the pose verdicts below: a burst
        // captured in a debug view mode proves nothing, and this verb's callers read warnings[].
        FString ViewModeWarning;
        if (ViewportInfo->TryGetStringField(TEXT("viewModeWarning"), ViewModeWarning))
        {
            AddWarning(ViewModeWarning);
        }
    }
    if (!bPoseSampled)
    {
        AddWarning(TEXT(
            "The pose could not be sampled at any instant: the preview component reported no component-space bone "
            "transforms, which means its animation system never evaluated. The images cannot be trusted to show a "
            "posed mesh."));
    }
    else if (AnimationAsset && FramePlan.Num() > 1 && !bAnyPoseChanged)
    {
        AddWarning(FString::Printf(
            TEXT("The pose is identical at all %d sampled instants even though '%s' is loaded. Either the animation ")
            TEXT("carries no bone tracks, or every sampled frame lands on the same pose. Comparing these images proves nothing."),
            FramePlan.Num(), *AnimationAsset->GetPathName()));
    }
    if (!AnimationAsset)
    {
        AddWarning(FramePlanSource == TEXT("bindPose")
            ? FString(TEXT("No animation was supplied, so this is a BIND POSE capture and the requested multi-instant "
                           "burst was collapsed to one image — every instant would have been identical. Pass "
                           "'animation' to review the mesh in motion."))
            : FString(TEXT("No animation was supplied, so this is a BIND POSE capture. Pass 'animation' to review "
                           "the mesh in motion.")));
    }
    if (PreviewComponentCount > 1)
    {
        AddWarning(FString::Printf(
            TEXT("%d preview mesh components were found in this editor's preview scene; the one matching the "
                 "requested mesh (or the first found) was posed and measured. Other meshes in frame are not "
                 "driven by this call."),
            PreviewComponentCount));
    }
    if (BlankShots > 0)
    {
        AddWarning(FString::Printf(
            TEXT("%d of %d shots came back near-uniform black (see each shot's imageStats). A blank set reads as a ")
            TEXT("clean success unless you check this."),
            BlankShots, ShotsJson.Num()));
    }
    if (PoseResult.PosesOutOfFrame > 0)
    {
        // The failure `blank` cannot see. A Persona preview backdrop is brightly lit, so a shot
        // that missed the mesh entirely comes back with blank:false and litPixelFraction 1 - a
        // clean-looking picture of nothing. The verdict is conservative in one direction by
        // construction, so a positive count is a FACT rather than a suspicion.
        AddWarning(FString::Printf(
            TEXT("%d of %d shots framed no part of the mesh: its bounding sphere provably could not project into "
                 "the frame (see each shot's `framing`). Those images are backdrop, and `blank` does not catch "
                 "them because the preview backdrop is lit. Widen `padding` or check the requested angles."),
            PoseResult.PosesOutOfFrame, PoseResult.Captures.Num()));
    }
    if (Warnings.Num() > 0)
    {
        Result->SetArrayField(TEXT("warnings"), Warnings);
    }

    // Before the response is built, so the window fields below are measured rather than intended.
    //
    // This function's own copies of the preview viewport are dropped FIRST. The lambda clears the
    // acquisition's pair, but it was defined before these two locals existed and so cannot see
    // them; they are the remaining references, and one surviving reference is what turns the close
    // into check(SceneViewport.IsUnique()). Neither is read below.
    ViewportClient = nullptr;
    SceneViewport.Reset();
    CloseAssetEditorIfRequested();
    Result->SetBoolField(TEXT("assetEditorWasAlreadyOpen"), bWasAlreadyOpen);
    Result->SetBoolField(TEXT("assetEditorClosed"), bAssetEditorClosed);
    // The close is queued onto the next editor tick rather than run on the capture's own
    // stack, so assetEditorClosed normally reads FALSE here. Without this field beside it,
    // that false is indistinguishable from "left open by request". The subject block already
    // publishes it; the top level did not.
    Result->SetBoolField(TEXT("assetEditorCloseDeferred"),
        PinWrightCaptureSubject::HasPendingDeferredAssetEditorClose(AssetPath));

    // The subject, in the shared cross-verb block, and built AFTER the close so its window fields
    // are measured rather than intended. Always present on this verb because this verb always has
    // a subject - an assetPath IS one here - which is what makes the block honest rather than an
    // empty object emitted for symmetry.
    {
        PinWrightCaptureSubject::FResolvedSubject ResolvedSubject;
        // NO viewport pair is copied in. The close above may have destroyed that viewport, and
        // MakeSubjectInfoObject reads neither field - the block is kind, path, bounds, time axis
        // and the two measured window flags. Assigning them here used to re-acquire a reference to
        // a viewport that was already being torn down, for a reader that never looks at it.
        // Bounds from the mesh ASSET, which is what the camera was framed from and what every
        // shot's `framing` verdict was measured against.
        ResolvedSubject.BoundsOrigin = Center;
        ResolvedSubject.BoundsRadius = static_cast<double>(MeshBounds.SphereRadius);
        ResolvedSubject.bTimeSupported = bTimeAxis;
        ResolvedSubject.TimeStartSeconds = 0.0;
        ResolvedSubject.TimeEndSeconds = LengthSeconds;
        ResolvedSubject.CaptureSource = TEXT("personaPreviewViewport");
        ResolvedSubject.BoundsSource = TEXT("assetBounds");
        ResolvedSubject.bEditorWasAlreadyOpen = bWasAlreadyOpen;
        ResolvedSubject.bEditorClosed = bAssetEditorClosed;
        // An animation scrub IS reproducible: SetPosition writes an ABSOLUTE time, and the
        // evaluation that follows is synchronous and deterministic. This is one of the few subject
        // kinds where that can honestly be said - a particle system's cannot be, which is why the
        // Niagara kind hardcodes false.
        ResolvedSubject.bTimeReproducible = bTimeAxis;

        // `kind` and `path` are fields of FResolvedSubject and are serialized by the shared block,
        // not patched on afterwards - one producer for the `subject` shape, so this verb cannot
        // drift from the other seven. The animation itself is already reported at the top level as
        // `animationPath` and is deliberately not duplicated inside the block.
        ResolvedSubject.Kind = AnimationAsset
            ? PinWrightCaptureSubject::ESubjectKind::Animation
            : PinWrightCaptureSubject::ESubjectKind::SkeletalMesh;
        ResolvedSubject.AssetPath = AssetPath;
        Result->SetObjectField(TEXT("subject"),
            PinWrightCaptureSubject::MakeSubjectInfoObject(ResolvedSubject));
    }

    Ctx.SendSuccess(Result);
    return true;
}
