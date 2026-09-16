// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"

class FEditorViewportClient;
class FSceneViewport;
class FJsonObject;

// The base multi-shot capture primitive: a LIST OF FREE CAMERAS, captured in one call, with one
// restore path for everything the set touched.
//
// WHY A FREE CAMERA LIST AND NOT AN ORBIT. `[{location, rotation}]` is strictly more general -
// orbit poses compute down into it, and the reverse is false. A camera looking along a corridor,
// framing two objects with neither at the centre, or flying a path has no orbit expression, so an
// orbit-shaped base would wall those off invisibly. render.capture_asset_preview has taken
// `location` + `rotation` since it shipped; this is that surface, shared.
//
// A free-camera list is only honourable because of the orbit-suppression fix in
// CaptureEditorViewportToPng: before ToggleOrbitCamera(false), an editor preview viewport
// discarded the requested location's DIRECTION and kept only its distance from the pivot, so a
// (location, rotation) primitive could not have worked at all. That defect was diagnosed and
// fixed TWICE independently - AnimationPreviewCaptureHandler.cpp:34-45 spells it out for the
// Persona path, and the static-mesh path carried the identical bug undiagnosed until it was found
// again - which is the whole argument for having one base instead of a loop per verb.
//
// WHAT THIS OWNS. Per-set concerns: the pose bound and honest truncation, the throwaway warm-up
// shot, driving the subject to each pose's instant, the framing verdict per shot, and the
// set-level report. Per-frame concerns - the exposure pin, the scoped view mode, sprite
// suppression, orbit suppression, both viewport transform slots, the aim verification and the
// blank check - are owned by CaptureEditorViewportToPng, which already carries the unconditional
// ON_SCOPE_EXIT discipline for all of them. This primitive does not duplicate that; it is the
// layer above it.
//
// WHY FRAMING IS HERE AND NOT THERE. EvaluateBoundsFraming needs the subject's bounds, and
// FViewportCaptureRequest carries none - a frame does not know what it is a frame OF. So the
// bounds are a SET-level input here and the verdict is measured per shot against the pose the
// renderer resolved to. Before this, exactly one verb ran that check and the other seven returned
// frames that could be pure backdrop with `blank` false and litPixelFraction 1.
namespace PinWrightPoseCapture
{
    // Hard ceiling on poses per call. Each pose moves a real editor camera, resizes the viewport
    // and does a full offscreen readback with a settle loop, so the cost is seconds per shot on a
    // busy scene and an unbounded list would stall the editor for minutes with no way to cancel.
    // Eight is the working set an agent can actually look at in one pass.
    //
    // A longer list is TRUNCATED AND REPORTED, never silently shortened: `posesRequested`,
    // `posesCaptured` and `posesTruncated` are all in the response, so a caller who asked for 20
    // learns that 12 were dropped instead of concluding the last 12 poses were degenerate.
    constexpr int32 MaxPosesPerCall = 8;

    // ---- the subject time seam ----
    //
    // Put the subject at one instant. Returns false and fills the error pair when the subject
    // cannot reach it; a set that never asks for a time never calls this and never sees a refusal.
    //
    // THE SAME TYPE as PinWrightCaptureSubject::FSubjectTimeSetter, not merely the same shape: a
    // `using` declaration names a type, it does not create one, so a resolver's setter passes
    // straight into FPoseListCaptureRequest with no conversion and no adapter. It is declared HERE
    // rather than included from the resolver on purpose - this primitive owns camera handling and
    // knows nothing about subjects (see FCameraPose), and a hard include of the subject registry
    // would invert that layering for the sake of one TFunction alias. If the resolver's signature
    // ever moves, this moves with it.
    using FSubjectTimeSetter =
        TFunction<bool(double TimeSeconds, FString& OutErrCode, FString& OutErrMsg)>;

    // One camera. No notion of a target, a pivot or a subject - that is the generator's job.
    struct FCameraPose
    {
        FVector Location = FVector::ZeroVector;
        FRotator Rotation = FRotator::ZeroRotator;
        // "perspective" | "orthographic". Per-pose because a canonical set mixes them.
        FString ProjectionMode = TEXT("perspective");
        float Fov = 50.0f;
        // World centimetres, orthographic only. See FViewportCaptureRequest::OrthoWidth.
        float OrthoWidth = PinWrightRenderCapture::DefaultOrthoWorldWidth;
        // Output filename for this shot. Empty auto-names from a second-resolution timestamp,
        // which collides for shots taken inside the same second - a caller capturing a set should
        // supply distinct names.
        FString Filename;

