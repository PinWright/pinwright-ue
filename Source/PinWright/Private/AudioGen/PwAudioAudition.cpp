// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "AudioGen/PwAudioAudition.h"

#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwAudioExport.h"

#include "Components/AudioComponent.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundWave.h"

bool PwIsAuditionSlotObservable(FString& OutReason)
{
    if (!GEditor)
    {
        OutReason = TEXT("GEditor is null (commandlet or an -unattended boot with no editor engine), ")
                    TEXT("so the editor preview audio slot does not exist in this process.");
        return false;
    }
    return true;
}

bool PwIsAuditionAvailable(FString& OutReason)
{
    if (!PwIsAuditionSlotObservable(OutReason))
    {
        return false;
    }

    // UEngine::UseSound() is `AudioDeviceManager != nullptr` (UnrealEngine.cpp:4262).
    // UEditorEngine::ResetPreviewAudioComponent returns nullptr on !UseSound()
    // (EditorEngine.cpp:2891) before it creates or plays anything, so PlayPreviewSound
    // would come back null. Say that up front instead of reporting a generic failure.
    if (!GEditor->UseSound())
    {
        OutReason = TEXT("The engine has no audio device manager (UEngine::UseSound() is false - ")
                    TEXT("e.g. launched with -nosound, or a host with no audio output), so ")
                    TEXT("UEditorEngine::PlayPreviewSound cannot start anything.");
        return false;
    }
    return true;
}

FPwAuditionPreviewState PwCaptureAuditionPreviewState()
{
    FPwAuditionPreviewState State;
    if (!GEditor)
    {
        return State;  // bMeasured stays false: nothing was observed.
    }

    State.bMeasured = true;
    if (const UAudioComponent* Component = GEditor->GetPreviewAudioComponent())
    {
        State.bComponentPresent = true;
        State.bPlaying = Component->IsPlaying();
        if (const USoundBase* Sound = Component->Sound)
        {
            State.SoundPath = Sound->GetPathName();
        }
    }
    return State;
}

UAudioComponent* PwGetAuditionComponent()
{
    return GEditor ? GEditor->GetPreviewAudioComponent() : nullptr;
}

void PwStopAudition()
{
    if (!GEditor)
    {
        return;
    }

    // Stop the live component directly BEFORE clearing the slot.
    // ResetPreviewAudioComponent() early-returns on !UseSound() (EditorEngine.cpp:2891),
    // which is ahead of its own PreviewAudioComponent->Stop() at :2899 - so on a host
    // whose audio device manager went away, the documented stop idiom alone would leave
    // a component the editor still considers active.
    if (UAudioComponent* Existing = GEditor->GetPreviewAudioComponent())
    {
        Existing->Stop();
    }

    // Then the documented idiom: clears PreviewSoundCue and re-seats the slot with a
    // bare, soundless component (EditorEngine.cpp:2882-2914), releasing the last strong
    // reference this module holds to a transient audition wave.
    GEditor->ResetPreviewAudioComponent();
}

bool PwAuditionSoundBase(USoundBase* Sound, FString& OutError)
{
    if (!Sound)
    {
        OutError = TEXT("No sound to audition (null USoundBase).");
        return false;
    }
    if (!PwIsAuditionAvailable(OutError))
    {
        return false;
    }

    // Explicitly stop first rather than relying on ResetPreviewAudioComponent's internal
    // Stop(): that one sits behind the !UseSound() early-out, and the §11 contract here
    // is "never leave a displaced preview running", not "usually".
    PwStopAudition();

    UAudioComponent* Component = GEditor->PlayPreviewSound(Sound);
    if (!Component)
    {
        // The only nullptr route is ResetPreviewAudioComponent's !UseSound() early-out,
        // which PwIsAuditionAvailable already screened for - so reaching here means the
        // audio engine went away between the two calls. Leave nothing half-started.
        PwStopAudition();
        OutError = FString::Printf(
            TEXT("UEditorEngine::PlayPreviewSound returned no audio component for '%s'; ")
            TEXT("the editor audio engine refused the preview."),
            *Sound->GetPathName());
        return false;
    }

    return true;
}

bool PwAuditionBuffer(const FPwAudioBuffer& In, FString& OutError)
{
    if (In.NumFrames() <= 0)
    {
        OutError = TEXT("Audio buffer is empty (0 frames); there is nothing to audition.");
        return false;
    }

    USoundWave* Wave = PwCreateTransientSoundWave(In);
    if (!Wave)
    {
        OutError = FString::Printf(
            TEXT("PwCreateTransientSoundWave produced no USoundWave for a %d-frame / %d Hz buffer."),
            In.NumFrames(), In.SampleRate);
        return false;
    }

    // The wave is transient and unrooted; it stays alive because
    // UAudioComponent::Sound is a UPROPERTY on the editor's rooted
    // PreviewAudioComponent. It becomes collectable again the moment the slot is
    // stopped or displaced, which is the lifetime we want.
    return PwAuditionSoundBase(Wave, OutError);
}

FPwAuditionCandidateResolver& PwAuditionCandidateResolver()
{
    // Function-local static: one instance across every TU under Unity builds.
    static FPwAuditionCandidateResolver GResolver;
    return GResolver;
}
