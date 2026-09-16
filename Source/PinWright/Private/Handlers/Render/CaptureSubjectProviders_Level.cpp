// Copyright (c) 2026 Alexander Penkin. MIT License.

// The World and Actor capture-subject providers.
//
// These are the two kinds whose pixels come from the LIVE Level Editor viewport. Everything an
// asset-kind provider spends its acquire on -- opening a toolkit, walking a Slate tree for an
// SEditorViewport, closing the window again -- has no counterpart here: the level viewport is
// already open or the call fails, and nothing is closed afterwards.
//
// WHAT IS ACTUALLY SHARED WITH THE ASSET KINDS, and is the whole reason these are providers rather
// than two more copies of the same twenty lines:
//
//   * BOUNDS ARE A RESOLVED PROPERTY, NOT A VERB'S PRIVATE ARITHMETIC. camera.frame_actor,
//     camera.orbit_shots and camera.animation_shots each compute an origin and a radius, and each
//     one does it differently: two read GetActorBounds once, the third unions it across every
//     sampled instant because a posed skeletal mesh changes shape every frame. Both are correct;
//     WHICH one applies is a property of the subject, so it belongs here.
//   * TIME IS A SETTER, NOT A LOOP. A placed actor has no time axis of its own -- a bound skeletal
//     mesh only evaluates in the editor while Sequencer drives it, so a plain SkeletalMeshActor
//     does not animate in the viewport at all (AnimationShotsHandler.cpp:11-18). The time axis for
//     these two kinds is therefore a Level Sequence scrub, handed back as one callable so the
//     capture primitive can drive it per pose instead of every verb re-issuing SetGlobalPosition
//     plus ForceUpdate and one of them forgetting the ForceUpdate.
//
// WHAT THIS FILE DELIBERATELY DOES NOT DO. It does not resolve a camera distance. FResolvedSubject
// carries an origin and a radius and nothing else, because the distance depends on the projection
// mode, the field of view and the padding -- all of which are the verb's arguments, not the
// subject's. camera.orbit_shots' `radius` argument therefore still overrides the fit distance in
// the verb; the only place it reaches a subject is the bare-point case, where the point has no size
// and the orbit radius doubles as the framing extent.

#include "Handlers/Render/CaptureSubjectProviders_Level.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Render/CameraShotPlanUtils.h"
#include "Handlers/Sequencer/SequencePlayheadUtils.h"
#include "Utils/ActorUtils.h"
#include "Utils/AssetUtils.h"

#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/Level.h"
#include "Engine/LevelBounds.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "LevelSequence.h"
#include "Misc/FrameRate.h"
#include "Misc/FrameTime.h"
#include "MovieScene.h"
#include "Slate/SceneViewport.h"

namespace PinWrightCaptureSubjectLevel
{
    // File-unique NAMED namespace rather than an anonymous one: Unity merges these translation
    // units, and every sibling in Handlers/Render already carries helpers with these shapes.
    namespace Internal
    {
        // ---- per-call release state ----
        //
        // FSubjectProvider::Release is a closure REGISTERED ONCE at static init, so it can capture
        // nothing a particular resolve produced. FResolvedSubject::ProviderState is where the
        // resolver requires that state to live, and this is the level kinds' entry in it: the
        // playhead position the resolve found, and whether it was asked to put it back.
        struct FLevelReleaseState : public PinWrightCaptureSubject::FSubjectReleaseState
        {
            bool bRestorePlayhead = false;
            FFrameTime EntryPlayhead;
        };

        // The refusal a world/actor subject gives when a caller asks for a time it has no axis for.
        // Same code and the same reasoning as camera.animation_shots' own missing-sequence
        // rejection: the absence is an argument the caller did not supply, not an editor that
        // cannot serve the kind.
        FSubjectTimeSetter MakeNoTimeAxisSetter(ESubjectKind Kind)
        {
            const FString KindName = (Kind == ESubjectKind::Actor) ? TEXT("actor") : TEXT("world");
            return [KindName](double /*TimeSeconds*/, FString& OutErrCode, FString& OutErrMsg) -> bool
            {
                OutErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
                OutErrMsg = FString::Printf(
                    TEXT("This %s subject has no time axis: a placed actor does not evaluate in the editor on ")
                    TEXT("its own, so a time can only be set by scrubbing a Level Sequence. Pass sequencePath ")
                    TEXT("to give the subject a time axis."),
                    *KindName);
                return false;
            };
        }

