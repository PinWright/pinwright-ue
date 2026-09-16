// Copyright (c) 2026 Alexander Penkin. MIT License.

// AudioSynthAuditionHandler.cpp - audio.synth.audition
//
// The verb that makes generated audio audible. Everything it does to the editor
// goes through AudioGen/PwAudioAudition.h; this file is the wire contract.
//
// Two design rules shape the response and are worth stating here because a later
// edit would otherwise quietly undo them:
//
// §11 (do not mutate global state without restoring it). The editor's preview
// audio component is ONE shared slot, so starting an audition takes it away from
// whatever else held it. Persisting past the call is the whole point, so the verb
// does not self-restore - it returns what the caller needs to undo it: a measured
// `previous` block, a measured `displacedPlayingPreview`, and `stopWith`, the
// literal method+args that stop it again.
//
// §1 (report only what happened). `started` is "PlayPreviewSound handed back a
// component", `playing` is UAudioComponent::IsPlaying() read back off the editor's
// own component afterwards. Neither is a literal, and they are deliberately NOT
// collapsed into one field: in the editor the audio thread runs on the game
// thread, so a very short sound can finish inside Play() and leave IsPlaying()
// false on a perfectly successful audition (see the comment at
// AudioComponent.cpp:926-928). Collapsing them would turn that into a fake failure.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"

#include "AudioGen/PwAudioAudition.h"
#include "AudioGen/PwAudioBuffer.h"

#include "Components/AudioComponent.h"
#include "Dom/JsonObject.h"
#include "Sound/SoundBase.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    // Deliberately NOT named ResolveSoundAsset: Handlers/Audio/AudioHandler.cpp has a
    // file-static of that name, and a Unity merge of the two TUs would be a redefinition
    // error. LOAD_Quiet|LOAD_NoWarn because an unresolvable path is a normal, reported
    // outcome of this verb, not something that should spray engine warnings into an
    // automation run.
    USoundBase* PwResolveAuditionSound(const FString& InPath)
    {
        FString Normalized = InPath;
        Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
        while (Normalized.EndsWith(TEXT("/")))
        {
            Normalized.LeftChopInline(1);
        }
        if (Normalized.IsEmpty())
        {
            return nullptr;
        }

        // In-memory first: a never-saved or transient sound has no on-disk package, and
        // that is exactly what the synth path produces.
        if (USoundBase* Found = Cast<USoundBase>(
                StaticFindObject(USoundBase::StaticClass(), nullptr, *Normalized)))
        {
            return Found;
        }
        return Cast<USoundBase>(StaticLoadObject(USoundBase::StaticClass(), nullptr,
            *Normalized, nullptr, LOAD_Quiet | LOAD_NoWarn));
    }

    // The §11 payload: what the shared slot held when this call started.
    TSharedPtr<FJsonObject> MakePreviousBlock(const FPwAuditionPreviewState& State)
    {
        TSharedPtr<FJsonObject> Previous = MakeShared<FJsonObject>();
        Previous->SetBoolField(TEXT("measured"), State.bMeasured);
        Previous->SetBoolField(TEXT("previewComponentPresent"), State.bComponentPresent);
        Previous->SetBoolField(TEXT("wasPlaying"), State.bPlaying);
        Previous->SetStringField(TEXT("soundPath"), State.SoundPath);
        return Previous;
    }

    // The undo pointer. A verb that persists a change owes the caller the way back.
    void AddStopWith(const TSharedPtr<FJsonObject>& Resp)
    {
        TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
        Args->SetBoolField(TEXT("stop"), true);

        TSharedPtr<FJsonObject> StopWith = MakeShared<FJsonObject>();
        StopWith->SetStringField(TEXT("method"), TEXT("audio.synth.audition"));
        StopWith->SetObjectField(TEXT("args"), Args);
        Resp->SetObjectField(TEXT("stopWith"), StopWith);
    }
}

