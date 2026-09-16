// Copyright (c) 2026 Alexander Penkin. MIT License.

// camera.animation_shots — a frame burst of a PLACED, animated skeletal mesh: N instants of a
// Level Sequence crossed with M camera angles, captured through the live Level Editor viewport.
//
// This is the in-level half of animated review. The isolated half (no level, no actor, no
// sequence) is render.capture_animation_preview, which drives a Persona preview viewport
// instead. Both return the same per-shot fields, so a set moves between them unchanged.
//
// Everything here exists because a set of images cannot, on its own, tell you that an animation
// is running. Four rules follow from that, and each one is load-bearing:
//
//  1. SCRUB, NEVER PLAY. A bound skeletal mesh only evaluates in the editor while Sequencer
//     drives it — bUpdateAnimationInEditor is transient and defaults false, so a plain
//     SkeletalMeshActor does not animate in the viewport on its own. And a PLAYING sequence
//     advances between captures, so the frames drift by however long each capture took and stop
//     being comparable instants. If the sequence is playing when this verb is called, it is
//     paused first and the response says so.
//  2. DRIVE IN FRAMES, NOT SECONDS. Seconds keep a sub-frame remainder that accumulates through
//     the display-rate/tick-resolution conversion, so a burst walks off its own grid. Every
//     seconds-shaped input here is converted to display-rate frames up front, and the frames
//     actually used are echoed back.
//  3. ONE CAMERA AND ONE CAPTURE SIZE FOR THE WHOLE BURST. A posed skeletal mesh's bounds change
//     every frame, so recomputing the framing per frame would move the camera under the subject
//     and destroy the comparison the burst exists to support. The frame plan is walked once
//     WITHOUT capturing to union the bounds across every sampled instant; the camera is solved
//     from that union and then held. Resolution is resolved once, for the same reason and
//     because changing capture size mid-session is what produced an FViewport::GetHitProxy
//     assert in this project — an incident that cost 66 actors and 125 emitters of unsaved level
//     state. Both properties are now STRUCTURAL rather than remembered: the size lives on
//     FPoseListCaptureRequest, which has no per-pose override, and every pose is generated from
//     one solved centre and distance before a single frame is drawn.
//  4. MEASURE THE POSE, NOT JUST THE PIXELS. A no-capture pre-pass samples the component-space
//     bone transforms at every instant, so the response can state whether the pose changed at
//     all — and, when it did not, whether the actor nonetheless moved, which is the exact
//     signature of a mesh sliding across the level in bind pose.
//
// WHAT THIS FILE NO LONGER OWNS. The nested frame x view capture loop is gone. The burst is now a
// POSE GENERATOR plus one PinWrightPoseCapture::CaptureCameraPoses call carrying an
// FSubjectTimeSetter, so the warm-up frame, the pose bound and its honest truncation, the
// per-shot framing verdict, the exposure pin, the scoped view mode, sprite suppression, orbit
// suppression, both viewport transform slots and the aim verification all come from the shared
// primitive instead of from a loop only this verb ran. The hand-rolled loop could not report a
// partial set: a shot that failed mid-burst discarded every shot before it. It can now.
//
// THE SUBJECT IS A PARAMETER, AND IT IS STILL A WORLD/ACTOR ONE. `subject` accepts the `actor`
// and `world` kinds and `actorName` normalises into the first of them, so no existing call moves.
// It is NOT an animation-asset verb and must not become one: the subject is a PLACED actor and
// the time axis is a Level Sequence scrubbed through Sequencer, which is the only thing that
// makes a bound skeletal mesh evaluate in the editor at all. An asset kind is refused by name
// with UNSUPPORTED_ASSET_EDITOR — render.capture_animation_preview is the isolated half.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "Handlers/Render/AnimationPoseEvidence.h"
#include "Handlers/Render/CameraShotPlanUtils.h"
#include "Handlers/Render/CaptureSubject.h"
#include "Handlers/Render/CaptureSubjectProviders_Level.h"
#include "Handlers/Render/PoseListCapture.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Handlers/Sequencer/SequencePlayheadUtils.h"
#include "Utils/ActorUtils.h"
#include "Utils/AssetUtils.h"
#include "Utils/JsonUtils.h"
#include "Utils/MovieSceneJsonUtils.h"

#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorViewportClient.h"
#include "GameFramework/Actor.h"
#include "LevelSequence.h"
#include "Misc/DateTime.h"
#include "Misc/ScopeExit.h"
#include "MovieScene.h"
#include "Slate/SceneViewport.h"

// File-unique named namespace so Unity merges cannot collide these with the same-shaped helpers
// in sibling Render/*.cpp files.
namespace PinWrightAnimationShots
{
    // One sampled instant of the burst.
    struct FBurstFrame
    {
        FFrameTime DisplayTime;
        double Seconds = 0.0;
        PinWrightAnimationPose::FPoseSample Pose;
    };

    // Default number of instants when the caller does not say. It is derived from the shot
    // budget rather than fixed, because the burst's real cost is frames x angles: five instants
    // is a useful read of one animation cycle from a single angle, but five instants from six
    // sides is thirty viewport captures and thirty PNGs, which is neither cheap nor readable.
    // Filling the budget instead keeps the default call affordable at every view count.
    constexpr int32 GDefaultFrameCountCap = 5;

    inline int32 DefaultFrameCountForViews(int32 ViewCount)
    {
        const int32 SafeViews = FMath::Max(ViewCount, 1);
        return FMath::Clamp(PinWrightCameraFrame::GMaxOrbitShots / SafeViews, 1, GDefaultFrameCountCap);
    }

    // FParamSpec carrying BOTH a documented default and alternate spellings. RPC_PARAM_DEF cannot
    // express aliases and ParamAliasUtils::MakeAliasParamSpec cannot express a default, and the
    // playhead knobs below need both: their bodies resolve each value through GetBoolFirstOf /
    // GetStringFirstOf over the snake_case twin, so the twin must be declared or the dispatcher
    // refuses it with UNKNOWN_PARAMS before the body runs. Field order is FParamSpec's declaration
    // order (Handlers/ParamSpec.h): Name, Type, Description, bRequired, Default, Aliases.
    inline FParamSpec ShotsParamWithAliases(const TCHAR* Name, const TCHAR* Type, const TCHAR* Desc,
                                            const TCHAR* Default, const TArray<FString>& Aliases)
    {
        FParamSpec Spec{FString(Name), FString(Type), FString(Desc), false, FString(Default)};
        Spec.Aliases = Aliases;
        return Spec;
    }
}