        // ---- subject state for this shot ----
        //
        // The instant the subject is driven to before this pose is captured, through
        // FPoseListCaptureRequest::SubjectTimeSetter. Unset means "leave the subject where it is",
        // which is the right answer for a kind with no time axis and for a set that only moves the
        // camera. A time set here with NO setter bound is REFUSED, not dropped: a silently ignored
        // instant produces a capture of the wrong moment that is indistinguishable from a capture
        // of the right one.
        //
        // The layering, unchanged from when this was an extension point: camera handling is
        // universal, the subject step is pluggable. A static mesh ignores the time entirely (no
        // setter, and a pose carrying one is the refusal above); an animation preview SCRUBS to it
        // (cheap and deterministic); a Niagara system SIMULATES to it via
        // UNiagaraComponent::AdvanceSimulation. This primitive knows none of that - it calls the
        // closure once per pose and reports how many times it did.
        //
        // NOT A REPRODUCIBILITY PROMISE for particles. A Niagara system is a simulation, so a
        // capture set only repeats if the system is deterministic AND the tick delta is fixed;
        // neither has been verified here. Do not document particle capture sets as reproducible.
        //
        // Stays a float while the setter takes a double: float widens to double exactly, so the
        // driver loses nothing, while a double here would make every caller assigning an engine
        // animation time (all float) narrow at the assignment.
        TOptional<float> SubjectTimeSeconds;
    };

    struct FPoseListCaptureRequest
    {
        TArray<FCameraPose> Poses;
        // ONE pixel size for the whole set, and deliberately no per-pose override. A capture size
        // that VARIES within an editor session trips
        // `Assertion failed: ProxyMap.Num() == TestSizeX * TestSizeY` in FViewport::GetHitProxy -
        // an incident that cost 66 actors and 125 emitters of unsaved level state - while a
        // constant size is proven over 48 cycles. Every caller resolves its resolution ONCE and
        // passes it here; making the size set-level is what turns that rule into a property of the
        // code rather than of each verb remembering it.
        int32 Width = PinWrightRenderCapture::DefaultCaptureEdge;
        int32 Height = PinWrightRenderCapture::DefaultCaptureEdge;
        // Auto-name prefix and output subdirectory, passed straight through per shot.
        FString FilenamePrefix;
        FString Subdirectory;

        // Set-level viewport state. Read ONCE by the caller and applied identically to every shot,
        // so "every shot in this set was taken under the same viewport state" is a property of the
        // code rather than of the payload staying constant between iterations.
        PinWrightRenderCapture::FExposurePin Exposure;
        bool bHideEditorSprites = false;
        PinWrightRenderCapture::FViewModePin ViewMode;
        // The preview-scene rig, applied identically to every shot in the set. SET-LEVEL for a
        // stronger reason than the fields above: a rig that varied between shots would make "the
        // subject changed" and "the light moved" the same reading, which is the one comparison an
        // orbit set exists to make. CaptureCameraPoses owns one guard around the entire set, so
        // the shared profile is snapshotted once and restored only after the final control frame.
        PinWrightPreviewSceneRig::FPreviewSceneRigPin PreviewSceneRig;
        // Internal handoff used only by CaptureCameraPoses after it creates that set-level guard.
        bool bPreviewSceneRigAlreadyScoped = false;
        PinWrightPreviewSceneRig::FPreviewSceneRigReport PreviewSceneRigAtSetEntry;
        // Whether that one guard drove the sky/reflection capture drain, and whether a capture was
        // still queued when it returned. Set-level for the same reason the rig is: the drain runs
        // once for the whole set, so every shot in the set is lit by it and every shot's receipt
        // reports the same answer.
        bool bPreviewSceneCaptureUpdatedAtSetEntry = false;
        bool bPreviewSceneCaptureIncompleteAtSetEntry = false;
        PinWrightRenderCapture::FViewportCaptureSetContext* ViewportCaptureSetContext = nullptr;
        // Also internal: the real viewport wrapper enables the extra pose-0 control. Keeping this
        // off on the injected primitive preserves its use as a minimal sequence test seam.
        bool bMeasurePoseRepeatability = false;

        float ViewDistanceScale = 0.0f;
        bool bViewDistanceScaleProvided = false;
        bool bAutoViewDistanceScale = false;
        bool bRejectBlankCapture = false;
        bool bAllowBlank = false;

        // Keep the pixels for every real shot in the output. This is used by an animation burst
        // to verify that a large pose change reached the rendered image. It is false by default
        // because retaining a full frame for every shot is expensive.
        bool bRetainPixels = false;