REGISTER_RPC_HANDLER("audio.synth.audition", "audio.synth",
    "Play a sound out loud through the editor's preview audio device (no PIE session or editor world needed), or stop it. "
    "The preview slot is shared editor state: starting an audition displaces any other editor preview, so the response reports "
    "a measured `previous` block plus `stopWith`, the call that undoes it.",
    RPC_PARAMS(
        RPC_PARAM_OPT("assetPath", "path",
            "Object path of a USoundBase in the project (SoundWave, SoundCue, MetaSound source) to audition. "
            "Mutually exclusive with candidateId and stop."),
        RPC_PARAM_OPT("candidateId", "string",
            "Id of a generated audio candidate to audition. Requires the audio.synth candidate registry; "
            "rejected with ASSET_NOT_FOUND when no registry is wired into this build. "
            "Mutually exclusive with assetPath and stop."),
        RPC_PARAM_DEF("stop", "boolean",
            "Stop the editor preview and release the shared slot instead of starting playback. "
            "Mutually exclusive with assetPath and candidateId.", "false")
    ))
{
    const bool bStop = Ctx.GetBool(TEXT("stop"), false);
    const FString AssetPath = Ctx.GetString(TEXT("assetPath"));
    const FString CandidateId = Ctx.GetString(TEXT("candidateId"));

    // Nothing below can be reported honestly without an editor to look at.
    FString Unavailable;
    if (!PwIsAuditionSlotObservable(Unavailable))
    {
        Ctx.SendError(ErrorCodes::ERR_AUDITION_FAILED, Unavailable);
        return true;
    }

    // §3 - no unsafe default. "Neither source supplied" is an error, never a silent
    // no-op and never an implicit stop.
    const int32 SourceCount = (AssetPath.IsEmpty() ? 0 : 1) + (CandidateId.IsEmpty() ? 0 : 1)
        + (bStop ? 1 : 0);
    if (SourceCount == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_AUDITION_FAILED,
            TEXT("No audition source. Pass assetPath=<USoundBase object path>, candidateId=<synth candidate>, ")
            TEXT("or stop=true. There is no default sound to fall back to."));
        return true;
    }
    if (SourceCount > 1)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("assetPath, candidateId and stop are mutually exclusive; exactly one must be supplied."));
        return true;
    }

    // Snapshot the shared slot BEFORE anything touches it, so every response that goes
    // on to mutate it - success or failure - can say what it took over.
    const FPwAuditionPreviewState Before = PwCaptureAuditionPreviewState();

    // ---------------------------------------------------------------------
    // stop=true - undo form.
    // ---------------------------------------------------------------------
    if (bStop)
    {
        PwStopAudition();

        // Verdict comes from re-reading the editor's component, not from the fact that
        // PwStopAudition() was called (§4).
        const UAudioComponent* After = PwGetAuditionComponent();
        const bool bStillPlaying = (After != nullptr) && After->IsPlaying();

        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetBoolField(TEXT("stopped"), !bStillPlaying);
        Resp->SetBoolField(TEXT("playing"), bStillPlaying);
        Resp->SetBoolField(TEXT("displacedPlayingPreview"), Before.bPlaying);
        Resp->SetObjectField(TEXT("previous"), MakePreviousBlock(Before));
        AddStopWith(Resp);

        if (bStillPlaying)
        {
            Ctx.SendError(ErrorCodes::ERR_AUDITION_FAILED,
                TEXT("The editor preview component is still playing after being stopped."), Resp);
            return true;
        }
        Resp->SetBoolField(TEXT("success"), true);
        Ctx.SendSuccess(Resp);
        return true;
    }

    // ---------------------------------------------------------------------
    // Resolve the source. Both branches leave the shared slot untouched on failure,
    // which is why the rejections below carry no `previous` block: there is nothing to
    // undo. `previous` appears only on responses that actually took the slot - do not
    // "fix" that by emitting it everywhere, or it stops meaning anything.
    // ---------------------------------------------------------------------
    USoundBase* Sound = nullptr;
    FPwAudioBuffer CandidateBuffer;
    const bool bFromCandidate = !CandidateId.IsEmpty();

    if (bFromCandidate)
    {
        // SEAM: the candidate registry is a sibling chunk. When it is not wired, reject
        // rather than fall back to anything (§3). ASSET_NOT_FOUND rather than a new code
        // because the caller's remedy is the same either way: name a resolvable source.
        FPwAuditionCandidateResolver& Resolver = PwAuditionCandidateResolver();
        if (!Resolver.IsBound())
        {
            Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
                FString::Printf(
                    TEXT("No audio.synth candidate registry is wired into this build, so candidateId '%s' ")
                    TEXT("cannot be resolved. Pass assetPath instead."),
                    *CandidateId));
            return true;
        }

        FString ResolveErrorCode;
        FString ResolveError;
        TSharedPtr<FJsonObject> ResolveErrorData;
        if (!Resolver.Execute(CandidateId, CandidateBuffer, ResolveErrorCode, ResolveError,
                              ResolveErrorData))
        {
            // The resolver owns the CODE, not this handler. An evicted candidate, an empty
            // registry and an unknown id have divergent remedies and therefore divergent
            // codes; hardcoding one here told an agent its id was wrong when the candidate
            // had merely been evicted, leaving the truth only in the payload's `status` -
            // the same defect shape as §5b (one door reports it, another does not).
            // ASSET_NOT_FOUND stays as the fallback for a resolver that names no code.
            const FString EmittedCode = ResolveErrorCode.IsEmpty()
                ? FString(ErrorCodes::ERR_ASSET_NOT_FOUND)
                : ResolveErrorCode;

            // The structured miss (status / removal record / live budget) travels as data,
            // not prose - prose can be dropped by the oversize-spill rewrite (§7).
            Ctx.SendError(EmittedCode,
                FString::Printf(TEXT("Candidate '%s' could not be resolved: %s"),
                    *CandidateId, *ResolveError),
                ResolveErrorData);
            return true;
        }
        if (CandidateBuffer.NumFrames() <= 0)
        {
            Ctx.SendError(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
                FString::Printf(TEXT("Candidate '%s' resolved to a 0-frame buffer; there is nothing to hear."),
                    *CandidateId));
            return true;
        }
    }
    else
    {
        Sound = PwResolveAuditionSound(AssetPath);
        if (!Sound)
        {
            Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
                FString::Printf(TEXT("No USoundBase could be loaded from '%s'."), *AssetPath));
            return true;
        }
    }

    // ---------------------------------------------------------------------
    // Start playback. From here on the shared slot HAS been mutated.
    // ---------------------------------------------------------------------
    FString AuditionError;
    const bool bStarted = bFromCandidate
        ? PwAuditionBuffer(CandidateBuffer, AuditionError)
        : PwAuditionSoundBase(Sound, AuditionError);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("started"), bStarted);
    Resp->SetBoolField(TEXT("displacedPlayingPreview"), Before.bPlaying);
    Resp->SetObjectField(TEXT("previous"), MakePreviousBlock(Before));
    Resp->SetStringField(TEXT("source"), bFromCandidate ? TEXT("candidateId") : TEXT("assetPath"));
    if (bFromCandidate)
    {
        Resp->SetStringField(TEXT("candidateId"), CandidateId);
    }
    else
    {
        Resp->SetStringField(TEXT("assetPath"), AssetPath);
    }
    AddStopWith(Resp);

    if (!bStarted)
    {
        // PwAuditionSoundBase / PwAuditionBuffer already stopped the slot on their own
        // failure paths, so nothing is left half-started. Re-measure and say so.
        const UAudioComponent* AfterFailure = PwGetAuditionComponent();
        Resp->SetBoolField(TEXT("playing"),
            (AfterFailure != nullptr) && AfterFailure->IsPlaying());
        Ctx.SendError(ErrorCodes::ERR_AUDITION_FAILED, AuditionError, Resp);
        return true;
    }

    // Everything below is read back off the editor's own component - never echoed from
    // what was passed in. On the candidate path this is also how the transient wave that
    // PwAuditionBuffer created is identified at all.
    const UAudioComponent* Component = PwGetAuditionComponent();
    const USoundBase* Playing = Component ? Component->Sound : nullptr;
    const bool bIsPlaying = (Component != nullptr) && Component->IsPlaying();

    Resp->SetBoolField(TEXT("playing"), bIsPlaying);
    Resp->SetStringField(TEXT("soundPath"), Playing ? Playing->GetPathName() : FString());
    Resp->SetStringField(TEXT("soundClass"), Playing ? Playing->GetClass()->GetName() : FString());
    if (Playing)
    {
        // Duration tells the caller how long to wait before the sound is over. A looping
        // sound reports the engine's indefinite-loop sentinel, hence the paired flag.
        Resp->SetNumberField(TEXT("durationSeconds"), Playing->GetDuration());
        Resp->SetBoolField(TEXT("looping"), Playing->IsLooping());
    }

    if (!bIsPlaying)
    {
        // Not a failure: the editor runs the audio thread on the game thread, so a very
        // short sound can complete inside Play() (AudioComponent.cpp:926-928). Report the
        // gap rather than picking one of the two readings to believe.
        TArray<TSharedPtr<FJsonValue>> Warnings;
        Warnings.Add(MakeShared<FJsonValueString>(
            TEXT("The preview component was created but reports IsPlaying()==false. In the editor the ")
            TEXT("audio thread runs on the game thread, so a very short sound can finish before this ")
            TEXT("was measured; a silent or zero-length source reads the same way.")));
        Resp->SetArrayField(TEXT("warnings"), Warnings);
    }

    Resp->SetBoolField(TEXT("success"), true);
    Ctx.SendSuccess(Resp);
    return true;
}