        // Bounding-sphere radius of a box, the convention every capture verb in this cluster
        // already uses for `radius`: the half-diagonal, floored at 1 so a zero-extent actor still
        // yields a framable camera.
        double BoundsRadiusFromBox(const FBox& Box)
        {
            return FMath::Max(Box.GetExtent().Size(), 1.0);
        }
    }

    bool UnionActorBoundsAcrossInstants(AActor& Actor, const TArray<double>& InstantSeconds,
        const FSubjectTimeSetter& TimeSetter, FSampledBoundsUnion& Out,
        FString& OutErrCode, FString& OutErrMsg)
    {
        Out = FSampledBoundsUnion();
        if (InstantSeconds.Num() == 0)
        {
            return true;
        }
        if (!TimeSetter)
        {
            OutErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            OutErrMsg = TEXT("A sampled bounds union needs a time setter, and none was supplied");
            return false;
        }

        TArray<FVector> SampledOrigins;
        SampledOrigins.Reserve(InstantSeconds.Num());

        for (const double Seconds : InstantSeconds)
        {
            if (!TimeSetter(Seconds, OutErrCode, OutErrMsg))
            {
                return false;
            }

            FVector Origin = FVector::ZeroVector;
            FVector Extent = FVector::ZeroVector;
            Actor.GetActorBounds(false, Origin, Extent);
            const FBox InstantBox = FBox::BuildAABB(Origin, Extent);
            if (!InstantBox.IsValid)
            {
                // An instant with no renderable bounds contributes nothing rather than dragging the
                // union to the origin -- the same rule McpActorUtils::SumActorBounds keeps.
                continue;
            }

            Out.Union += InstantBox;
            Out.MaxSingleInstantRadius = FMath::Max(Out.MaxSingleInstantRadius, Extent.Size());
            for (const FVector& Previous : SampledOrigins)
            {
                Out.MaxOriginSeparation =
                    FMath::Max(Out.MaxOriginSeparation, FVector::Dist(Previous, Origin));
            }
            SampledOrigins.Add(Origin);
            ++Out.InstantsSampled;
        }
        return true;
    }

    bool ResolveLevelBounds(UWorld* World, FVector& OutOrigin, double& OutRadius)
    {
        OutOrigin = FVector::ZeroVector;
        OutRadius = 0.0;
        if (!World)
        {
            return false;
        }

        // Prefer the level's own ALevelBounds actor, then fall back to enclosing every actor -- the
        // same order level.get_bounds uses. Most levels have no ALevelBounds actor, so the fallback
        // is the common path.
        FBox Bounds(ForceInit);
        if (ULevel* PersistentLevel = World->PersistentLevel)
        {
            if (PersistentLevel->LevelBoundsActor.IsValid())
            {
                Bounds = PersistentLevel->LevelBoundsActor->GetComponentsBoundingBox();
            }
        }
        if (!Bounds.IsValid)
        {
            // World-scoped, not level-scoped: a capture of the level viewport shows every loaded
            // sublevel, so framing it from the persistent level alone would under-frame a
            // streamed-in world.
            Bounds = McpActorUtils::SumActorBounds(World);
        }
        if (!Bounds.IsValid)
        {
            return false;
        }

        OutOrigin = Bounds.GetCenter();
        OutRadius = Internal::BoundsRadiusFromBox(Bounds);
        return true;
    }

    bool ResolveLevelSubjectBounds(const FSubjectRequest& Request, const FLevelTimePlan& TimePlan,
        FResolvedSubject& OutSubject, FSubjectTimeSetter& OutTimeSetter,
        FLevelSubjectReport& OutReport, FString& OutErrCode, FString& OutErrMsg)
    {
        OutReport = FLevelSubjectReport();

        if (Request.Kind != ESubjectKind::World && Request.Kind != ESubjectKind::Actor)
        {
            OutErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            OutErrMsg = TEXT("The level subject providers serve only the 'world' and 'actor' kinds");
            return false;
        }

        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World)
        {
            OutErrCode = ErrorCodes::ERR_NO_EDITOR_WORLD;
            OutErrMsg = TEXT("No active editor world");
            return false;
        }

        OutSubject.Kind = Request.Kind;
        OutSubject.CaptureSource = CaptureSourceLevelViewport;

        // ---- 1. Everything that can be refused from the request alone, BEFORE anything in the
        // editor is opened or moved. A malformed subject must not leave a sequence opened on its
        // way to being rejected. ----

        AActor* Actor = nullptr;
        // ParseSubject sets bPointProvided, which is the authority. The non-zero fallback is for a
        // request assembled in code rather than parsed from the wire (a verb normalising its own
        // legacy arguments, or a test); it covers every point except one at exactly the world
        // origin, and that one falls through to level bounds instead of refusing. `radius` alone is
        // deliberately NOT taken as evidence of a point: on a world subject with no point it is
        // simply an orbit distance, and reading it as a point would refuse a legal request.
        const bool bPointProvided = Request.bPointProvided || !Request.Point.IsZero();

