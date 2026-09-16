// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwAudioAudition - play a generated buffer or a project sound through the
// EDITOR PREVIEW audio device, so a human can actually hear it. This is the
// acceptance gate the rest of the audio-gen chunks are a proxy for.
//
// WHY UEditorEngine::PlayPreviewSound AND NOT UGameplayStatics::PlaySound2D.
// The existing audio.play_sound_2d verb uses PlaySound2D, which hard-requires a
// valid UWorld (AudioHandler.cpp errors with NO_WORLD when there is none) and
// resolves its sound as a project asset. PlayPreviewSound
// (EditorEngine.h:1316, impl EditorEngine.cpp:2965) has neither constraint:
//   - it takes a bare USoundBase* and performs NO package, asset-registry or
//     path lookup, so a TRANSIENT, never-saved USoundWave auditions fine - which
//     is exactly the shape PwCreateTransientSoundWave produces;
//   - it needs no PIE session and no editor world at all;
//   - it lives in UnrealEd, which this module already links, so there is no
//     engine-plugin gate and no integration sub-module to route through.
// It is a two-liner over ResetPreviewAudioComponent: install the sound in the
// editor's preview component, Play() it, broadcast OnSoundBasePreview, and
// return the component - or return nullptr when the audio engine is disabled
// (ResetPreviewAudioComponent early-outs on !UseSound(), EditorEngine.cpp:2891).
// That nullptr is a real failure and every caller here treats it as one.
//
// THE SHARED-STATE CONTRACT (docs/rpc-design.md §11). GEditor keeps exactly ONE
// preview audio component - UPROPERTY(transient) PreviewAudioComponent,
// EditorEngine.h:431 - and ResetPreviewAudioComponent stops whatever occupies it
// before installing a new one (EditorEngine.cpp:2897-2900). Auditioning
// therefore DISPLACES every other editor preview: a Sound Cue editor preview, a
// content-browser scrub, a previous audition by this same verb. The mutation is
// the point (the sound must keep playing after the RPC returns), so nothing here
// self-restores. Instead:
//   - PwCaptureAuditionPreviewState() snapshots the slot BEFORE it is taken, so
//     a caller can report what it displaced;
//   - PwStopAudition() is the documented undo, and is idempotent.
// "Does not restore" and "cannot be restored" are different contracts; this
// module ships the first.
//
// GEditor may be null (commandlet, -unattended automation without an editor
// engine). Every entry point below checks for that and fails with a message
// rather than crashing.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UAudioComponent;
class USoundBase;
struct FPwAudioBuffer;

// The editor preview slot as it was at one instant. Every field defaults to the
// failure/empty reading, so a snapshot that was never taken cannot be mistaken
// for "nothing was playing" (rpc-design.md §2: make the zero value a failure).
struct FPwAuditionPreviewState
{
    // False when the slot could not be read at all (no GEditor). Callers must
    // not report the other fields as measurements when this is false.
    bool bMeasured = false;

    // A preview audio component existed. Note this is routinely true and idle:
    // ResetPreviewAudioComponent() leaves a bare, soundless component behind.
    bool bComponentPresent = false;

    // UAudioComponent::IsPlaying() at snapshot time - the only field that says
    // something audible was actually going on.
    bool bPlaying = false;

    // Full object path of the sound occupying the slot, empty when none.
    FString SoundPath;
};

// True when the preview slot can be observed at all, i.e. GEditor exists.
// Stopping only needs this much; starting playback needs PwIsAuditionAvailable.
bool PwIsAuditionSlotObservable(FString& OutReason);

// True when a sound can actually be started: the slot is observable AND the
// engine has an audio device manager (UEngine::UseSound(), UnrealEngine.cpp:4262).
// Without one, PlayPreviewSound returns nullptr, so this is the difference
// between "will fail" and "may play".
bool PwIsAuditionAvailable(FString& OutReason);

// Read the shared preview slot. Safe with a null GEditor (returns bMeasured=false).
FPwAuditionPreviewState PwCaptureAuditionPreviewState();

// The editor's live preview audio component, or nullptr. Exposed so a caller can
// derive "is it playing" from the engine's own object rather than from a readback
// of whatever this module just returned (rpc-design.md §4).
UAudioComponent* PwGetAuditionComponent();

// Start auditioning Sound through the editor preview device, displacing whatever
// held the slot. Returns true only when PlayPreviewSound handed back a real
// UAudioComponent; on false OutError says why and the slot is left stopped.
bool PwAuditionSoundBase(USoundBase* Sound, FString& OutError);

// Wrap a generated buffer in a transient USoundWave (PwCreateTransientSoundWave)
// and audition that. An empty buffer is rejected before anything is allocated.
bool PwAuditionBuffer(const FPwAudioBuffer& In, FString& OutError);

// Stop the editor preview and clear the shared slot. Idempotent, and a no-op
// with a null GEditor.
void PwStopAudition();

// ---------------------------------------------------------------------------
// Candidate-registry seam
// ---------------------------------------------------------------------------

// SEAM ONLY - nothing in this file, or in the audition handler, includes or links
// the candidate registry. `audio.synth.audition` takes a candidateId the moment
// somebody binds this hook (AudioGen/PwCandidateRegistry.h owns the store, held
// by FPluginState); until then the hook is unbound and the verb REJECTS
// candidateId rather than guessing at a sound (rpc-design.md §3).
//
// The resolver hands back a BUFFER, not an asset: a synth candidate is generated
// audio with no package behind it, which is exactly the case PlayPreviewSound
// handles and PlaySound2D does not.
//
// OutErrorCode and OutErrorData both exist because a miss is not one outcome (§7).
// The registry distinguishes evicted (the id was real, the budget reclaimed it -
// re-render) from never-generated (generate something first) from unknown-id (the
// id is wrong), and those three remedies diverge, so each has its own top-level
// error code. The CODE is the first thing an agent branches on: an audition that
// flattened all three into one told an agent its id was wrong when its candidate
// had actually been evicted, which is `rpc-design.md` §5b - one door into the
// state reporting it correctly and another not.
//
// So the resolver, not the audition handler, owns the code. This file still knows
// nothing about FPwCandidateRegistry; the resolver's binder does, and passes
// FPwCandidateRegistry::MissErrorCode(...) straight through. Leave OutErrorCode
// empty and the handler falls back to ASSET_NOT_FOUND.
//
// OutErrorData carries the same miss as structure (BuildMissPayload: status,
// removal record, live budget) because prose can be dropped by the oversize-spill
// rewrite. Leave it null for a plain miss.
DECLARE_DELEGATE_RetVal_FiveParams(bool, FPwAuditionCandidateResolver,
    const FString& /*CandidateId*/, FPwAudioBuffer& /*OutBuffer*/,
    FString& /*OutErrorCode*/, FString& /*OutError*/,
    TSharedPtr<FJsonObject>& /*OutErrorData*/);

FPwAuditionCandidateResolver& PwAuditionCandidateResolver();