        // Effective ceiling for THIS call. Defaults to MaxPosesPerCall; a verb that already
        // shipped a different, higher bound sets its own here so converting it onto this
        // primitive does not silently shorten sets its callers have been taking for months
        // (camera.orbit_shots has allowed 24 since it shipped). The value in force is published
        // as `maxPosesPerCall`, so the bound is never implicit.
        int32 MaxPoses = MaxPosesPerCall;

        // Take one throwaway capture before the first real shot and delete its file.
        //
        // WHY. The first capture into a freshly opened or freshly resized viewport is measurably
        // dark - 2.26 stops on a pinned exposure in one measured case - and
        // viewport.warmup.settled does NOT catch every instance of it, because the frame can stop
        // changing while the ambient contribution is still missing. A set whose first shot is
        // colder than the rest is not a set. One discarded frame removes the trap for every
        // caller instead of documenting it for each.
        //
        // Costs one extra draw + readback per call, so a caller that knows the viewport is warm
        // can turn it off. Reported either way (`warmupShotTaken`), never assumed.
        bool bWarmupShot = true;

        // Drives FCameraPose::SubjectTimeSeconds. Unbound - the default - means this set has no
        // time axis at all: every shot is captured with the subject wherever it already is, and a
        // pose that carries a time is refused rather than captured at the wrong instant.
        //
        // CALLED ONCE PER POSE AND NEVER MORE. The warm-up frame reuses Poses[0], so the driver
        // runs for pose 0 BEFORE the warm-up and the shot loop then skips it. A setter can be
        // expensive (a Niagara advance is N game-thread ticks) and is not required to be
        // idempotent, so a second application for the same pose is not a free extra safety - it is
        // a second, unequal application of something that may not repeat.
        FSubjectTimeSetter SubjectTimeSetter;

        // ---- the subject coverage seam ----
        //
        // Show or hide the subject without moving the camera or touching anything else in the
        // scene. Unbound - the default - means this set cannot measure coverage, which is reported
        // as an ABSENT coverage number rather than a zero.
        //
        // WHY THIS EXISTS, and it is not a nicety. `boundsInFrame` answers "is the subject's
        // bounding sphere inside the frustum". That stays TRUE when the subject is a dot in the
        // middle of the frame, when the floor plane occludes it, and - the case that produced this
        // seam - when the subject has no particles alive at the captured instant and the frame is
        // pure backdrop. A real capture of an empty preview scene reported boundsInFrame:true,
        // blank:false and litPixelFraction:1.0: every published signal said healthy and the picture
        // contained nothing. No geometric measure can catch that, because geometrically the bounds
        // ARE in frame; only pixels can.
        //
        // The measurement is therefore DIFFERENTIAL: draw the pose once with the subject hidden,
        // once with it shown, and count the pixels that changed. That is the only definition of
        // "what proportion of the frame the subject occupies" that stays true for a dot, for an
        // occluded subject, and for a subject that is not there at all.
        using FSubjectVisibilitySetter =
            TFunction<bool(bool bVisible, FString& OutErrCode, FString& OutErrMsg)>;
        FSubjectVisibilitySetter SubjectVisibilitySetter;

        // Take the extra reference frame per pose. Costs one additional draw + readback per shot
        // and holds two full-size pixel buffers at a time, so a caller that only wants pictures
        // turns it off. Ignored - not an error - when no visibility setter is bound, because a kind
        // that cannot hide its subject cannot be measured this way, and saying so through an absent
        // number is the honest answer.
        //
        // THE CALLER'S OWN CHOICE, not handler policy: it is the wire parameter `measureCoverage`
        // on render.capture_asset_preview, which defaults it to true and honours an explicit false.
        // Defaulting FALSE here rather than true is deliberate - a verb that has not thought about
        // the differential must not start paying for it silently, so the primitive stays quiet and
        // the verb that offers the knob is the one that turns it on.
        bool bMeasureSubjectCoverage = false;

        // A shot whose subject covers less than this fraction of the frame gets a warning. NOT a
        // failure: a deliberately wide establishing shot is a legitimate picture, and refusing it
        // would turn a review aid into a new way for a capture to fail.
        //
        // 0.005 is 0.5% of the frame - about 5000 px at 1024x1024, roughly a 70x70 square. Below
        // that a reviewer cannot judge surface detail at all, which is what these captures are for.
        double CoverageWarnFraction = 0.005;

        // Per-channel 0-255 difference above which a pixel counts as changed. 8 rides over encoder
        // and temporal noise between two draws of the same pose while still catching a faint
        // additive particle: the sparks measured here move a pixel by 100 or more.
        int32 CoverageChannelThreshold = 8;