        if (Request.Kind == ESubjectKind::Actor)
        {
            if (Request.ActorName.IsEmpty())
            {
                OutErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
                OutErrMsg = TEXT("Provide a target: actorName (or objectPath/actorPath) or point {x, y, z}");
                return false;
            }
            // FindActorByName, not ResolveActor: this reproduces exactly what camera.frame_actor,
            // camera.orbit_shots and camera.animation_shots resolve with today, so routing them
            // through the resolver cannot change which actor a name picks out or which code an
            // unresolvable name returns.
            Actor = McpActorUtils::FindActorByName(nullptr, Request.ActorName);
            if (!Actor)
            {
                OutErrCode = ErrorCodes::ERR_ACTOR_NOT_FOUND;
                OutErrMsg = FString::Printf(TEXT("Actor not found: %s"), *Request.ActorName);
                return false;
            }
            OutSubject.ActorName = Request.ActorName;
        }
        else if (bPointProvided && Request.Radius <= 0.0f)
        {
            OutErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            OutErrMsg = PointWithoutRadiusMessage();
            return false;
        }

        // ---- 2. The time source. This is the first step that touches the editor: it opens the
        // sequence in Sequencer, pauses a playing one, and records the playhead to restore. ----

        if (TimePlan.HasTimeSource())
        {
            ULevelSequence* LevelSeq =
                Cast<ULevelSequence>(
                    ResolveAsset(TimePlan.SequencePath, /*bLoadObject=*/true).Object);
            if (!LevelSeq)
            {
                OutErrCode = ErrorCodes::ERR_SEQUENCE_NOT_FOUND;
                OutErrMsg = FString::Printf(TEXT("Level sequence not found at '%s'"),
                    *TimePlan.SequencePath);
                return false;
            }
            UMovieScene* MovieScene = LevelSeq->GetMovieScene();
            if (!MovieScene)
            {
                OutErrCode = ErrorCodes::ERR_SEQUENCE_INVALID;
                OutErrMsg = FString::Printf(TEXT("Level sequence '%s' has no MovieScene"),
                    *TimePlan.SequencePath);
                return false;
            }

            bool bOpened = false;
            if (!SequencePlayheadUtils::EnsureSequenceOpenInSequencer(
                    LevelSeq, TimePlan.bOpenSequenceIfNeeded, bOpened, OutErrCode, OutErrMsg))
            {
                return false;
            }
            OutReport.bSequenceOpened = bOpened;
            // The one asset-editor fact this kind can honestly report: Sequencer already had the
            // sequence open. Nothing here ever CLOSES it, so bEditorClosed stays false -- closing a
            // Sequencer window the caller opened would be a surprise, not a cleanup.
            OutSubject.bEditorWasAlreadyOpen = !bOpened;

            // A playing sequence advances between captures, so the frames drift by however long
            // each capture took and stop being comparable instants.
            OutReport.bPausedPlayback = SequencePlayheadUtils::PausePlaybackIfPlaying();

            // Recorded BEFORE the first scrub, and released through ProviderState so the restore
            // rides the FResolvedSubject destructor on every exit path, error paths included.
            TSharedRef<Internal::FLevelReleaseState> ReleaseState =
                MakeShared<Internal::FLevelReleaseState>();
            ReleaseState->bRestorePlayhead = TimePlan.bRestorePlayheadOnRelease;
            ReleaseState->EntryPlayhead = SequencePlayheadUtils::GetPlayheadPosition();
            OutSubject.ProviderState = ReleaseState;

            const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
            FFrameTime RangeStart(0);
            FFrameTime RangeEnd(0);
            if (SequencePlayheadUtils::GetPlaybackRangeInDisplayFrames(*MovieScene, RangeStart, RangeEnd))
            {
                OutSubject.TimeStartSeconds = DisplayRate.AsSeconds(RangeStart);
                OutSubject.TimeEndSeconds = DisplayRate.AsSeconds(RangeEnd);
            }

            const EUpdatePositionMethod UpdateMethod = TimePlan.UpdateMethod;
            const FString CanonicalMethod = TimePlan.CanonicalMethod;
            const bool bForceUpdate = TimePlan.bForceUpdate;
            OutTimeSetter = [DisplayRate, UpdateMethod, CanonicalMethod, bForceUpdate](
                double TimeSeconds, FString& SetterErrCode, FString& SetterErrMsg) -> bool
            {
                if (!FMath::IsFinite(TimeSeconds))
                {
                    SetterErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
                    SetterErrMsg = TEXT("Subject time must be a finite number of seconds");
                    return false;
                }
                // Seconds in, WHOLE DISPLAY FRAMES out. A sub-frame remainder accumulates through
                // the display-rate/tick-resolution conversion and walks a burst off the sequence's
                // own grid, so every seconds-shaped input is rounded onto that grid here -- the same
                // rule camera.animation_shots applies to intervalSeconds
                // (AnimationShotsHandler.cpp:19-22).
                const FFrameTime DisplayTime(DisplayRate.AsFrameTime(TimeSeconds).RoundToFrame());
                SequencePlayheadUtils::ApplyPlayheadPosition(
                    DisplayTime, UpdateMethod, CanonicalMethod, bForceUpdate);
                return true;
            };
            OutSubject.bTimeSupported = true;
            // A Level Sequence scrub is a deterministic evaluation of authored data: the same frame
            // yields the same pose on every run. This is the property a Niagara subject does NOT
            // have, and the reason the flag exists at all.
            OutSubject.bTimeReproducible = true;
        }
        else
        {
            OutTimeSetter = Internal::MakeNoTimeAxisSetter(Request.Kind);
            OutSubject.bTimeSupported = false;
            OutSubject.bTimeReproducible = false;
        }

