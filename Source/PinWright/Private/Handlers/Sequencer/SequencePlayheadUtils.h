// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Niagara/NiagaraTickPreflight.h"
#include "LevelSequence.h"
#include "LevelSequenceEditorBlueprintLibrary.h"
#include "Compat/EngineVersionCompat.h"
#include "MovieScene.h"
// EUpdatePositionMethod / FMovieSceneSequencePlaybackParams. Included explicitly rather than
// inherited from LevelSequenceEditorBlueprintLibrary.h so this header survives the
// -StrictIncludes -DisableUnity packaging build.
#include "MovieSceneSequencePlayer.h"

// The ONE production implementation of "move the Sequencer playhead to an exact position and
// make the editor present that frame".
//
// sequencer.set_playhead owns the wire surface; every verb that steps an animation to capture
// it (camera.animation_shots) drives the same functions here instead of re-issuing
// SetGlobalPosition/ForceUpdate itself. A second copy of a scrub is how one of them keeps a
// stale engine-version branch, or forgets ForceUpdate and silently records an off-by-one frame
// across a whole burst — exactly the failure a duplicated key-interpolation parser produced in
// this same cluster before it was consolidated into SequencerKeyInterp.h.
namespace SequencePlayheadUtils
{
    // Canonicalize the wire spelling of the playhead update method. Returns false for an
    // unrecognized spelling so a handler can reject it by name rather than silently falling
    // back to a default the caller did not ask for.
    inline bool ParseUpdateMethod(const FString& In, EUpdatePositionMethod& OutMethod, FString& OutCanonical)
    {
        const FString Normalized = In.TrimStartAndEnd().ToLower();
        if (Normalized == TEXT("scrub"))
        {
            OutMethod = EUpdatePositionMethod::Scrub;
            OutCanonical = TEXT("scrub");
            return true;
        }
        if (Normalized == TEXT("jump"))
        {
            OutMethod = EUpdatePositionMethod::Jump;
            OutCanonical = TEXT("jump");
            return true;
        }
        if (Normalized == TEXT("play"))
        {
            OutMethod = EUpdatePositionMethod::Play;
            OutCanonical = TEXT("play");
            return true;
        }
        return false;
    }

    // Refuse to drive the playhead while the open level holds a Niagara system that asserts on its
    // next tick.
    //
    // Moving the playhead is not a read. LevelEditorSequencerIntegration hangs OnSequencerEvaluated
    // off OnGlobalTimeChanged and calls ReRenderLevelViewports() -> RequestRealTimeFrames(1),
    // whose engine-authored comment states the intent outright: "ensure that we tick the world".
    // That promotes the next frame to a full LEVELTICK_ViewportsOnly pass, which runs
    // FNiagaraWorldManagerTickFunction over every system simulation in the world. Opening the
    // sequence in the asset editor does the same thing on top, which is how a read-shaped scrub
    // came to be the verb that killed a shared editor four and a half minutes after an unrelated
    // agent's Niagara write armed the trap
    // (B-niagara-di-count-mismatch-vectorvm-assert-kills-editor).
    //
    // The refusal is one call; the alternative is every agent in the editor losing everything they
    // have not saved, with nothing in the crash naming the cause. Same error code the Niagara verbs
    // use for the same fault, so one fault keeps one name whichever verb finds it.
    inline bool RejectPlayheadOnTickUnsafeNiagara(FString& OutErrorCode, FString& OutErrorMessage)
    {
        TArray<PinWrightNiagara::FTickUnsafeNiagaraSystem> Unsafe;
        if (PinWrightNiagara::FindTickUnsafeNiagaraSystems(Unsafe) == 0)
        {
            return true;
        }
        OutErrorCode = ErrorCodes::ERR_NIAGARA_DATA_INTERFACE_MISMATCH;
        OutErrorMessage = FString::Printf(
            TEXT("Refusing to move the Sequencer playhead: the open level is running Niagara system(s) whose ")
            TEXT("compiled data-interface count differs from the resolved count — %s. Scrubbing forces a world ")
            TEXT("tick, and ticking one of these asserts inside the VectorVM on a worker thread, which is an ")
            TEXT("appError and kills the editor process for every session attached to it. Run niagara.compile on ")
            TEXT("the named system(s), or detach/deactivate the components holding them, then retry. ")
            TEXT("niagara.audit_level reports the same set with the components and owning actors named."),
            *PinWrightNiagara::DescribeTickUnsafeNiagaraSystems(Unsafe));
        return false;
    }

    // The ULevelSequenceEditorBlueprintLibrary playhead APIs address whichever sequence is open
    // in Sequencer, never an arbitrary asset — so the target must be the open one before any
    // position is written. bOutOpened reports whether this call did the opening.
    inline bool EnsureSequenceOpenInSequencer(ULevelSequence* Sequence, bool bOpenIfNeeded,
        bool& bOutOpened, FString& OutErrorCode, FString& OutErrorMessage)
    {
        bOutOpened = false;
        if (!Sequence)
        {
            OutErrorCode = ErrorCodes::ERR_SEQUENCE_NOT_FOUND;
            OutErrorMessage = TEXT("No level sequence supplied");
            return false;
        }
        // Before the open AND before the already-open early return: an evaluation of an
        // already-open sequence ticks the world just as an opening one does.
        if (!RejectPlayheadOnTickUnsafeNiagara(OutErrorCode, OutErrorMessage))
        {
            return false;
        }
        if (ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence() == Sequence)
        {
            return true;
        }
        if (!bOpenIfNeeded)
        {
            OutErrorCode = ErrorCodes::ERR_SEQUENCE_NOT_OPEN;
            OutErrorMessage = FString::Printf(
                TEXT("'%s' is not the sequence currently open in Sequencer, and open=false was requested"),
                *Sequence->GetPathName());
            return false;
        }
        if (!ULevelSequenceEditorBlueprintLibrary::OpenLevelSequence(Sequence))
        {
            OutErrorCode = ErrorCodes::ERR_EXECUTION_ERROR;
            OutErrorMessage = FString::Printf(TEXT("Failed to open '%s' in Sequencer"), *Sequence->GetPathName());
            return false;
        }
        bOutOpened = true;
        return true;
    }