        // The subject's bounds, for the per-shot `framing` verdict. Radius <= 0 - the default -
        // means the caller supplied none and no `framing` block is emitted for any shot: a set
        // with no subject has nothing to be in frame, and an empty block would be the fourteenth
        // absence assertion nobody knew about.
        //
        // FROM A STATIC SOURCE, never from the posed or simulated subject, and set-level rather
        // than per-pose for exactly that reason: bounds that move between shots make "the subject
        // left the frame" and "the bounds grew" the same reading. The two correct static sources
        // in this tree are the asset's own bounds and the union of bounds sampled across every
        // instant the set will visit.
        FVector BoundsOrigin = FVector::ZeroVector;
        double BoundsRadius = 0.0;
    };

    struct FPoseListCaptureOutput
    {
        // Parallel arrays, one entry per CAPTURED pose (the warm-up shot is not among them).
        TArray<PinWrightRenderCapture::FViewportCaptureOutput> Captures;
        TArray<PinWrightRenderCapture::FViewportCaptureRequest> Requests;
        TArray<FCameraPose> Poses;
        // Parallel too: the framing verdict for each captured pose, measured against the pose the
        // RENDERER resolved to (FViewportCaptureOutput::EffectiveLocation / EffectiveRotation),
        // never against the pose that was asked for. An orbit-mode viewport places the eye from
        // its own pivot and keeps only the requested location's distance, so a capture can be
        // valid, aimed elsewhere, and framing measured off the request would call it in frame.
        // Every entry is bEvaluated=false when the request carried no bounds.
        TArray<PinWrightRenderCapture::FBoundsFramingCheck> Framings;

        int32 PosesRequested = 0;
        int32 PosesTruncated = 0;
        // The bound that was in force, published so it is never implicit.
        int32 MaxPoses = MaxPosesPerCall;
        bool bWarmupShotTaken = false;
        // The throwaway frame's file was removed. False with bWarmupShotTaken true and a non-empty
        // WarmupShotPath means one stale PNG is on disk; it is reported rather than hidden because
        // a file the caller did not ask for showing up in an output directory is otherwise
        // inexplicable.
        bool bWarmupShotDiscarded = false;
        // Where the throwaway frame was written. Kept ONLY so a failed delete can name the file
        // the caller now has to remove; empty when no warm-up shot was taken or it produced no
        // file, which is also what stops a "clean up this leftover PNG" warning firing when there
        // is no PNG to clean up.
        FString WarmupShotPath;

        // A subject time setter was bound: this set had a time axis. False means every shot shows
        // the subject wherever it already was, which is the only correct answer for a kind with no
        // time axis and must not read as a failure.
        bool bSubjectTimeDriven = false;
        // Poses whose SubjectTimeSeconds was applied. Never counts the warm-up frame - it reuses
        // Poses[0] and deliberately does not re-drive time - so on a set where every pose carries
        // a time this equals Captures.Num().
        int32 SubjectTimesApplied = 0;

        // The request carried usable bounds, so a framing verdict was attempted for every shot.
        // Published so that `posesFramingEvaluated: 0` reads as "bounds were given and nothing
        // could be measured" rather than being indistinguishable from "no bounds were given".
        bool bFramingRequested = false;
        int32 FramingsEvaluated = 0;
        // Coverage was asked for AND a visibility setter was bound, so a differential was attempted
        // for every shot. Published so that an absent coverage number reads as "this kind cannot be
        // measured that way" rather than being indistinguishable from "nobody asked".
        bool bCoverageRequested = false;
        // Reference frames actually drawn. Never counts the warm-up. A number below the shot count
        // means some shots carry no coverage figure.
        int32 CoverageReferenceShots = 0;
        // Parallel to Captures: the measured fraction of the frame the subject occupies, unset
        // where no differential ran. TOptional rather than -1 so "not measured" can never be read
        // as "measured, and very small".
        TArray<TOptional<double>> SubjectCoverage;
        // Shots whose subject was PROVABLY outside the frame. Conservative in one direction by
        // construction (FBoundsFramingCheck): a false verdict is a fact, a true one is only "not
        // provably absent", so this count never over-reports.
        int32 PosesOutOfFrame = 0;

        // A final, discarded capture of pose 0 compared with the reported pose-0 pixels. This is
        // the control for lighting/render-state stability across the complete set.
        bool bPoseRepeatabilityControlShotTaken = false;
        bool bPoseRepeatabilityControlShotDiscarded = false;
        bool bPoseRepeatabilityMeasured = false;
        FString PoseRepeatabilityNotMeasuredReason;
        double PoseRepeatabilityMeanAbsDelta = 0.0;
        int32 PoseRepeatabilityMaxDelta = 0;
        double PoseRepeatabilityChangedPixelFraction = 0.0;
        int32 PoseRepeatabilityChannelThreshold = 1;
    };

