// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Handlers/Render/CaptureSubject.h"

// EUpdatePositionMethod. Included explicitly rather than inherited through
// LevelSequenceEditorBlueprintLibrary.h so this header survives the -StrictIncludes
// -DisableUnity packaging build, for the same reason SequencePlayheadUtils.h does it.
#include "MovieSceneSequencePlayer.h"

class AActor;
class UWorld;

// The World and Actor capture subjects: the two kinds that draw through the LIVE Level Editor
// viewport rather than through an asset-editor preview.
//
// The providers register themselves into PinWrightCaptureSubject's registry at static init, so the
// generic path (PinWrightCaptureSubject::Resolve) needs no switch and no include of this file.
// Everything below the registration is exposed anyway, for two reasons that are not stylistic:
//
//  1. FSubjectRequest CANNOT CARRY A TIME SOURCE. A placed actor does not animate in the editor on
//     its own -- a bound skeletal mesh only evaluates while Sequencer drives it
//     (AnimationShotsHandler.cpp:11-18) -- so the time axis for these two kinds is a Level
//     Sequence, named by an argument that lives on the verb (`sequencePath`) and not inside
//     `subject`. FLevelTimePlan carries it, plus the sampled instants the bounds union needs.
//     A subject resolved through the registry alone therefore has NO time axis, and its setter
//     says so rather than silently doing nothing.
//  2. THE BOUNDS HALF MUST BE ASSERTABLE WITHOUT A VIEWPORT. A test that needs a live Level
//     Editor viewport takes a conditional-skip path under -unattended and then reports success
//     without having run its assertions (board ticket B-test-skips-assertions-silently). Bounds,
//     the bounds source, the bare-point refusal, the sampled union and the no-time-axis refusal
//     are all decided by ResolveLevelSubjectBounds, which acquires no viewport at all, so their
//     tests cannot skip.
namespace PinWrightCaptureSubjectLevel
{
    using PinWrightCaptureSubject::ESubjectKind;
    using PinWrightCaptureSubject::FResolvedSubject;
    using PinWrightCaptureSubject::FSubjectRequest;
    using PinWrightCaptureSubject::FSubjectTimeSetter;

    // ---- vocabulary ----
    //
    // Written once here and asserted as INDEPENDENTLY SPELLED LITERALS in the tests, the same
    // discipline TestActorFindByNameSubsystemGuard.cpp keeps for error codes: a rename of the
    // constant that forgot the wire spelling still fails.

    // The viewport these two kinds draw through. Verbatim the string RenderHandler.cpp and
    // ViewportHandler.cpp:750 already publish for the same viewport.
    inline constexpr TCHAR CaptureSourceLevelViewport[] = TEXT("levelEditorViewport");

    // Every actor in the world, or the level's ALevelBounds actor when it has one.
    inline constexpr TCHAR BoundsSourceLevelBounds[] = TEXT("levelBounds");
    // A bare point plus the caller's radius; the point has no size, so the orbit radius doubles
    // as the framing extent (CameraFrameHandler.cpp, the bHasPoint branch).
    inline constexpr TCHAR BoundsSourcePoint[] = TEXT("point");
    // One actor's bounds at the instant they were read.
    inline constexpr TCHAR BoundsSourceActorBounds[] = TEXT("actorBounds");
    // The union of one actor's bounds across every sampled instant of a time plan.
    inline constexpr TCHAR BoundsSourceSampledUnion[] = TEXT("sampledUnion");

    // The refusal camera.orbit_shots has emitted since it shipped, character-for-character. Kept
    // as a function so the one spelling reaches both the provider and any verb that wants to
    // pre-validate its own arguments before resolving.
    inline const TCHAR* PointWithoutRadiusMessage()
    {
        return TEXT("radius (> 0) is required when the target is a bare point with no bounds");
    }

    // ---- time source ----

    // How a world/actor subject is driven to an instant, and which instants the bounds union is
    // measured across. An empty SequencePath means the subject has NO time axis: the resolved
    // subject reports bTimeSupported=false and the time setter refuses by name.
    struct FLevelTimePlan
    {
        // Package path of the Level Sequence to scrub. This is the verb's own `sequencePath`
        // argument, not part of the `subject` object -- see the header comment.
        FString SequencePath;

        // The instants, in SECONDS, the caller intends to capture. Used ONLY to union the actor's
        // bounds across the burst; the time setter accepts any instant, sampled or not. Empty
        // leaves the Actor kind on single-instant bounds.
        TArray<double> InstantSeconds;

        EUpdatePositionMethod UpdateMethod = EUpdatePositionMethod::Scrub;
        FString CanonicalMethod = TEXT("scrub");
        // One immediate re-evaluation after each scrub, so the captured pixels show the frame that
        // was asked for and not the previous one (SequencePlayheadUtils.h, ApplyPlayheadPosition).
        bool bForceUpdate = true;
        bool bOpenSequenceIfNeeded = true;
        // Put the playhead back where the resolve found it, on release. Turn OFF in a verb that
        // already owns its own ON_SCOPE_EXIT restore (camera.animation_shots does today), so the
        // playhead is not written twice.
        bool bRestorePlayheadOnRelease = true;