    // Write the playhead position and (by default) force one immediate re-evaluation.
    // Returns the update method ACTUALLY applied, which is not always the one requested — see
    // the 5.3 branch. TargetDisplayTime is a DISPLAY-RATE FFrameTime, the unit SetGlobalPosition
    // consumes (its EMovieSceneTimeUnit default is DisplayRate); passing seconds here silently
    // reinterprets them as frames.
    inline FString ApplyPlayheadPosition(const FFrameTime& TargetDisplayTime,
        EUpdatePositionMethod UpdateMethod, const FString& CanonicalMethod, bool bForceUpdate)
    {
        FString AppliedMethod = CanonicalMethod;
#if UE_VERSION_OLDER_THAN(5, 4, 0)
        // The FMovieSceneSequencePlaybackParams-based SetGlobalPosition() was added in
        // UE 5.4. On 5.3 only SetCurrentTime(int32) exists: it always jumps and cannot
        // carry a sub-frame remainder, so the position is rounded to the nearest display
        // frame and the caller is told the method actually applied, not the one asked for.
        ULevelSequenceEditorBlueprintLibrary::SetCurrentTime(TargetDisplayTime.RoundToFrame().Value);
        AppliedMethod = TEXT("jump");
        (void)UpdateMethod;  // parsed and validated on every engine; only 5.4+ can honor it
#else
        FMovieSceneSequencePlaybackParams PlaybackParams;
        PlaybackParams.PositionType = EMovieScenePositionType::Frame;
        PlaybackParams.Frame = TargetDisplayTime;
        PlaybackParams.UpdateMethod = UpdateMethod;
        ULevelSequenceEditorBlueprintLibrary::SetGlobalPosition(PlaybackParams);
#endif

        // Scrubbing evaluates, but the editor can still present the previous frame until the
        // next tick — fatal for "set playhead then capture" bursts, which would silently record
        // an off-by-one frame on EVERY frame of the burst. One immediate refresh closes that.
        if (bForceUpdate)
        {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
            ULevelSequenceEditorBlueprintLibrary::ForceUpdate();
#else
            // ForceUpdate (NotifyMovieSceneDataChanged with RefreshAllImmediately) arrived in
            // UE 5.5. RefreshCurrentLevelSequence is the same notification on this engine, at
            // the Unknown change type, which is the strongest refresh 5.4 publishes.
            ULevelSequenceEditorBlueprintLibrary::RefreshCurrentLevelSequence();
#endif
        }
        return AppliedMethod;
    }

    // The playhead's current position as a DISPLAY-RATE frame time. Used to put the playhead
    // back where a burst found it, so stepping an animation to photograph it is not a mutation
    // of the editor's visible state.
    //
    // The int32 GetCurrentTime()/SetCurrentTime(int32) pair is deprecated from UE 5.4 in favour
    // of the FMovieSceneSequencePlaybackParams form, so calling the old one unconditionally
    // trips a deprecation diagnostic on 5.4+ while the new one does not exist on 5.3.
    inline FFrameTime GetPlayheadPosition()
    {
#if UE_VERSION_OLDER_THAN(5, 4, 0)
        return FFrameTime(ULevelSequenceEditorBlueprintLibrary::GetCurrentTime());
#else
        const FMovieSceneSequencePlaybackParams Params = ULevelSequenceEditorBlueprintLibrary::GetGlobalPosition();
        return Params.Frame;
#endif
    }

    // Stop playback before a burst steps the playhead by hand. A playing sequence advances
    // between captures, so the set stops being a set of comparable instants — the frames drift
    // by however long each capture took. Returns true when this call actually paused something,
    // so a verb can report that it changed the editor's transport state.
    inline bool PausePlaybackIfPlaying()
    {
        if (!ULevelSequenceEditorBlueprintLibrary::IsPlaying())
        {
            return false;
        }
        ULevelSequenceEditorBlueprintLibrary::Pause();
        return true;
    }

    // The sequence's playback range expressed as DISPLAY-RATE frame times. This is the window a
    // burst samples across when the caller does not name explicit frames. Returns false when the
    // range is empty or inverted, which no sensible sampling plan can be built from.
    inline bool GetPlaybackRangeInDisplayFrames(const UMovieScene& MovieScene,
        FFrameTime& OutStart, FFrameTime& OutEnd)
    {
        const FFrameRate DisplayRate = MovieScene.GetDisplayRate();
        const FFrameRate TickResolution = MovieScene.GetTickResolution();
        const TRange<FFrameNumber> Range = MovieScene.GetPlaybackRange();
        if (Range.IsEmpty() || !Range.HasLowerBound() || !Range.HasUpperBound())
        {
            return false;
        }
        OutStart = FFrameRate::TransformTime(FFrameTime(Range.GetLowerBoundValue()), TickResolution, DisplayRate);
        OutEnd = FFrameRate::TransformTime(FFrameTime(Range.GetUpperBoundValue()), TickResolution, DisplayRate);
        return OutEnd > OutStart;
    }
}