// ---- camera.animation_shots ----
REGISTER_RPC_HANDLER("camera.animation_shots", "camera",
    "Capture a frame burst of a placed, animated skeletal mesh: N instants of a Level Sequence crossed with M camera angles, in one call. "
    "Scrubs the Sequencer playhead to each instant (never plays), holds one camera and one capture size for the whole burst, and returns "
    "per-shot image statistics plus numeric proof of whether the POSE actually changed between instants.",
    RPC_PARAMS(
        ParamAliasUtils::MakeAliasParamSpec(TEXT("sequencePath"), TEXT("path"),
            TEXT("Level Sequence that drives the animation. Scrubbing it is the only thing that makes a bound skeletal mesh evaluate in the editor."),
            /*bRequired=*/true, TArray<FString>({TEXT("path"), TEXT("sequence_path")})),
        // NOT required at the spec level any more, and the change is deliberate. A required slot
        // is enforced by the dispatcher before the handler runs, which made `subject` unusable:
        // a caller naming a subject kind with no time axis died on the missing `actorName` (or,
        // with `assetPath`, on UNKNOWN_PARAMS) and never reached the typed refusal that tells
        // them WHY a static mesh cannot be photographed over time. Absence is still refused, by
        // the body, naming both accepted spellings — the same shape camera.orbit_shots has used
        // for its own optional-target slot since it shipped.
        ParamAliasUtils::MakeAliasParamSpec(TEXT("actorName"), TEXT("string"),
            TEXT("Animated actor to frame and pose-sample. Accepts a display label, internal object name, or object path. Alternative spelling of subject:{kind:\"actor\", name:...}." ACTORNAME_COLLISION_STEER),
            /*bRequired=*/false, ActorNameParamUtils::ActorNameKeys()),
        RPC_PARAM_OPT("subject", "object",
            "What the burst is OF, as one object: {kind, name, point, radius}. `kind` is 'actor' (a placed, animated actor - the same thing actorName names) "
            "or 'world' (the level itself: pass point{x,y,z}+radius to orbit a spot, or omit both to frame the whole level's bounds). "
            "Omit the object entirely and actorName normalises into kind:'actor', so no existing call changes. "
            "The asset kinds - staticMesh, skeletalMesh, animation, niagara - are REFUSED here with UNSUPPORTED_ASSET_EDITOR naming the reason, because this verb's time axis is a Level Sequence "
            "scrubbed through Sequencer and an asset in its own editor has no placed actor for one to drive; render.capture_animation_preview is the isolated-asset half."),
        RPC_PARAM_OPT("frames", "array", "Explicit DISPLAY-RATE frame numbers to sample, e.g. [0, 6, 12, 18]. Wins over every other time option. Rounded onto whole display frames, and the response says so when any instant moved."),
        RPC_PARAM_OPT("frameCount", "integer", "Number of instants to sample. Defaults to filling the 24-shot budget at the requested view count, capped at 5."),
        RPC_PARAM_OPT("frameStart", "integer", "First display frame of the burst. Defaults to the start of the sequence playback range."),
        RPC_PARAM_OPT("frameStep", "integer", "Display frames between instants. Omit to spread frameCount instants evenly across the playback range."),
        RPC_PARAM_OPT("intervalSeconds", "number", "Seconds between instants, converted to whole display frames up front. Alternative spelling of frameStep; frameStep wins."),
        RPC_PARAM_OPT("angles", "array", "Explicit camera poses, each {azimuth, elevation} in degrees. Crossed with every sampled instant."),
        RPC_PARAM_OPT("count", "integer", "Number of camera poses, spread by `distribution`. Alternative to angles."),
        RPC_PARAM_OPT("views", "string", "'sides' captures the six axis-aligned views at every instant. Cannot be combined with count or angles."),
        RPC_PARAM_DEF("distribution", "string", "How `count` spreads its poses. 'ring' (default, and the pre-existing behaviour) puts them at evenly-spaced azimuths on ONE horizontal circle at `elevation`, which never sees the top or the underside. 'sphere' uses a golden-angle (Fibonacci) spiral over the whole viewing sphere - deterministic, near-optimal even coverage for any N - and IGNORES `elevation`, which the response says out loud rather than dropping silently. Only affects `count`; `angles` and `views` name their own poses. Reported back in shotDistribution.", "ring"),
        RPC_PARAM_OPT("seed", "number", "Jitter the pose distribution by a seeded azimuth offset, so a burst can vary between runs without becoming irreproducible. Omitted, output is deterministic (offset 0). Supplied, output is STILL deterministic - the same seed always gives the same poses - and the seed is echoed in shotDistribution so any burst can be retaken exactly."),
        RPC_PARAM_OPT("azimuth", "number", "Single-angle horizontal orbit angle in degrees (default 45)."),
        RPC_PARAM_OPT("elevation", "number", "Vertical orbit angle in degrees above the horizon (default 15; lower than the still-capture default because a gait reads badly foreshortened from above)."),
        RPC_PARAM_OPT("radius", "number", "Camera distance. Defaults to a bounds-fit distance solved once from the union of the actor's bounds across every sampled instant."),
        RPC_PARAM_OPT("padding", "number", "Bounds-fit margin multiplier (default 1.15)."),
        RPC_PARAM_OPT("fov", "number", "Perspective field of view in degrees (default 50)."),
        RPC_PARAM_OPT("projectionMode", "string", "'perspective' (default) or 'orthographic'. Orthographic poses snap onto the nearest cardinal world axis."),
        RPC_PARAM_OPT("exposure", "object|number", PINWRIGHT_EXPOSURE_PARAM_DESC " Read once for the whole burst, so two instants of the animation are exposed identically and a pose difference cannot be confused with a brightness one."),
        RPC_PARAM_OPT("viewMode", "string", PINWRIGHT_VIEW_MODE_PARAM_DESC " Read once for the whole burst, so every instant and every angle is rendered in the same mode, and restored once after the last shot."),
        // Offered here and NOT on the preview verbs, and the difference is real rather than
        // stylistic: this verb draws the LIVE Level Editor viewport, whose world has actors and
        // therefore billboard sprites over the subject. FPreviewScene registers components with
        // no AActor at all, so the same flag on an asset-preview verb could only ever report that
        // nothing changed (PreviewViewportCaptureUtils.h:26-31).
        ParamAliasUtils::MakeAliasParamSpec(TEXT("hideEditorSprites"), TEXT("boolean"),
            TEXT(PINWRIGHT_HIDE_EDITOR_SPRITES_PARAM_DESC), /*bRequired=*/false,
            TArray<FString>({TEXT("hideEditorSprites"), TEXT("hide_editor_sprites")})),
        RPC_PARAM_OPT("width", "number", "Output width in pixels. Default 768 for both a burst and a single image."),
        RPC_PARAM_OPT("height", "number", "Output height in pixels. Same default rule as width."),
        PinWrightAnimationShots::ShotsParamWithAliases(TEXT("updateMethod"), TEXT("string"),
            TEXT("How Sequencer applies each position: scrub (default), jump, play. Snake_case update_method accepted."),
            TEXT("scrub"), TArray<FString>({TEXT("update_method")})),
        PinWrightAnimationShots::ShotsParamWithAliases(TEXT("forceUpdate"), TEXT("boolean"),
            TEXT("Force one immediate re-evaluation after each scrub so the captured pixels show the frame that was asked for and not the previous one. Leave on. Snake_case force_update accepted."),
            TEXT("true"), TArray<FString>({TEXT("force_update")})),
        PinWrightAnimationShots::ShotsParamWithAliases(TEXT("open"), TEXT("boolean"),
            TEXT("Open the sequence in Sequencer when it is not the one currently open. false rejects with SEQUENCE_NOT_OPEN. Also accepted as openIfNeeded / open_if_needed."),
            TEXT("true"), TArray<FString>({TEXT("openIfNeeded"), TEXT("open_if_needed")})),
        PinWrightAnimationShots::ShotsParamWithAliases(TEXT("restorePlayhead"), TEXT("boolean"),
            TEXT("Put the playhead back where the burst found it. Default true. Snake_case restore_playhead accepted."),
            TEXT("true"), TArray<FString>({TEXT("restore_playhead")})),
        RPC_PARAM_OPT("inline", "boolean", "When true, also embed base64 PNG bytes per shot (default false).")
    ))
{
    using namespace PinWrightCameraFrame;
    using namespace PinWrightAnimationShots;

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    const bool bHasPayload = Payload.IsValid();
    const auto HasField = [&](const TCHAR* Key) { return bHasPayload && Payload->HasField(Key); };

    // Everything that can be judged from the request alone is judged FIRST, before any asset is
    // loaded, any actor is resolved and any editor state is touched. A malformed burst request
    // must not leave a sequence opened or a playhead moved on its way to being rejected.

    // ---- Subject. Parsed FIRST because the kind decides whether this verb can serve the request
    // at all, and that is knowable from the payload alone. A kind with no time axis is refused
    // here, before an actor is resolved, a sequence is loaded or a viewport is touched. ----
    PinWrightCaptureSubject::FSubjectRequest SubjectRequest;
    {
        FString SubjectErrCode;
        FString SubjectErrMsg;
        if (!PinWrightCaptureSubject::ParseSubject(Payload, SubjectRequest,
                SubjectErrCode, SubjectErrMsg))
        {
            Ctx.SendError(SubjectErrCode, SubjectErrMsg);
            return true;
        }
    }
    // Only an explicit `subject` object changes the kind. Without one the verb is exactly what it
    // was: an actor burst, whose target is `actorName`. Inference on a legacy payload would let a
    // stray `radius` (which means CAMERA DISTANCE here) reclassify the subject.
    const bool bSubjectProvided = HasField(TEXT("subject"));
    const PinWrightCaptureSubject::ESubjectKind SubjectKind = bSubjectProvided
        ? SubjectRequest.Kind
        : PinWrightCaptureSubject::ESubjectKind::Actor;
    SubjectRequest.Kind = SubjectKind;

    if (SubjectKind != PinWrightCaptureSubject::ESubjectKind::Actor &&
        SubjectKind != PinWrightCaptureSubject::ESubjectKind::World)
    {
        // Decision 6: a capability a subject kind cannot support is a TYPED refusal, not silence
        // and not the dispatcher's UNKNOWN_PARAMS. The message has to name the missing axis,
        // because the caller's next move is a different verb and nothing else in the response
        // points at one.
        Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR,
            TEXT("camera.animation_shots has no time axis for an asset subject. Its instants come from "
                 "scrubbing a Level Sequence, which drives a PLACED actor in the level — an asset open in "
                 "its own editor has no placed actor for a sequence to drive, and a static mesh has no time "
                 "to be at in the first place. Use subject:{kind:\"actor\"} (or actorName) for an animated "
                 "actor in the level, or render.capture_animation_preview for a skinned asset in isolation."));
        return true;
    }

    const bool bActorSubject = (SubjectKind == PinWrightCaptureSubject::ESubjectKind::Actor);

    FString ActorName = SubjectRequest.ActorName;
    if (ActorName.IsEmpty())
    {
        ActorName = ActorNameParamUtils::ResolveActorName(Ctx);
    }
    if (bActorSubject && ActorName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(
                TEXT("An actor burst needs a target. Pass one of: %s — or subject:{kind:\"world\", point:{x,y,z}, radius:N} "
                     "to shoot the level itself instead of an actor."),
                *FString::Join(ActorNameParamUtils::ActorNameKeys(), TEXT(", "))));
        return true;
    }
    SubjectRequest.ActorName = ActorName;

    // Read here, with the other request-only validation, and above the burst loops: one exposure
    // for the whole burst is what lets two instants of the same animation be compared at all.
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

    const FString SequencePath =
        Ctx.GetStringFirstOf({TEXT("sequencePath"), TEXT("path"), TEXT("sequence_path")});
    if (SequencePath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("sequencePath is required: scrubbing a Level Sequence is the only thing that makes a bound "
                 "skeletal mesh evaluate in the editor viewport."));
        return true;
    }

    EUpdatePositionMethod UpdateMethod = EUpdatePositionMethod::Scrub;
    FString CanonicalMethod;
    const FString RequestedMethod =
        Ctx.GetStringFirstOf({TEXT("updateMethod"), TEXT("update_method")}, TEXT("scrub"));
    if (!SequencePlayheadUtils::ParseUpdateMethod(RequestedMethod, UpdateMethod, CanonicalMethod))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Unknown updateMethod '%s' (expected 'scrub', 'jump' or 'play')"),
                *RequestedMethod));
        return true;
    }
    const bool bForceUpdate = Ctx.GetBoolFirstOf({TEXT("forceUpdate"), TEXT("force_update")}, true);

    // ---- View plan. Same shapes as camera.orbit_shots so a pose moves between the two verbs. ----
    const bool bElevationProvided = HasField(TEXT("elevation"));
    const float DefaultElevation = static_cast<float>(Ctx.GetNumber(TEXT("elevation"), 15.0));
    const float Fov = static_cast<float>(Ctx.GetNumber(TEXT("fov"), 50.0));
    const float Padding = static_cast<float>(Ctx.GetNumber(TEXT("padding"), 1.15));
    const bool bInline = Ctx.GetBool(TEXT("inline"), false);
    const bool bHideEditorSprites =
        Ctx.GetBoolFirstOf({TEXT("hideEditorSprites"), TEXT("hide_editor_sprites")}, false);

    // ---- Pose distribution + seeded jitter. Pure pose-generation inputs, resolved above the plan
    // because the plan is the only thing they touch. `ring` is the default and reproduces the
    // previous behaviour byte-for-byte, so no existing caller's pixels move. ----
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
    const double AzimuthOffsetDegrees =
        bSeedProvided ? SeedToAzimuthOffsetDegrees(Seed) : 0.0;

    const bool bProjectionProvided = HasField(TEXT("projectionMode"));
    const FString RequestedProjection =
        Ctx.GetString(TEXT("projectionMode"), TEXT("perspective")).ToLower();
    if (bProjectionProvided && !IsValidProjectionMode(RequestedProjection))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("projectionMode must be 'perspective' or 'orthographic'"));
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
        // ONE table, shared with camera.orbit_shots and render.capture_animation_preview. It used
        // to be written out here verbatim, which is three chances for one copy to be reordered —
        // and a reordered `sides` set is six individually correct images that no longer line up
        // with the archived set they are compared against.
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
        ViewPlan = (Distribution == EShotDistribution::Sphere)
            ? MakeSphereDistribution(Count, AzimuthOffsetDegrees, RequestedProjection)
            : MakeRingDistribution(Count, DefaultElevation, AzimuthOffsetDegrees, RequestedProjection);
    }
    else
    {
        // One three-quarter view. Elevation 15 rather than camera.frame_actor's 30: a walk cycle
        // is judged on stride length and foot contact, and a high camera foreshortens both.
        ViewPlan.Add(FPlannedShot{
            static_cast<float>(Ctx.GetNumber(TEXT("azimuth"), 45.0)), DefaultElevation, RequestedProjection});
    }

    if (Fov <= 0.0f)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("fov must be greater than zero"));
        return true;
    }

    // ---- Explicit instants are known from the request alone, so they are parsed here and the
    // shot budget they imply is enforced before anything in the world is resolved. A derived
    // plan needs the sequence's playback range and is checked again once that is known. ----
    const bool bFramesProvided = HasField(TEXT("frames"));
    const bool bFrameCountProvided = HasField(TEXT("frameCount"));
    const int32 FrameCount = bFrameCountProvided
        ? Ctx.GetInt(TEXT("frameCount"), 5)
        : DefaultFrameCountForViews(ViewPlan.Num());
    if (FrameCount < 1)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("frameCount must be at least 1"));
        return true;
    }

    TArray<FFrameTime> FramePlan;
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
                    FramePlan.Add(FFrameTime::FromDecimal(FrameNumber));
                }
            }
        }
        if (FramePlan.Num() == 0)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("frames must be a non-empty array of finite display-rate frame numbers"));
            return true;
        }
        FramePlanSource = TEXT("frames");
        if (FramePlan.Num() * ViewPlan.Num() > GMaxOrbitShots)
        {
            Ctx.SendError(ErrorCodes::ERR_TOO_MANY_SHOTS,
                FString::Printf(
                    TEXT("%d instants x %d angles = %d shots exceeds the maximum of %d. Reduce the frame list, ")
                    TEXT("reduce the view plan, or split the burst across calls."),
                    FramePlan.Num(), ViewPlan.Num(), FramePlan.Num() * ViewPlan.Num(), GMaxOrbitShots));
            return true;
        }
    }

    // ---- Request is well-formed. Now resolve the world objects it names. ----
    //
    // The actor is resolved HERE, ahead of the subject resolver that will resolve it again, for
    // one reason: the skeletal-component check below must fire before a sequence is opened or a
    // playhead is moved, and it needs the component the resolver does not hand back. The lookup
    // is the same FindActorByName the resolver runs, so the two cannot disagree about which actor
    // a name picks out or which code an unresolvable name returns.
    AActor* Actor = nullptr;
    USkeletalMeshComponent* SkeletalComponent = nullptr;
    if (bActorSubject)
    {
        Actor = McpActorUtils::FindActorByName(nullptr, ActorName);
        if (!Actor)
        {
            Ctx.SendError(ErrorCodes::ERR_ACTOR_NOT_FOUND,
                FString::Printf(TEXT("Actor not found: %s"), *ActorName));
            return true;
        }
        // Absence is reported rather than tolerated: without a skeletal mesh component there is
        // nothing to pose-sample, and a burst that cannot measure the pose is exactly the
        // "photographs identically at every frame" case this verb exists to expose. A
        // StaticMeshActor reaching this verb is itself the most common root cause — a rig
        // assembled from StaticMeshActors cannot animate by design, and that was the real fault
        // behind one "the staff is rigidly bound to hand_l" misdiagnosis.
        SkeletalComponent = Actor->FindComponentByClass<USkeletalMeshComponent>();
        if (!SkeletalComponent)
        {
            Ctx.SendError(ErrorCodes::ERR_ACTOR_NO_SKELETAL_MESH_COMPONENT,
                FString::Printf(
                    TEXT("Actor '%s' (%s) has no SkeletalMeshComponent, so there is no pose to sample and nothing that ")
                    TEXT("can animate. A rig assembled from StaticMeshActors cannot animate by design — check ")
                    TEXT("actor.get_components."),
                    *ActorName, *Actor->GetClass()->GetName()));
            return true;
        }
    }

    ULevelSequence* LevelSeq =
        Cast<ULevelSequence>(ResolveAsset(SequencePath, /*bLoadObject=*/true).Object);
    if (!LevelSeq)
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_NOT_FOUND,
            FString::Printf(TEXT("Level sequence not found at '%s'"), *SequencePath));
        return true;
    }
    UMovieScene* MovieScene = LevelSeq->GetMovieScene();
    if (!MovieScene)
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_INVALID,
            FString::Printf(TEXT("Level sequence '%s' has no MovieScene"), *SequencePath));
        return true;
    }

    // ---- Time plan. Frames are authoritative everywhere; seconds are converted once, here. ----
    const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
    const FFrameRate TickResolution = MovieScene->GetTickResolution();

    FFrameTime RangeStart(0);
    FFrameTime RangeEnd(0);
    const bool bHasRange =
        SequencePlayheadUtils::GetPlaybackRangeInDisplayFrames(*MovieScene, RangeStart, RangeEnd);

    const bool bFrameStepProvided = HasField(TEXT("frameStep"));
    const bool bIntervalProvided = HasField(TEXT("intervalSeconds"));
    const bool bFrameStartProvided = HasField(TEXT("frameStart"));

    if (!bFramesProvided)
    {
        const FFrameTime StartFrame = bFrameStartProvided
            ? FFrameTime::FromDecimal(Ctx.GetNumber(TEXT("frameStart"), 0.0))
            : (bHasRange ? RangeStart : FFrameTime(0));

        // frameStep wins over intervalSeconds; both land on WHOLE display frames so the burst
        // sits on the sequence's own grid instead of accumulating a sub-frame remainder that
        // walks the samples off it.
        double StepFrames = 0.0;
        if (bFrameStepProvided)
        {
            StepFrames = Ctx.GetNumber(TEXT("frameStep"), 0.0);
            FramePlanSource = TEXT("frameStep");
        }
        else if (bIntervalProvided)
        {
            const double IntervalSeconds = Ctx.GetNumber(TEXT("intervalSeconds"), 0.0);
            StepFrames = FMath::RoundToDouble(DisplayRate.AsDecimal() * IntervalSeconds);
            FramePlanSource = TEXT("intervalSeconds");
        }
        else if (bHasRange && FrameCount > 1)
        {
            // Spread across the playback range, endpoint-EXCLUSIVE. A cyclic animation's last
            // frame repeats its first, so an inclusive span would spend one shot of the budget
            // photographing the same pose twice; a caller who wants the terminal pose passes
            // `frames` or `frameStep`.
            const double Span = (RangeEnd - RangeStart).AsDecimal();
            StepFrames = FMath::Max(FMath::RoundToDouble(Span / static_cast<double>(FrameCount)), 1.0);
            FramePlanSource = TEXT("playbackRange");
        }
        else
        {
            StepFrames = 1.0;
            FramePlanSource = bHasRange ? TEXT("playbackRange") : TEXT("singleFrame");
        }

        if (!FMath::IsFinite(StepFrames))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("frameStep / intervalSeconds must be finite"));
            return true;
        }
        for (int32 Index = 0; Index < FrameCount; ++Index)
        {
            FramePlan.Add(StartFrame + FFrameTime::FromDecimal(StepFrames * static_cast<double>(Index)));
        }
    }

    // ---- Snap the plan onto WHOLE display frames, and say so when that moved anything. ----
    //
    // Rule 2 of this file's header already required it of every seconds-shaped input; it is now
    // required of every input, because the instants travel to the capture primitive as SECONDS
    // (FCameraPose::SubjectTimeSeconds) and come back through DisplayRate::AsFrameTime. A
    // sub-frame position could not survive that round trip intact, so it is rounded HERE, once,
    // where the response can report the rounded value rather than echoing a request the pixels
    // did not honour. `frames:[6.5]` is the only shape that reaches this with a remainder.
    int32 RoundedInstantCount = 0;
    for (FFrameTime& DisplayTime : FramePlan)
    {
        const FFrameTime Rounded(DisplayTime.RoundToFrame());
        if (Rounded != DisplayTime)
        {
            ++RoundedInstantCount;
            DisplayTime = Rounded;
        }
    }

    // ---- Shot budget. frames x angles is the real cost, and each shot re-poses and resizes a
    // live viewport, so the ceiling is on the product and not on either axis. ----
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

    // ---- Resolution, resolved ONCE and held for the whole burst. Changing capture size between
    // shots re-enters the viewport resize path on every one of them, which is the class of
    // change that produced an FViewport::GetHitProxy assert in this project. ----
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

    // ---- Resolve the subject: the live viewport, the sequence, the time setter, and the bounds
    // ONE camera is solved from — all in one call, and in that order. The viewport is acquired
    // FIRST inside AcquireLevelSubject, so a headless run fails without having opened a sequence
    // or moved a playhead. That ordering used to live here and is now shared with every other
    // world/actor capture, which is the only way it stays true in all of them. ----
    const bool bOpenIfNeeded =
        Ctx.GetBoolFirstOf({TEXT("open"), TEXT("openIfNeeded"), TEXT("open_if_needed")}, true);
    const bool bRestorePlayhead =
        Ctx.GetBoolFirstOf({TEXT("restorePlayhead"), TEXT("restore_playhead")}, true);

    PinWrightCaptureSubjectLevel::FLevelTimePlan TimePlan;
    TimePlan.SequencePath = SequencePath;
    TimePlan.UpdateMethod = UpdateMethod;
    TimePlan.CanonicalMethod = CanonicalMethod;
    TimePlan.bForceUpdate = bForceUpdate;
    TimePlan.bOpenSequenceIfNeeded = bOpenIfNeeded;
    // The resolver owns the restore now, rather than this file keeping a second ON_SCOPE_EXIT
    // beside it: one writer for the playhead means it cannot be put back twice, and the release
    // below runs on every exit path including the refusals underneath it.
    TimePlan.bRestorePlayheadOnRelease = bRestorePlayhead;
    // POSED bounds, unioned across every instant the burst will visit, in a no-capture pre-pass.
    // This is the whole reason the plan is handed over: a posed skeletal mesh's bounds change
    // every frame, so a camera solved from ONE instant would move under the subject between shots
    // and destroy the comparison the burst exists to support. It is deliberately different from
    // the preview path's mesh-ASSET bounds, and both are correct for what they frame.
    TimePlan.InstantSeconds.Reserve(FramePlan.Num());
    for (const FFrameTime& DisplayTime : FramePlan)
    {
        TimePlan.InstantSeconds.Add(DisplayRate.AsSeconds(DisplayTime));
    }

    PinWrightCaptureSubject::FResolvedSubject Resolved;
    PinWrightCaptureSubject::FSubjectTimeSetter SubjectTimeSetter;
    PinWrightCaptureSubjectLevel::FLevelSubjectReport LevelReport;
    FString ErrCode;
    FString ErrMsg;
    if (!PinWrightCaptureSubjectLevel::AcquireLevelSubject(SubjectRequest, TimePlan,
            Resolved, SubjectTimeSetter, LevelReport, ErrCode, ErrMsg))
    {
        Ctx.SendError(ErrCode, ErrMsg);
        return true;
    }
    ON_SCOPE_EXIT { PinWrightCaptureSubjectLevel::ReleaseLevelSubject(Resolved); };

    FEditorViewportClient* const ViewportClient = Resolved.ViewportClient;
    const TSharedPtr<FSceneViewport> SceneViewport = Resolved.SceneViewport;
    const bool bOpened = LevelReport.bSequenceOpened;
    const bool bPausedPlayback = LevelReport.bPausedPlayback;
    // Carries the viewport state (view mode above all) out of the shot list for the single
    // top-level `viewport` block; only meaningful once at least one shot succeeded.
    PinWrightRenderCapture::FViewportCaptureOutput LastCapture;

    // ---- The scoped view mode. Parsed HERE rather than with the other arguments because it is
    // given the resolved viewport client: that is what lets a mode whose sub-visualisation is
    // already selected in the editor through instead of refusing it. Read ONCE for the whole
    // burst, so a difference between two instants can never be a render-mode difference. ----
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

    // ---- Pose pre-pass. The BOUNDS half of the old pre-pass now belongs to the resolver, which
    // walked the same instants and unioned them; this is the half it cannot do, because it needs
    // the skeletal COMPONENT rather than the actor: the component-space pose at every instant, so
    // the burst can state whether anything moved. It also recovers the update method actually
    // applied, which an older engine can silently downgrade. ----
    TArray<FBurstFrame> BurstFrames;
    BurstFrames.Reserve(FramePlan.Num());
    FString AppliedMethod = CanonicalMethod;
    int32 UnsampledPoseCount = 0;

    for (const FFrameTime& DisplayTime : FramePlan)
    {
        AppliedMethod = SequencePlayheadUtils::ApplyPlayheadPosition(
            DisplayTime, UpdateMethod, CanonicalMethod, bForceUpdate);

        FBurstFrame Frame;
        Frame.DisplayTime = DisplayTime;
        Frame.Seconds = DisplayRate.AsSeconds(DisplayTime);
        if (bActorSubject && !PinWrightAnimationPose::SamplePose(SkeletalComponent, Frame.Pose))
        {
            ++UnsampledPoseCount;
        }
        BurstFrames.Add(MoveTemp(Frame));
    }

    // `radius` means CAMERA DISTANCE here, and it is accepted in both spellings: the top-level
    // argument this verb has always had, and `subject.radius`, which is the same number for a
    // bare point — a point has no size, so its orbit radius doubles as the framing extent
    // (the rule camera.orbit_shots has kept since it shipped). Without the second spelling a
    // world subject carrying only subject.radius would be fit-framed against its own radius
    // instead of orbited at it.
    const bool bTopLevelRadius = HasField(TEXT("radius"));
    const float RadiusParam = bTopLevelRadius
        ? static_cast<float>(Ctx.GetNumber(TEXT("radius"), 0.0))
        : SubjectRequest.Radius;
    const bool bRadiusProvided = bTopLevelRadius || SubjectRequest.Radius > 0.0f;
    if (Resolved.BoundsRadius <= 0.0 && !(bRadiusProvided && RadiusParam > 0.0f))
    {
        // Only a `world` subject reaches this: an actor with no bounds at any sampled instant was
        // already refused by name inside the resolver, with the message this verb has always sent.
        // A level where nothing has finite renderable bounds frames nothing, and guessing a
        // distance would produce a plausible picture of nowhere.
        Ctx.SendError(ErrorCodes::ERR_BOUNDS_EMPTY,
            TEXT("Nothing in this level reported finite renderable bounds, so no camera can be framed on it. "
                 "Pass subject:{kind:\"world\", point:{x,y,z}, radius:N} to name the spot to shoot instead."));
        return true;
    }

    const FVector Center = Resolved.BoundsOrigin;
    const float BoundsRadius = FMath::Max(static_cast<float>(Resolved.BoundsRadius), 1.0f);
    const float BaseDistance = (bRadiusProvided && RadiusParam > 0.0f)
        ? RadiusParam
        : ComputeFitDistance(BoundsRadius, Fov, Padding);

    // ---- Pose verdict, computed before any capture so it is available even if the viewport
    // path fails afterwards. Each instant is compared against BOTH its predecessor and the first
    // instant: a pose that moves and returns would read as unchanged against its neighbour
    // alone, and a pose that drifts by a hair per frame would read as unchanged against the
    // first alone. ----
    TArray<TSharedPtr<FJsonValue>> FramesJson;
    FramesJson.Reserve(BurstFrames.Num());
    bool bAnyPoseChanged = false;
    double MaxComponentTranslationCm = 0.0;
    for (int32 Index = 0; Index < BurstFrames.Num(); ++Index)
    {
        TSharedPtr<FJsonObject> FrameObj = MakeShared<FJsonObject>();
        FrameObj->SetNumberField(TEXT("index"), Index);
        FrameObj->SetNumberField(TEXT("frame"), BurstFrames[Index].DisplayTime.AsDecimal());
        FrameObj->SetNumberField(TEXT("time"), BurstFrames[Index].Seconds);
        // Omitted entirely on a `world` subject: there is no skeletal component, so there is no
        // pose, and `poseSampled: false` would read as "the animation system never evaluated" —
        // a finding — rather than "this subject has no pose axis", which is a fact about the
        // request. Present only when it has something to say.
        if (bActorSubject)
        {
            FrameObj->SetBoolField(TEXT("poseSampled"), BurstFrames[Index].Pose.bValid);
        }
        if (BurstFrames[Index].Pose.bValid)
        {
            FrameObj->SetNumberField(TEXT("boneCount"), BurstFrames[Index].Pose.BoneTransforms.Num());
        }
        if (bActorSubject && Index > 0)
        {
            const PinWrightAnimationPose::FPoseDelta FromPrevious =
                PinWrightAnimationPose::ComparePose(BurstFrames[Index - 1].Pose, BurstFrames[Index].Pose);
            const PinWrightAnimationPose::FPoseDelta FromFirst =
                PinWrightAnimationPose::ComparePose(BurstFrames[0].Pose, BurstFrames[Index].Pose);

            TSharedPtr<FJsonObject> PrevObj = MakeShared<FJsonObject>();
            PinWrightAnimationPose::AddPoseDeltaFields(FromPrevious, PrevObj);
            FrameObj->SetObjectField(TEXT("poseDeltaFromPrevious"), PrevObj);

            TSharedPtr<FJsonObject> FirstObj = MakeShared<FJsonObject>();
            PinWrightAnimationPose::AddPoseDeltaFields(FromFirst, FirstObj);
            FrameObj->SetObjectField(TEXT("poseDeltaFromFirst"), FirstObj);

            bAnyPoseChanged = bAnyPoseChanged ||
                PinWrightAnimationPose::PoseChanged(FromPrevious) ||
                PinWrightAnimationPose::PoseChanged(FromFirst);
            MaxComponentTranslationCm =
                FMath::Max(MaxComponentTranslationCm, FromFirst.ComponentTranslationCm);
        }
        FramesJson.Add(MakeShared<FJsonValueObject>(FrameObj));
    }

    // ---- Pose generation. Every shot of the burst is planned before a single frame is drawn:
    // instants outer, angles inner, so a filename listing reads as a strip and so the camera for
    // a given view index is IDENTICAL at every instant — it is computed once, from one centre and
    // one distance, and copied. That is the burst's whole comparison guarantee, and it is now a
    // property of the generator rather than of a loop remembering not to re-solve. ----
    const FString BurstStamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));

    PinWrightPoseCapture::FPoseListCaptureRequest PoseRequest;
    PoseRequest.Width = Width;
    PoseRequest.Height = Height;
    PoseRequest.FilenamePrefix = TEXT("AnimShots");
    PoseRequest.Subdirectory = TEXT("AnimShots");
    PoseRequest.Exposure = ExposurePin;
    PoseRequest.ViewMode = ViewModePin;
    PoseRequest.bHideEditorSprites = bHideEditorSprites;
    // This verb has allowed frames x angles up to 24 since it shipped and the primitive's own
    // default is 8. Keeping the verb's bound is what stops the conversion silently shortening
    // bursts callers have been taking for months; the value in force is published as
    // poseSet.maxPosesPerCall, so the bound is never implicit.
    PoseRequest.MaxPoses = GMaxOrbitShots;
    // The time axis, handed to the primitive instead of scrubbed here. It drives the subject to
    // each pose's instant exactly once — including once for pose 0 BEFORE the warm-up frame, so
    // the throwaway shot pages in the content the set actually shows at the moment it shows it.
    PoseRequest.SubjectTimeSetter = SubjectTimeSetter;
    // The bounds the per-shot `framing` verdict is measured against: from the static union, never
    // from the posed subject at that shot's instant. Bounds that moved between shots would make
    // "the subject left the frame" and "the bounds grew" the same reading.
    PoseRequest.BoundsOrigin = Center;
    PoseRequest.BoundsRadius = static_cast<double>(BoundsRadius);
    PoseRequest.Poses.Reserve(TotalShots);

    // Parallel to PoseRequest.Poses, so the response can name each shot's instant and angle
    // without re-deriving them from an index.
    struct FBurstShotPlan
    {
        int32 FrameIndex = 0;
        int32 ViewIndex = 0;
        float Azimuth = 0.0f;
        float Elevation = 0.0f;
        float RequestedAzimuth = 0.0f;
        float RequestedElevation = 0.0f;
        bool bOrthoAxisSnapped = false;
    };
    TArray<FBurstShotPlan> ShotPlan;
    ShotPlan.Reserve(TotalShots);

    for (int32 FrameIndex = 0; FrameIndex < BurstFrames.Num(); ++FrameIndex)
    {
        for (int32 ViewIndex = 0; ViewIndex < ViewPlan.Num(); ++ViewIndex)
        {
            const FPlannedShot& Shot = ViewPlan[ViewIndex];
            FBurstShotPlan Planned;
            Planned.FrameIndex = FrameIndex;
            Planned.ViewIndex = ViewIndex;
            Planned.Azimuth = Shot.Azimuth;
            Planned.Elevation = Shot.Elevation;
            Planned.RequestedAzimuth = Shot.Azimuth;
            Planned.RequestedElevation = Shot.Elevation;
            if (Shot.ProjectionMode == TEXT("orthographic"))
            {
                Planned.bOrthoAxisSnapped =
                    SnapOrbitAnglesToOrthographicAxis(Planned.Azimuth, Planned.Elevation);
            }

            PinWrightPoseCapture::FCameraPose Pose;
            Pose.ProjectionMode = Shot.ProjectionMode;
            Pose.Fov = Fov;
            // Frame index leads the filename so an ls of the output directory reads as a strip.
            // az/el are rounded to ints so the name carries no '.', '/' or '\' that
            // MakeScreenshotFilename would reject (which would fall back to a colliding auto-name
            // and leave only the last shot of each second on disk).
            Pose.Filename = FString::Printf(TEXT("AnimShots_%s_f%02d_v%02d_az%d_el%d.png"),
                *BurstStamp, FrameIndex, ViewIndex,
                FMath::RoundToInt(Planned.Azimuth), FMath::RoundToInt(Planned.Elevation));
            PlaceOrbitCamera(Center, Planned.Azimuth, Planned.Elevation, BaseDistance,
                Pose.Location, Pose.Rotation);
            if (Shot.ProjectionMode == TEXT("orthographic"))
            {
                Pose.OrthoWidth = ComputeOrthoWorldWidth(BoundsRadius, Padding, Width, Height);
            }
            // The instant this pose photographs, in seconds. The plan is already snapped to whole
            // display frames above, so the setter's seconds -> whole-frame conversion is exact and
            // the burst stays on the sequence's own grid.
            Pose.SubjectTimeSeconds = static_cast<float>(BurstFrames[FrameIndex].Seconds);

            PoseRequest.Poses.Add(MoveTemp(Pose));
            ShotPlan.Add(Planned);
        }
    }

    // ---- One call. Everything below the pose list — the warm-up frame, the pose bound, the
    // exposure pin, the scoped view mode, sprite suppression, orbit suppression, both viewport
    // transform slots, the aim verification, the framing verdict and the subject time driver —
    // belongs to the primitive, so a fix there reaches every capture verb instead of one. ----
    PinWrightPoseCapture::FPoseListCaptureOutput PoseResult;
    const bool bCaptureComplete = PinWrightPoseCapture::CaptureCameraPoses(
        *ViewportClient, SceneViewport, PoseRequest, PoseResult, ErrCode, ErrMsg);
    if (!bCaptureComplete && PoseResult.Captures.Num() == 0)
    {
        // Nothing was captured, so there is nothing to report but the failure.
        Ctx.SendError(ErrCode, ErrMsg);
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> ShotsJson;
    ShotsJson.Reserve(PoseResult.Captures.Num());
    int32 BlankShots = 0;

    for (int32 ShotIndex = 0; ShotIndex < PoseResult.Captures.Num(); ++ShotIndex)
    {
        const PinWrightRenderCapture::FViewportCaptureOutput& Capture = PoseResult.Captures[ShotIndex];
        const FBurstShotPlan& Planned = ShotPlan[ShotIndex];
        // The view mode is a property of the viewport, not of a shot, and nothing in the set
        // changes it — reported once at the top level rather than repeated per shot.
        LastCapture = Capture;

        TSharedPtr<FJsonObject> ShotObj = MakeShared<FJsonObject>();
        ShotObj->SetNumberField(TEXT("frameIndex"), Planned.FrameIndex);
        ShotObj->SetNumberField(TEXT("frame"), BurstFrames[Planned.FrameIndex].DisplayTime.AsDecimal());
        ShotObj->SetNumberField(TEXT("time"), BurstFrames[Planned.FrameIndex].Seconds);
        ShotObj->SetNumberField(TEXT("viewIndex"), Planned.ViewIndex);
        TSharedPtr<FJsonObject> AngleObj = MakeShared<FJsonObject>();
        AngleObj->SetNumberField(TEXT("azimuth"), Planned.Azimuth);
        AngleObj->SetNumberField(TEXT("elevation"), Planned.Elevation);
        ShotObj->SetObjectField(TEXT("angle"), AngleObj);
        if (Planned.bOrthoAxisSnapped)
        {
            ShotObj->SetBoolField(TEXT("orthoAxisSnapped"), true);
            ShotObj->SetNumberField(TEXT("requestedAzimuth"), Planned.RequestedAzimuth);
            ShotObj->SetNumberField(TEXT("requestedElevation"), Planned.RequestedElevation);
        }
        AddShotFields(Capture, PoseResult.Requests[ShotIndex], ShotObj);
        // Was the subject in this frame at all? Written by the primitive, and written NOTHING
        // when the verdict could not be evaluated — `blank` cannot answer this, because it
        // catches BLACK frames and a lit backdrop with the subject off-screen is not black.
        PinWrightPoseCapture::AddPoseFramingField(PoseResult, ShotIndex, ShotObj);
        MaybeAddBase64(Capture.Path, bInline, ShotObj);
        if (Capture.ImageStats.bBlank)
        {
            ++BlankShots;
        }
        ShotsJson.Add(MakeShared<FJsonValueObject>(ShotObj));
    }

    // ---- Response. The images are the deliverable; the numbers say whether they can be trusted. ----
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("shots"), ShotsJson);
    Result->SetNumberField(TEXT("count"), ShotsJson.Num());
    Result->SetArrayField(TEXT("frames"), FramesJson);
    Result->SetNumberField(TEXT("frameCount"), BurstFrames.Num());
    Result->SetNumberField(TEXT("viewCount"), ViewPlan.Num());
    Result->SetStringField(TEXT("framePlanSource"), FramePlanSource);
    if (bActorSubject)
    {
        Result->SetStringField(TEXT("actorName"), ActorName);
    }
    Result->SetStringField(TEXT("sequencePath"), SequencePath);
    Result->SetObjectField(TEXT("displayRate"), MovieSceneJsonUtils::MakeFrameRateObject(DisplayRate));
    Result->SetObjectField(TEXT("tickResolution"), MovieSceneJsonUtils::MakeFrameRateObject(TickResolution));
    Result->SetStringField(TEXT("updateMethod"), AppliedMethod);
    Result->SetBoolField(TEXT("forcedUpdate"), bForceUpdate);
    Result->SetBoolField(TEXT("opened"), bOpened);
    Result->SetBoolField(TEXT("pausedPlayback"), bPausedPlayback);
    Result->SetBoolField(TEXT("restoredPlayhead"), bRestorePlayhead);
    Result->SetNumberField(TEXT("width"), Width);
    Result->SetNumberField(TEXT("height"), Height);
    // ONE vocabulary across every capture verb, classified from the default edge actually used
    // rather than from a flag beside it. `burstBudget` was this verb's own spelling of the shared
    // `budget` rule and is the one string in this wave that moves on the wire; `singleStill`
    // survives as a distinct reason even though every omitted-size policy resolves to 768.
    Result->SetStringField(TEXT("resolutionSource"),
        ResolveResolutionSource(bWidthProvided || bHeightProvided, DefaultSource));
    Result->SetNumberField(TEXT("cameraDistance"), BaseDistance);
    Result->SetObjectField(TEXT("framedCenter"), PinWrightRenderCapture::MakeVectorObject(Center));
    Result->SetNumberField(TEXT("framedRadius"), BoundsRadius);
    Result->SetNumberField(TEXT("blankShots"), BlankShots);

    // What the base primitive did with the pose list: how many poses were asked for, how many
    // were captured, how many the bound dropped, what the warm-up frame did, how many times the
    // subject time was applied, and how many shots the subject was provably outside.
    Result->SetObjectField(TEXT("poseSet"),
        PinWrightPoseCapture::MakePoseSetInfoObject(PoseResult));

    // How the poses were distributed, and the seed that reproduces them. Published unconditionally
    // so a burst can always be retaken; one serializer, shared with camera.orbit_shots, so the
    // block cannot drift between the two verbs a caller compares sets across.
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

    // What the burst was OF. Emitted always, because this verb always has a subject: `actorName`
    // normalises into one, so a caller who never sent a `subject` object still gets the block
    // describing what the resolver framed and where those bounds came from.
    {
        // `kind`, `name`, `captureSource`, `boundsSource`, the bounds and the time window all
        // come from the ONE serializer, so this verb cannot spell any of them differently from
        // the other seven. Nothing is re-written here.
        TSharedPtr<FJsonObject> SubjectInfo =
            PinWrightCaptureSubject::MakeSubjectInfoObject(Resolved);
        // The three facts the verb knows and the resolver's block does not: how many instants the
        // bounds union was measured across, the largest single-instant radius it exceeded, and how
        // far the subject travelled between the furthest two origins. A zero separation means the
        // union IS the single-instant answer and says nothing extra, which is worth knowing
        // before trusting a framing verdict.
        if (LevelReport.SampledBounds.InstantsSampled > 0)
        {
            SubjectInfo->SetNumberField(TEXT("boundsInstantsSampled"),
                LevelReport.SampledBounds.InstantsSampled);
            SubjectInfo->SetNumberField(TEXT("boundsMaxSingleInstantRadius"),
                LevelReport.SampledBounds.MaxSingleInstantRadius);
            SubjectInfo->SetNumberField(TEXT("boundsMaxOriginSeparation"),
                LevelReport.SampledBounds.MaxOriginSeparation);
        }
        Result->SetObjectField(TEXT("subject"), SubjectInfo);
    }

    // The two verdicts, stated plainly. `poseChanged` is the one that decides whether the images
    // are worth reading at all. Omitted entirely on a `world` subject: there is no skeletal
    // component to sample, so `poseSampled: false` would report a finding where there is only a
    // request shape.
    const bool bPoseSampled = bActorSubject && UnsampledPoseCount < BurstFrames.Num();
    if (bActorSubject)
    {
        Result->SetBoolField(TEXT("poseSampled"), bPoseSampled);
        Result->SetBoolField(TEXT("poseChanged"), bAnyPoseChanged);
        Result->SetNumberField(TEXT("actorTranslationCm"), MaxComponentTranslationCm);
    }

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
        // Mirror the non-Lit warning into warnings[] as well. This verb already has a warnings
        // channel that callers read, and a burst captured in wireframe is exactly as worthless
        // for review as one whose pose never changed — the two belong in the same place.
        FString ViewModeWarning;
        if (ViewportInfo->TryGetStringField(TEXT("viewModeWarning"), ViewModeWarning))
        {
            AddWarning(ViewModeWarning);
        }
    }
    // ---- A partial set is REPORTED, never discarded. The hand-rolled loop this replaced threw
    // away every shot it had already taken when one failed, so a five-instant burst that died on
    // the last frame returned an error and nothing else. The shots that exist are still the
    // deliverable; `partial` and `captureError` are what stop them reading as a complete set. ----
    if (!bCaptureComplete)
    {
        Result->SetBoolField(TEXT("partial"), true);
        TSharedPtr<FJsonObject> CaptureError = MakeShared<FJsonObject>();
        CaptureError->SetStringField(TEXT("code"), ErrCode);
        CaptureError->SetStringField(TEXT("message"), ErrMsg);
        CaptureError->SetNumberField(TEXT("failedAtShotIndex"), ShotsJson.Num());
        Result->SetObjectField(TEXT("captureError"), CaptureError);
        AddWarning(FString::Printf(
            TEXT("PARTIAL SET: %d of the %d planned shots were captured, then shot %d failed with %s: %s. ")
            TEXT("The shots below are real and usable, but the set is incomplete — do not read a missing angle ")
            TEXT("or instant as a finding about the subject."),
            ShotsJson.Num(), TotalShots, ShotsJson.Num(), *ErrCode, *ErrMsg));
    }
    if (RoundedInstantCount > 0)
    {
        AddWarning(FString::Printf(
            TEXT("%d of %d requested instants were not whole display frames and were rounded onto the sequence's ")
            TEXT("own grid; each shot's `frame` and `time` report the instant actually photographed. A sub-frame ")
            TEXT("position cannot be held across a burst — the remainder accumulates through the ")
            TEXT("display-rate/tick-resolution conversion and walks the samples off the grid."),
            RoundedInstantCount, BurstFrames.Num()));
    }
    if (!bActorSubject)
    {
        // Said out loud rather than left to be inferred from three missing keys. A caller who
        // knows this verb for its pose verdict has to be told why this response has none.
        AddWarning(TEXT(
            "This burst framed the LEVEL, not an actor, so there is no pose to measure and no poseSampled / "
            "poseChanged verdict in this response. The images show the sequence at each instant; whether anything "
            "animated is not established by them. Pass subject:{kind:\"actor\"} (or actorName) to get the pose "
            "evidence."));
    }
    // The whole verdict chain is actor-only. Guarded as a block rather than per branch: an
    // `else if (!bAnyPoseChanged)` reached with a world subject would announce that "the pose is
    // identical at every sampled instant" about a subject that has no pose at all.
    if (bActorSubject)
    {
        if (!bPoseSampled)
        {
            // "Could not measure" must never read as "measured clean".
            AddWarning(FString::Printf(
                TEXT("The pose could not be sampled at any instant: '%s' reported no component-space bone transforms, ")
                TEXT("which means its animation system never evaluated. The images cannot be trusted to show an animated pose."),
                *ActorName));
        }
        else if (!bAnyPoseChanged && MaxComponentTranslationCm > PinWrightAnimationPose::BoneMoveThresholdCm)
        {
            AddWarning(FString::Printf(
                TEXT("The mesh moved %.2f cm across the burst but its POSE never changed: it is sliding in bind pose. ")
                TEXT("The sequence is driving a transform track with no skeletal animation track behind it — check ")
                TEXT("sequencer.list_tracks for a MovieSceneSkeletalAnimationTrack on this binding."),
                MaxComponentTranslationCm));
        }
        else if (!bAnyPoseChanged)
        {
            AddWarning(TEXT(
                "The pose is identical at every sampled instant. Either no animation is bound to this actor in the "
                "sequence, or the sampled frames all land on the same pose. Every image in this set shows the same "
                "silhouette; comparing them proves nothing."));
        }
    }
    if (bActorSubject && UnsampledPoseCount > 0 && bPoseSampled)
    {
        AddWarning(FString::Printf(
            TEXT("%d of %d instants could not be pose-sampled and are excluded from the pose verdict."),
            UnsampledPoseCount, BurstFrames.Num()));
    }
    if (BlankShots > 0)
    {
        AddWarning(FString::Printf(
            TEXT("%d of %d shots came back near-uniform black (see each shot's imageStats). A blank set reads as a ")
            TEXT("clean success unless you check this — verify the viewport, lighting and camera before trusting it."),
            BlankShots, ShotsJson.Num()));
    }
    // Mirrored into warnings[] for the same reason as the non-Lit view mode: a burst whose
    // subject is outside the frame is exactly as worthless for review as one whose pose never
    // changed, and `blank` cannot see it — a lit backdrop with the subject off-screen is not
    // black. The set-level string is the primitive's; this verb only routes it to the channel its
    // callers already read.
    if (PoseResult.PosesOutOfFrame > 0)
    {
        const TSharedPtr<FJsonObject>* PoseSetInfo = nullptr;
        FString OutOfFrameWarning;
        if (Result->TryGetObjectField(TEXT("poseSet"), PoseSetInfo) && PoseSetInfo &&
            (*PoseSetInfo)->TryGetStringField(TEXT("outOfFrameWarning"), OutOfFrameWarning))
        {
            AddWarning(OutOfFrameWarning);
        }
    }
    if (AppliedMethod != CanonicalMethod)
    {
        AddWarning(FString::Printf(
            TEXT("Requested updateMethod '%s' but '%s' was applied; this engine version cannot honour the request, ")
            TEXT("and positions were rounded to whole display frames."),
            *CanonicalMethod, *AppliedMethod));
    }
    if (Warnings.Num() > 0)
    {
        Result->SetArrayField(TEXT("warnings"), Warnings);
    }

    Ctx.SendSuccess(Result);
    return true;
}