        // Undo step 2 on any failure below it, so a refused resolve leaves the editor exactly as it
        // found it rather than relying on the caller to notice.
        const auto FailAfterTimeSource = [&OutSubject]()
        {
            ReleaseLevelSubject(OutSubject);
            OutSubject.bTimeSupported = false;
        };

        // ---- 3. Bounds. ----

        if (Request.Kind == ESubjectKind::Actor)
        {
            check(Actor);
            const bool bWantsSampledUnion =
                OutSubject.bTimeSupported && TimePlan.InstantSeconds.Num() > 0;

            if (bWantsSampledUnion)
            {
                if (!UnionActorBoundsAcrossInstants(*Actor, TimePlan.InstantSeconds,
                        OutTimeSetter, OutReport.SampledBounds, OutErrCode, OutErrMsg))
                {
                    FailAfterTimeSource();
                    return false;
                }
                if (!OutReport.SampledBounds.Union.IsValid)
                {
                    OutErrCode = ErrorCodes::ERR_BOUNDS_EMPTY;
                    OutErrMsg = FString::Printf(
                        TEXT("Actor '%s' reported no bounds at any sampled instant, so no camera can be framed on it"),
                        *Request.ActorName);
                    FailAfterTimeSource();
                    return false;
                }
                OutSubject.BoundsOrigin = OutReport.SampledBounds.Union.GetCenter();
                OutSubject.BoundsRadius = Internal::BoundsRadiusFromBox(OutReport.SampledBounds.Union);
                OutSubject.BoundsSource = BoundsSourceSampledUnion;
                return true;
            }

            FVector Origin = FVector::ZeroVector;
            FVector Extent = FVector::ZeroVector;
            Actor->GetActorBounds(false, Origin, Extent);
            const FBox ActorBox = FBox::BuildAABB(Origin, Extent);
            if (!ActorBox.IsValid)
            {
                OutErrCode = ErrorCodes::ERR_BOUNDS_EMPTY;
                OutErrMsg = FString::Printf(
                    TEXT("Actor '%s' reported no bounds, so no camera can be framed on it"),
                    *Request.ActorName);
                FailAfterTimeSource();
                return false;
            }
            OutSubject.BoundsOrigin = Origin;
            OutSubject.BoundsRadius = Internal::BoundsRadiusFromBox(ActorBox);
            OutSubject.BoundsSource = BoundsSourceActorBounds;
            return true;
        }

        // World kind.
        if (bPointProvided)
        {
            // Verbatim the existing bare-point path: a point has no size, so the orbit radius
            // doubles as the framing extent. The refusal for a missing radius already fired in
            // step 1.
            OutSubject.BoundsOrigin = Request.Point;
            OutSubject.BoundsRadius = static_cast<double>(Request.Radius);
            OutSubject.BoundsSource = BoundsSourcePoint;
            return true;
        }

        FVector LevelOrigin = FVector::ZeroVector;
        double LevelRadius = 0.0;
        if (ResolveLevelBounds(World, LevelOrigin, LevelRadius))
        {
            OutSubject.BoundsOrigin = LevelOrigin;
            OutSubject.BoundsRadius = LevelRadius;
            OutSubject.BoundsSource = BoundsSourceLevelBounds;
            return true;
        }