        bool HasTimeSource() const { return !SequencePath.IsEmpty(); }
    };

    // ---- sampled bounds union ----

    // What a walk over the sampled instants measured. Published rather than reduced to one number
    // because the union is only defensible against the single-instant maximum it must exceed, and
    // because a caller reporting `framing` needs to say which of the two it used.
    struct FSampledBoundsUnion
    {
        FBox Union = FBox(ForceInit);
        // Largest bounding-sphere radius any SINGLE sampled instant reported.
        double MaxSingleInstantRadius = 0.0;
        // Largest distance between any two sampled bounds origins. Zero means the subject did not
        // move across the plan, in which case the union is the single-instant answer and says
        // nothing extra.
        double MaxOriginSeparation = 0.0;
        int32 InstantsSampled = 0;
    };

    // Union one actor's world bounds across a list of instants, driving the subject to each one
    // through the supplied setter.
    //
    // WHY THE SETTER IS A PARAMETER AND NOT LOOKED UP. This is the rule camera.animation_shots'
    // no-capture pre-pass already follows: a posed skeletal mesh's bounds change every frame, so a
    // camera solved per frame would move under the subject and destroy the comparison the burst
    // exists to support (AnimationShotsHandler.cpp:23-29). Taking the setter as an argument also
    // makes the union assertable against a fixture the TEST moves rather than Sequencer, which is
    // what lets its test run with no sequence, no viewport and no GPU.
    //
    // Returns false only when the setter refuses an instant; the setter's own error pair is passed
    // through untouched.
    bool UnionActorBoundsAcrossInstants(AActor& Actor, const TArray<double>& InstantSeconds,
        const FSubjectTimeSetter& TimeSetter, FSampledBoundsUnion& Out,
        FString& OutErrCode, FString& OutErrMsg);

    // The level's own bounds: the ALevelBounds actor when the persistent level has one, else the
    // union of every actor's component bounding box. Same two-step rule (and the same shared
    // McpActorUtils::SumActorBounds) as level.get_bounds, so the two cannot drift into different
    // ideas of how big the level is. Returns false when nothing in the world has finite renderable
    // bounds -- an empty level frames nothing, and that is reported rather than guessed at.
    bool ResolveLevelBounds(UWorld* World, FVector& OutOrigin, double& OutRadius);

    // ---- what the resolve did to the editor ----

    // Side effects FResolvedSubject has no field for. A verb that changed the editor's transport
    // state has to be able to say so (rpc-design.md: report what happened).
    struct FLevelSubjectReport
    {
        // This resolve paused a PLAYING sequence. A playing sequence advances between captures, so
        // a burst taken over one is not a set of comparable instants.
        bool bPausedPlayback = false;
        // This resolve opened the sequence in Sequencer; it was not the one already open.
        bool bSequenceOpened = false;
        // Populated only when the time plan carried instants and the Actor kind unioned them.
        FSampledBoundsUnion SampledBounds;
    };

    // ---- resolve ----

    // Fill the bounds, capture source, time window and time setter of a World or Actor subject
    // WITHOUT acquiring a viewport. Everything a caller can be refused for except "there is no
    // live Level Editor viewport" is decided here.
    //
    // OutTimeSetter is ALWAYS filled, per the resolver's contract: when the subject has no time
    // axis the setter refuses by name, so a caller that never asks for a time never sees the
    // refusal.
    //
    // RELEASE. A resolve that opened Sequencer stores its restore state in
    // FResolvedSubject::ProviderState, which is where the resolver requires per-call state to live
    // -- FSubjectProvider::Release is registered once at static init and can capture nothing. The
    // release therefore rides the FResolvedSubject destructor like every other kind's, and a
    // failed resolve leaves no state behind at all.
    bool ResolveLevelSubjectBounds(const FSubjectRequest& Request, const FLevelTimePlan& TimePlan,
        FResolvedSubject& OutSubject, FSubjectTimeSetter& OutTimeSetter,
        FLevelSubjectReport& OutReport, FString& OutErrCode, FString& OutErrMsg);

    // ResolveLevelSubjectBounds plus the live Level Editor viewport, in that order: the viewport is
    // acquired FIRST so a headless run fails without having opened a sequence or moved a playhead,
    // matching what camera.animation_shots already does before it touches Sequencer.
    bool AcquireLevelSubject(const FSubjectRequest& Request, const FLevelTimePlan& TimePlan,
        FResolvedSubject& OutSubject, FSubjectTimeSetter& OutTimeSetter,
        FLevelSubjectReport& OutReport, FString& OutErrCode, FString& OutErrMsg);

    // Undo what a successful resolve took: put the playhead back where it was found, when the plan
    // asked for that. Idempotent -- it consumes the state it acts on -- and a no-op on a subject
    // that took nothing.
    //
    // PRECONDITION: the subject was resolved by one of the two functions above. It is the level
    // providers' registered Release, so the resolver only ever calls it on subjects they acquired;
    // a caller driving the explicit API is responsible for the same pairing.
    void ReleaseLevelSubject(FResolvedSubject& Subject);
}