    // Draw ONE frame. The step that needs a viewport, a world and a GPU, and the only one -
    // injected so that everything else the set does (truncation, the warm-up shot, the subject
    // time driver, the framing measurement, the parallel result arrays and the whole failure
    // ordering) is assertable with none of them. That matters here specifically: a test that needs
    // a live preview viewport is a test that takes a conditional-skip path on a busy machine and
    // reports success without running its assertions - board ticket
    // B-test-skips-assertions-silently, which fired on this very cluster of verbs. It is the same
    // split ResolveEffectiveViewPose already uses against the aiming contract.
    //
    // bWarmupFrame marks the throwaway frame, which is not one of the set's shots.
    using FPoseFrameCapturer = TFunction<bool(
        const PinWrightRenderCapture::FViewportCaptureRequest& Frame,
        bool bWarmupFrame,
        PinWrightRenderCapture::FViewportCaptureOutput& OutCapture,
        FString& OutErrorCode,
        FString& OutErrorMessage)>;

    // The set's whole sequence with the frame-drawing step injected. CaptureCameraPoses is this
    // function bound to the real viewport capture and nothing else, so a test binding a stub is
    // exercising the production sequence rather than a copy of it.
    bool RunPoseListCapture(
        const FPoseListCaptureRequest& Request,
        const FPoseFrameCapturer& CaptureFrame,
        FPoseListCaptureOutput& OutResult,
        FString& OutErrorCode,
        FString& OutErrorMessage);

    // Capture every pose in the list. Returns false on the first pose that fails - or on the first
    // subject time that cannot be reached - with that pose's error code and message; poses
    // captured before it are still in OutResult, so a caller can report partial progress rather
    // than discarding work.
    bool CaptureCameraPoses(
        FEditorViewportClient& ViewportClient,
        const TSharedPtr<FSceneViewport>& SceneViewport,
        const FPoseListCaptureRequest& Request,
        FPoseListCaptureOutput& OutResult,
        FString& OutErrorCode,
        FString& OutErrorMessage);

    // The `poseSet` block: how many poses were asked for, how many were captured, how many were
    // dropped by the bound, what the warm-up shot did, and - only when the set had them - what the
    // subject time driver and the framing verdicts did. Unconditional, because a silently
    // shortened set is indistinguishable from a set that was always that length.
    TSharedPtr<FJsonObject> MakePoseSetInfoObject(const FPoseListCaptureOutput& Result);

    // Write the `framing` block for one captured pose into ShotObj, and write NOTHING when the set
    // carried no bounds or the verdict could not be evaluated. The absence is the contract: an
    // `evaluated: false` block on every shot of the seven verbs that frame nothing is noise, and a
    // block that says nothing is worse than no block. One function so seven adopters cannot each
    // decide the rule differently.
    // Measured coverage for one shot, or an unset optional where the differential did not run.
    // Exposed so a verb publishes the number the primitive measured instead of recomputing it.
    TOptional<double> GetPoseSubjectCoverage(const FPoseListCaptureOutput& Result, int32 PoseIndex);

    // The fraction of pixels that differ between two same-sized frames by more than
    // ChannelThreshold on any of R, G or B. Returns false and leaves OutFraction untouched when
    // the two buffers cannot be compared (different lengths, either empty), which is a
    // "not measured" answer and never a 0.0.
    //
    // Pure, and separated from the capture loop for exactly one reason: it is the arithmetic the
    // coverage claim rests on, and it has to be assertable against hand-built buffers with no
    // viewport, no GPU and no asset anywhere in the test.
    bool MeasureChangedPixelFraction(
        TConstArrayView<FColor> Reference,
        TConstArrayView<FColor> Subject,
        int32 ChannelThreshold,
        double& OutFraction);

    void AddPoseFramingField(
        const FPoseListCaptureOutput& Result,
        int32 PoseIndex,
        const TSharedPtr<FJsonObject>& ShotObj);

    // Write `subjectCoverage` (and `coverageWarning` when the shot is under the set's threshold)
    // into ShotObj, and write NOTHING when no differential ran for this pose. Kept separate from
    // AddPoseFramingField because the two answer different questions and have different
    // preconditions: framing needs bounds, coverage needs a visibility setter, and a shot can have
    // either, both or neither.
    void AddPoseCoverageField(
        const FPoseListCaptureOutput& Result,
        int32 PoseIndex,
        double WarnFraction,
        const TSharedPtr<FJsonObject>& ShotObj);
}