        // A level where nothing has finite renderable bounds is still capturable -- the viewport
        // draws whatever is there -- so this is NOT a refusal. It leaves BoundsRadius at 0, which is
        // exactly the value EvaluateBoundsFraming reads as "no usable bounds" and reports as
        // bEvaluated=false. BoundsSource stays empty rather than claiming a source that produced
        // nothing.
        OutSubject.BoundsOrigin = FVector::ZeroVector;
        OutSubject.BoundsRadius = 0.0;
        return true;
    }

    bool AcquireLevelSubject(const FSubjectRequest& Request, const FLevelTimePlan& TimePlan,
        FResolvedSubject& OutSubject, FSubjectTimeSetter& OutTimeSetter,
        FLevelSubjectReport& OutReport, FString& OutErrCode, FString& OutErrMsg)
    {
        // Viewport first. A headless run must fail before a sequence is opened or a playhead is
        // moved, so the refusal costs the editor nothing.
        FEditorViewportClient* ViewportClient = nullptr;
        TSharedPtr<FSceneViewport> SceneViewport;
        if (!PinWrightCameraFrame::GetActiveLevelViewport(
                ViewportClient, SceneViewport, OutErrCode, OutErrMsg))
        {
            return false;
        }

        if (!ResolveLevelSubjectBounds(Request, TimePlan, OutSubject, OutTimeSetter,
                OutReport, OutErrCode, OutErrMsg))
        {
            return false;
        }

        // Assigned only on the success path, so the contract's "never null on success" holds in the
        // other direction too: a failed acquire leaves no half-usable viewport behind.
        OutSubject.ViewportClient = ViewportClient;
        OutSubject.SceneViewport = SceneViewport;
        return true;
    }

    void ReleaseLevelSubject(FResolvedSubject& Subject)
    {
        if (!Subject.ProviderState.IsValid())
        {
            return;
        }
        // Consumed, so a second release is a no-op without needing a separate flag. The cast is
        // safe by the pairing precondition in the header: this runs only on subjects the level
        // providers resolved, either as their registered Release or through the explicit API.
        const TSharedPtr<Internal::FLevelReleaseState> ReleaseState =
            StaticCastSharedPtr<Internal::FLevelReleaseState>(Subject.ProviderState);
        Subject.ProviderState.Reset();

        if (ReleaseState.IsValid() && ReleaseState->bRestorePlayhead)
        {
            // Jump, not scrub: putting the playhead back is not an animation step, and the forced
            // update is what makes the editor present the restored frame rather than the last one
            // the burst photographed.
            SequencePlayheadUtils::ApplyPlayheadPosition(
                ReleaseState->EntryPlayhead, EUpdatePositionMethod::Jump, TEXT("jump"),
                /*bForceUpdate=*/true);
        }
    }

    namespace Internal
    {
        // Static-init registration, one file per kind, exactly the shape the dispatcher's own
        // REGISTER_RPC_HANDLER uses. Adding a kind creates a file and edits none.
        struct FAutoRegisterLevelSubjectProviders
        {
            FAutoRegisterLevelSubjectProviders()
            {
                RegisterKind(ESubjectKind::World);
                RegisterKind(ESubjectKind::Actor);
            }

            static void RegisterKind(ESubjectKind Kind)
            {
                PinWrightCaptureSubject::FSubjectProvider Provider;
                Provider.Kind = Kind;
                // The generic registry path carries no time plan: `sequencePath` is a verb
                // argument, not part of the `subject` object, so a subject resolved through the
                // registry alone has no time axis and its setter says so. A verb that owns a
                // sequence calls AcquireLevelSubject directly with the plan filled in.
                Provider.Acquire = [](const FSubjectRequest& Request, FResolvedSubject& OutSubject,
                    FSubjectTimeSetter& OutTimeSetter, FString& OutErrCode, FString& OutErrMsg) -> bool
                {
                    const FLevelTimePlan TimePlan;
                    FLevelSubjectReport Report;
                    return AcquireLevelSubject(Request, TimePlan, OutSubject, OutTimeSetter,
                        Report, OutErrCode, OutErrMsg);
                };
                Provider.Release = [](FResolvedSubject& Subject)
                {
                    ReleaseLevelSubject(Subject);
                };
                PinWrightCaptureSubject::RegisterProvider(Provider);
            }
        };

        // `static`, matching REGISTER_RPC_HANDLER's own AutoReg object: the registration is a side
        // effect of construction, and the name is file-unique so Unity cannot merge two.
        static const FAutoRegisterLevelSubjectProviders GAutoRegisterLevelSubjectProviders;
    }
}
