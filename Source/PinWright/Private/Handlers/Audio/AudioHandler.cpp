// Copyright (c) 2026 Alexander Penkin. MIT License.

// AudioHandler.cpp - Migrated from PinWright_AudioHandlers.cpp
// Audio asset creation, playback, sound mixes, dialogue, effects

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ParamAliasUtils.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Utils/AssetUtils.h"

#include "EngineUtils.h"
#include "Dom/JsonObject.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AudioDevice.h"
#include "Components/AudioComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundMix.h"
#include "Sound/SoundWave.h"

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------


static AActor* FindAudioActorByName(const FString& ActorName, UWorld* World)
{
    if (ActorName.IsEmpty())
        return nullptr;

    AActor* Actor = FindObject<AActor>(nullptr, *ActorName);
    if (Actor && Actor->IsValidLowLevel())
        return Actor;

    if (World)
    {
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            if (It->GetActorLabel().Equals(ActorName, ESearchCase::IgnoreCase) ||
                It->GetName().Equals(ActorName, ESearchCase::IgnoreCase))
            {
                return *It;
            }
        }
    }
    return nullptr;
}

static USoundBase* ResolveSoundAsset(const FString& SoundPath)
{
    if (SoundPath.IsEmpty())
        return nullptr;

    USoundBase* Sound = nullptr;

    // Prefer an already-loaded object addressed by its full object path. This
    // covers in-memory assets that have no on-disk package (never-saved /
    // transient sounds), and it must run BEFORE the resolver's load attempt:
    // when such an asset is registered in the AssetRegistry, a direct load
    // can try to stream a nonexistent package and emit a LogEditorAssetSubsystem
    // error. Mirrors the StaticLoadObject path
    // the sibling audio.authoring handlers use (AudioAuthoringHandler.cpp).
    if (SoundPath.Contains(TEXT("/")))
    {
        Sound = Cast<USoundBase>(StaticFindObject(USoundBase::StaticClass(), nullptr, *SoundPath));
        if (Sound)
            return Sound;
    }

    Sound = Cast<USoundBase>(ResolveAsset(SoundPath, /*bLoadObject=*/true).Object);

    if (Sound)
        return Sound;

    if (SoundPath.Contains(TEXT("/")))
    {
        UE_LOG(LogPinWrightSubsystem, Warning,
               TEXT("Sound asset '%s' not found (skipping recursive search)."), *SoundPath);
        return nullptr;
    }

    FString AssetName = FPaths::GetBaseFilename(SoundPath);
    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    TArray<FAssetData> AssetData;
    FARFilter Filter;
    Filter.ClassPaths.Add(USoundWave::StaticClass()->GetClassPathName());
    Filter.ClassPaths.Add(USoundCue::StaticClass()->GetClassPathName());
    Filter.bRecursivePaths = true;
    Filter.PackagePaths.Add(TEXT("/Game"));
    AssetRegistryModule.Get().GetAssets(Filter, AssetData);

    for (const FAssetData& Data : AssetData)
    {
        if (Data.AssetName.ToString().Equals(AssetName, ESearchCase::IgnoreCase))
        {
            Sound = Cast<USoundBase>(Data.GetAsset());
            if (Sound)
            {
                UE_LOG(LogPinWrightSubsystem, Log,
                       TEXT("Resolved sound '%s' to '%s'"), *SoundPath, *Sound->GetPathName());
                return Sound;
            }
        }
    }

    UE_LOG(LogPinWrightSubsystem, Warning,
           TEXT("Sound asset '%s' not found."), *SoundPath);
    return nullptr;
}

static USoundMix* ResolveSoundMix(const FString& MixPath)
{
    if (MixPath.IsEmpty())
        return nullptr;

    USoundMix* Mix = nullptr;
    Mix = Cast<USoundMix>(ResolveAsset(MixPath, /*bLoadObject=*/true).Object);
    if (Mix)
        return Mix;

    if (MixPath.Contains(TEXT("/")))
        return nullptr;

    FString AssetName = FPaths::GetBaseFilename(MixPath);
    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    TArray<FAssetData> AssetData;
    FARFilter Filter;
    Filter.ClassPaths.Add(USoundMix::StaticClass()->GetClassPathName());
    Filter.bRecursivePaths = true;
    Filter.PackagePaths.Add(TEXT("/Game"));
    AssetRegistryModule.Get().GetAssets(Filter, AssetData);

    for (const FAssetData& Data : AssetData)
    {
        if (Data.AssetName.ToString().Equals(AssetName, ESearchCase::IgnoreCase))
        {
            Mix = Cast<USoundMix>(Data.GetAsset());
            if (Mix)
                return Mix;
        }
    }
    return nullptr;
}

static USoundClass* ResolveSoundClass(const FString& ClassPath)
{
    if (ClassPath.IsEmpty())
        return nullptr;

    USoundClass* Class = nullptr;
    Class = Cast<USoundClass>(ResolveAsset(ClassPath, /*bLoadObject=*/true).Object);
    if (Class)
        return Class;

    if (ClassPath.Contains(TEXT("/")))
        return nullptr;

    FString AssetName = FPaths::GetBaseFilename(ClassPath);
    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    TArray<FAssetData> AssetData;
    FARFilter Filter;
    Filter.ClassPaths.Add(USoundClass::StaticClass()->GetClassPathName());
    Filter.bRecursivePaths = true;
    Filter.PackagePaths.Add(TEXT("/Game"));
    AssetRegistryModule.Get().GetAssets(Filter, AssetData);

    for (const FAssetData& Data : AssetData)
    {
        if (Data.AssetName.ToString().Equals(AssetName, ESearchCase::IgnoreCase))
        {
            Class = Cast<USoundClass>(Data.GetAsset());
            if (Class)
                return Class;
        }
    }
    return nullptr;
}


// ---------------------------------------------------------------------------
// audio.play_sound_at_location
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("audio.play_sound_at_location", "audio", "Spatialized one-shot playback of a USoundBase (cue/wave) at a world position; spawns a transient AudioComponent that auto-destroys when finished. Runtime op (works in PIE/editor world); not for asset authoring.",
    RPC_PARAMS(
        RPC_PARAM_REQ("soundPath", "path", "Asset path or name of the sound to play"),
        RPC_PARAM_OPT("location", "array", "World location [X,Y,Z]"),
        RPC_PARAM_OPT("rotation", "array", "World rotation [Pitch,Yaw,Roll]"),
        RPC_PARAM_OPT("volume", "number", "Volume multiplier (default 1.0)"),
        RPC_PARAM_OPT("pitch", "number", "Pitch multiplier (default 1.0)"),
        RPC_PARAM_OPT("startTime", "number", "Start time offset in seconds"),
        RPC_PARAM_OPT("attenuationPath", "path", "Path to SoundAttenuation asset"),
        RPC_PARAM_OPT("concurrencyPath", "path", "Path to SoundConcurrency asset")
    ))
{
    FString SoundPath;
    if (!Ctx.RequireString(TEXT("soundPath"), SoundPath)) return true;

    USoundBase* Sound = ResolveSoundAsset(SoundPath);
    if (!Sound)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), TEXT("Sound asset not found"));
        return true;
    }

    FVector Location = FVector::ZeroVector;
    FRotator Rotation = FRotator::ZeroRotator;
    TSharedPtr<FJsonObject> RawPayload = Ctx.GetRawPayload();

    const TArray<TSharedPtr<FJsonValue>>* LocArr;
    if (RawPayload->TryGetArrayField(TEXT("location"), LocArr) && LocArr && LocArr->Num() >= 3)
    {
        Location = FVector((*LocArr)[0]->AsNumber(), (*LocArr)[1]->AsNumber(), (*LocArr)[2]->AsNumber());
    }
    const TArray<TSharedPtr<FJsonValue>>* RotArr;
    if (RawPayload->TryGetArrayField(TEXT("rotation"), RotArr) && RotArr && RotArr->Num() >= 3)
    {
        Rotation = FRotator((*RotArr)[0]->AsNumber(), (*RotArr)[1]->AsNumber(), (*RotArr)[2]->AsNumber());
    }

    double Volume = Ctx.GetNumber(TEXT("volume"), 1.0);
    double Pitch = Ctx.GetNumber(TEXT("pitch"), 1.0);
    double StartTime = Ctx.GetNumber(TEXT("startTime"), 0.0);

    USoundAttenuation* Attenuation = nullptr;
    FString AttenPath = Ctx.GetString(TEXT("attenuationPath"));
    if (!AttenPath.IsEmpty())
    {
        Attenuation = LoadObject<USoundAttenuation>(nullptr, *AttenPath);
    }

    USoundConcurrency* Concurrency = nullptr;
    FString ConcPath = Ctx.GetString(TEXT("concurrencyPath"));
    if (!ConcPath.IsEmpty())
    {
        Concurrency = LoadObject<USoundConcurrency>(nullptr, *ConcPath);
    }

    if (!GEditor) return false;
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        Ctx.SendError(TEXT("NO_WORLD"), TEXT("No world context available"));
        return true;
    }

    UGameplayStatics::PlaySoundAtLocation(
        World, Sound, Location, Rotation, (float)Volume, (float)Pitch,
        (float)StartTime, Attenuation, Concurrency);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("soundPath"), SoundPath);
    TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
    LocObj->SetNumberField(TEXT("x"), Location.X);
    LocObj->SetNumberField(TEXT("y"), Location.Y);
    LocObj->SetNumberField(TEXT("z"), Location.Z);
    Resp->SetObjectField(TEXT("location"), LocObj);

    Ctx.SendSuccess(Resp);
    return true;
}

// ---------------------------------------------------------------------------
// audio.play_sound_2d
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("audio.play_sound_2d", "audio", "Non-spatialized one-shot playback of a USoundBase routed to the master submix (UI/HUD audio shape). Bypasses 3D positioning, attenuation, and concurrency. Runtime op only.",
    RPC_PARAMS(
        RPC_PARAM_REQ("soundPath", "path", "Asset path or name of the sound to play"),
        RPC_PARAM_OPT("volume", "number", "Volume multiplier (default 1.0)"),
        RPC_PARAM_OPT("pitch", "number", "Pitch multiplier (default 1.0)"),
        RPC_PARAM_OPT("startTime", "number", "Start time offset in seconds")
    ))
{
    FString SoundPath;
    if (!Ctx.RequireString(TEXT("soundPath"), SoundPath)) return true;

    USoundBase* Sound = ResolveSoundAsset(SoundPath);
    if (!Sound)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), TEXT("Sound asset not found"));
        return true;
    }

    double Volume = Ctx.GetNumber(TEXT("volume"), 1.0);
    double Pitch = Ctx.GetNumber(TEXT("pitch"), 1.0);
    double StartTime = Ctx.GetNumber(TEXT("startTime"), 0.0);

    if (!GEditor) return true;
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        Ctx.SendError(TEXT("NO_WORLD"), TEXT("No World Context"));
        return true;
    }

    UGameplayStatics::PlaySound2D(World, Sound, (float)Volume, (float)Pitch, (float)StartTime);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("soundPath"), SoundPath);
    Resp->SetNumberField(TEXT("volume"), Volume);
    Resp->SetNumberField(TEXT("pitch"), Pitch);
    AddAssetVerification(Resp, Sound);
    Ctx.SendSuccess(Resp);
    return true;
}


// ---------------------------------------------------------------------------
// audio.push_sound_mix
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("audio.push_sound_mix", "audio", "Activate a USoundMix on the global mix stack so its class adjusters take effect (volume/pitch tweaks per SoundClass). Pair with audio.pop_sound_mix to deactivate; mixes can be stacked.",
    RPC_PARAMS(
        RPC_PARAM_REQ("mixName", "path", "Asset path or name of the SoundMix to push")
    ))
{
    FString MixName;
    if (!Ctx.RequireString(TEXT("mixName"), MixName)) return true;

    USoundMix* Mix = ResolveSoundMix(MixName);
    if (Mix)
    {
        if (GEditor && GEditor->GetEditorWorldContext().World())
        {
            UGameplayStatics::PushSoundMixModifier(
                GEditor->GetEditorWorldContext().World(), Mix);
            TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
            Resp->SetBoolField(TEXT("success"), true);
            Resp->SetStringField(TEXT("mixName"), MixName);
            AddAssetVerification(Resp, Mix);
            Ctx.SendSuccess(Resp);
        }
        else
        {
            Ctx.SendError(TEXT("NO_WORLD"), TEXT("No World Context"));
        }
    }
    else
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), TEXT("SoundMix not found"));
    }
    return true;
}

// ---------------------------------------------------------------------------
// audio.pop_sound_mix
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("audio.pop_sound_mix", "audio", "Pop a SoundMix modifier from the audio stack",
    RPC_PARAMS(
        RPC_PARAM_REQ("mixName", "path", "Asset path or name of the SoundMix to pop")
    ))
{
    FString MixName;
    if (!Ctx.RequireString(TEXT("mixName"), MixName)) return true;

    USoundMix* Mix = ResolveSoundMix(MixName);
    if (Mix)
    {
        if (GEditor && GEditor->GetEditorWorldContext().World())
        {
            UGameplayStatics::PopSoundMixModifier(
                GEditor->GetEditorWorldContext().World(), Mix);
            TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
            Resp->SetBoolField(TEXT("success"), true);
            Resp->SetStringField(TEXT("mixName"), MixName);
            AddAssetVerification(Resp, Mix);
            Ctx.SendSuccess(Resp);
        }
        else
        {
            Ctx.SendError(TEXT("NO_WORLD"), TEXT("No World Context"));
        }
    }
    else
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), TEXT("SoundMix not found"));
    }
    return true;
}

// ---------------------------------------------------------------------------
// audio.set_sound_mix_class_override
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("audio.set_sound_mix_class_override", "audio", "Set a SoundMix class override for volume/pitch",
    RPC_PARAMS(
        RPC_PARAM_REQ("mixName", "path", "Asset path or name of the SoundMix"),
        RPC_PARAM_REQ("soundClassName", "path", "Asset path or name of the SoundClass"),
        RPC_PARAM_OPT("volume", "number", "Volume override (default 1.0)"),
        RPC_PARAM_OPT("pitch", "number", "Pitch override (default 1.0)"),
        RPC_PARAM_OPT("fadeInTime", "number", "Fade in time in seconds (default 1.0)"),
        RPC_PARAM_OPT("applyToChildren", "boolean", "Apply override to child classes (default true)")
    ))
{
    FString MixName = Ctx.GetString(TEXT("mixName"));
    FString ClassName = Ctx.GetString(TEXT("soundClassName"));

    USoundMix* Mix = ResolveSoundMix(MixName);
    USoundClass* Class = ResolveSoundClass(ClassName);

    if (!Mix || !Class)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), TEXT("Mix or Class not found"));
        return true;
    }

    double Volume = Ctx.GetNumber(TEXT("volume"), 1.0);
    double Pitch = Ctx.GetNumber(TEXT("pitch"), 1.0);
    double FadeTime = Ctx.GetNumber(TEXT("fadeInTime"), 1.0);
    bool bApply = Ctx.GetBool(TEXT("applyToChildren"), true);

    if (GEditor && GEditor->GetEditorWorldContext().World())
    {
        UGameplayStatics::SetSoundMixClassOverride(
            GEditor->GetEditorWorldContext().World(), Mix, Class, (float)Volume,
            (float)Pitch, (float)FadeTime, bApply);
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetBoolField(TEXT("success"), true);
        Resp->SetStringField(TEXT("mixName"), MixName);
        Resp->SetStringField(TEXT("className"), ClassName);
        Ctx.SendSuccess(Resp);
    }
    else
    {
        Ctx.SendError(TEXT("NO_WORLD"), TEXT("No World Context"));
    }
    return true;
}

// ---------------------------------------------------------------------------
// audio.play_sound_attached
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("audio.play_sound_attached", "audio", "Play a sound from an AudioComponent that follows a target actor (optionally a socket). The component auto-destroys when playback finishes. Runtime op; for persistent ambient sources see audio.create_ambient_sound.",
    RPC_PARAMS(
        RPC_PARAM_REQ("soundPath", "path", "Asset path of the USoundBase (Cue or Wave)."),
        RPC_PARAM_REQ("actorName", "string", "Display label or name of the actor to attach the AudioComponent to."),
        RPC_PARAM_OPT("attachPointName", "string", "Optional socket or component name on the actor; when omitted attaches to the root.")
    ))
{
    FString SoundPath = Ctx.GetString(TEXT("soundPath"));
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    FString AttachPoint = Ctx.GetString(TEXT("attachPointName"));

    USoundBase* Sound = ResolveSoundAsset(SoundPath);
    if (!Sound)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), TEXT("Sound not found"));
        return true;
    }

    if (!GEditor) return true;
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        Ctx.SendError(TEXT("NO_WORLD"), TEXT("No World Context"));
        return true;
    }

    AActor* TargetActor = FindAudioActorByName(ActorName, World);
    if (!TargetActor)
    {
        Ctx.SendError(TEXT("ACTOR_NOT_FOUND"), TEXT("Actor not found"));
        return true;
    }

    USceneComponent* AttachComp = TargetActor->GetRootComponent();
    if (!AttachPoint.IsEmpty())
    {
        USceneComponent* FoundComp = nullptr;
        TArray<USceneComponent*> Components;
        TargetActor->GetComponents(Components);
        for (USceneComponent* Comp : Components)
        {
            if (Comp->GetName() == AttachPoint ||
                Comp->DoesSocketExist(FName(*AttachPoint)))
            {
                FoundComp = Comp;
                break;
            }
        }
        if (FoundComp)
            AttachComp = FoundComp;
    }

    UAudioComponent* AudioComp = UGameplayStatics::SpawnSoundAttached(
        Sound, AttachComp, FName(*AttachPoint), FVector::ZeroVector,
        EAttachLocation::KeepRelativeOffset, true);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    if (AudioComp)
    {
        Resp->SetStringField(TEXT("componentName"), AudioComp->GetName());
        AddAssetVerification(Resp, Sound);
        AddComponentVerification(Resp, AudioComp);
        Ctx.SendSuccess(Resp);
    }
    else
    {
        Ctx.SendError(TEXT("ATTACH_FAILED"), TEXT("Failed to attach sound"));
    }
    return true;
}

// ---------------------------------------------------------------------------
// audio.fade_sound_out
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("audio.fade_sound_out", "audio", "Linearly ramp the volume of an actor's first AudioComponent down to a target level over fadeTime seconds; the component then stops automatically when target is 0.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Display label or name of the actor whose AudioComponent should fade."),
        RPC_PARAM_OPT("fadeTime", "number", "Duration of the fade in seconds; defaults to 1.0."),
        RPC_PARAM_OPT("targetVolume", "number", "Final volume multiplier at the end of the fade; defaults to 0.0 (silence).")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    double FadeTime = Ctx.GetNumber(TEXT("fadeTime"), 1.0);
    double TargetVol = Ctx.GetNumber(TEXT("targetVolume"), 0.0);

    if (!GEditor) return true;
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        Ctx.SendError(TEXT("NO_WORLD"), TEXT("No World Context"));
        return true;
    }

    AActor* TargetActor = FindAudioActorByName(ActorName, World);
    if (TargetActor)
    {
        UAudioComponent* AudioComp = TargetActor->FindComponentByClass<UAudioComponent>();
        if (AudioComp)
        {
            AudioComp->FadeOut((float)FadeTime, (float)TargetVol);

            TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
            Resp->SetBoolField(TEXT("success"), true);
            Resp->SetStringField(TEXT("actorName"), ActorName);
            Resp->SetStringField(TEXT("action"), TEXT("fade_sound_out"));
            AddActorVerification(Resp, TargetActor);
            Ctx.SendSuccess(Resp);
            return true;
        }
    }
    Ctx.SendError(TEXT("COMPONENT_NOT_FOUND"), TEXT("Audio component not found on actor"));
    return true;
}

// ---------------------------------------------------------------------------
// audio.fade_sound_in
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("audio.fade_sound_in", "audio", "Fade in a sound on an actor's audio component",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name or path of the actor with the audio component"),
        RPC_PARAM_OPT("fadeTime", "number", "Fade duration in seconds (default 1.0)"),
        RPC_PARAM_OPT("targetVolume", "number", "Target volume (default 1.0)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    double FadeTime = Ctx.GetNumber(TEXT("fadeTime"), 1.0);
    double TargetVol = Ctx.GetNumber(TEXT("targetVolume"), 1.0);

    if (!GEditor) return true;
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        Ctx.SendError(TEXT("NO_WORLD"), TEXT("No World Context"));
        return true;
    }

    AActor* TargetActor = FindAudioActorByName(ActorName, World);
    if (TargetActor)
    {
        UAudioComponent* AudioComp = TargetActor->FindComponentByClass<UAudioComponent>();
        if (AudioComp)
        {
            AudioComp->FadeIn((float)FadeTime, (float)TargetVol);

            TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
            Resp->SetBoolField(TEXT("success"), true);
            Resp->SetStringField(TEXT("actorName"), ActorName);
            Resp->SetStringField(TEXT("action"), TEXT("fade_sound_in"));
            AddActorVerification(Resp, TargetActor);
            Ctx.SendSuccess(Resp);
            return true;
        }
    }
    Ctx.SendError(TEXT("COMPONENT_NOT_FOUND"), TEXT("Audio component not found on actor"));
    return true;
}

// ---------------------------------------------------------------------------
// audio.create_ambient_sound
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("audio.create_ambient_sound", "audio", "Spawn an AAmbientSound actor (looping/persistent audio source) at a world position with the given Sound asset. Unlike audio.play_sound_at_location which auto-destroys, this actor stays in the level until deleted.",
    RPC_PARAMS(
        RPC_PARAM_REQ("soundPath", "path", "Asset path or name of the sound"),
        RPC_PARAM_OPT("location", "object|array", "World location, array [X,Y,Z] or object {x,y,z}"),
        RPC_PARAM_OPT("volume", "number", "Volume multiplier (default 1.0)"),
        RPC_PARAM_OPT("pitch", "number", "Pitch multiplier (default 1.0)"),
        RPC_PARAM_OPT("startTime", "number", "Start time offset in seconds"),
        RPC_PARAM_OPT("attenuationPath", "path", "Path to SoundAttenuation asset"),
        RPC_PARAM_OPT("concurrencyPath", "path", "Path to SoundConcurrency asset")
    ))
{
    FString SoundPath;
    if (!Ctx.RequireString(TEXT("soundPath"), SoundPath)) return true;

    USoundBase* Sound = ResolveSoundAsset(SoundPath);
    if (!Sound)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), TEXT("Sound asset not found"));
        return true;
    }

    // Accept both the array [X,Y,Z] and object {x,y,z} location shapes via the
    // shared ExtractVectorField helper — same as the sibling
    // audio.create_audio_component (line ~899). The previous array-only parse
    // silently dropped an object-form location to the zero default, spawning the
    // ambient actor at the world origin with a success-shaped response.
    TSharedPtr<FJsonObject> RawPayload = Ctx.GetRawPayload();
    FVector Location = ExtractVectorField(RawPayload, TEXT("location"), FVector::ZeroVector);

    double Volume = Ctx.GetNumber(TEXT("volume"), 1.0);
    double Pitch = Ctx.GetNumber(TEXT("pitch"), 1.0);
    double StartTime = Ctx.GetNumber(TEXT("startTime"), 0.0);

    USoundAttenuation* Attenuation = nullptr;
    FString AttenPath = Ctx.GetString(TEXT("attenuationPath"));
    if (!AttenPath.IsEmpty())
    {
        Attenuation = LoadObject<USoundAttenuation>(nullptr, *AttenPath);
    }

    USoundConcurrency* Concurrency = nullptr;
    FString ConcPath = Ctx.GetString(TEXT("concurrencyPath"));
    if (!ConcPath.IsEmpty())
    {
        Concurrency = LoadObject<USoundConcurrency>(nullptr, *ConcPath);
    }

    if (!GEditor) return true;
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        Ctx.SendError(TEXT("NO_WORLD"), TEXT("No World Context"));
        return true;
    }

    // The doc promises a persistent AAmbientSound *actor* that stays in the level
    // until deleted — not a transient UAudioComponent. Spawn the actor so the
    // result is selectable/movable/deletable through the actor namespace.
    // AAmbientSound is a MinimalAPI engine UCLASS, so its StaticClass() is not
    // exported for cross-module linkage; resolve the class by reflection and
    // spawn via the AActor base (see UE version compat note in CLAUDE.md).
    UClass* AmbientSoundClass = FindObject<UClass>(nullptr, TEXT("/Script/Engine.AmbientSound"));
    if (!AmbientSoundClass)
    {
        Ctx.SendError(TEXT("CLASS_NOT_FOUND"), TEXT("AAmbientSound class not found"));
        return true;
    }

    // Route through the shared spawn helper so this handler matches every other
    // spawn site (PIE/editor/unattended path selection, collision policy, outliner
    // label) instead of hand-rolling FActorSpawnParameters + World->SpawnActor.
    AActor* AmbientActor = SpawnActorInActiveWorld<AActor>(
        AmbientSoundClass, Location, FRotator::ZeroRotator, /*OptionalLabel*/ FString());
    if (!AmbientActor)
    {
        Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to create ambient sound"));
        return true;
    }

    // Configure the actor's AudioComponent with the requested sound and tuning.
    UAudioComponent* AudioComp = AmbientActor->FindComponentByClass<UAudioComponent>();
    if (AudioComp)
    {
        AudioComp->SetSound(Sound);
        AudioComp->VolumeMultiplier = (float)Volume;
        AudioComp->PitchMultiplier = (float)Pitch;
        if (Attenuation)
        {
            AudioComp->AttenuationSettings = Attenuation;
        }
        if (Concurrency)
        {
            AudioComp->ConcurrencySet.Add(Concurrency);
        }
        // Honor the requested start-time offset and begin playback; this also
        // activates the component so the placed ambient bed is audible.
        AudioComp->Play((float)StartTime);
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    AddActorVerification(Resp, AmbientActor);
    AddAssetVerification(Resp, Sound);
    // Mirror the sibling audio.spawn_sound_at_location handler: emit componentName
    // plus componentClass/ownerActorPath via the shared helper (it null-checks too).
    AddComponentVerification(Resp, AudioComp);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---------------------------------------------------------------------------
// audio.spawn_sound_at_location
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("audio.spawn_sound_at_location", "audio", "Spawn a sound component at a world location",
    RPC_PARAMS(
        RPC_PARAM_REQ("soundPath", "path", "Asset path or name of the sound"),
        RPC_PARAM_OPT("location", "array", "World location [X,Y,Z]"),
        RPC_PARAM_OPT("rotation", "array", "World rotation [Pitch,Yaw,Roll]"),
        RPC_PARAM_OPT("volume", "number", "Volume multiplier (default 1.0)"),
        RPC_PARAM_OPT("pitch", "number", "Pitch multiplier (default 1.0)"),
        RPC_PARAM_OPT("startTime", "number", "Start time offset in seconds")
    ))
{
    FString SoundPath;
    if (!Ctx.RequireString(TEXT("soundPath"), SoundPath)) return true;

    USoundBase* Sound = ResolveSoundAsset(SoundPath);
    if (!Sound)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), TEXT("Sound asset not found"));
        return true;
    }

    FVector Location = FVector::ZeroVector;
    FRotator Rotation = FRotator::ZeroRotator;
    TSharedPtr<FJsonObject> RawPayload = Ctx.GetRawPayload();

    const TArray<TSharedPtr<FJsonValue>>* LocArr;
    if (RawPayload->TryGetArrayField(TEXT("location"), LocArr) && LocArr && LocArr->Num() >= 3)
    {
        Location = FVector((*LocArr)[0]->AsNumber(), (*LocArr)[1]->AsNumber(), (*LocArr)[2]->AsNumber());
    }
    const TArray<TSharedPtr<FJsonValue>>* RotArr;
    if (RawPayload->TryGetArrayField(TEXT("rotation"), RotArr) && RotArr && RotArr->Num() >= 3)
    {
        Rotation = FRotator((*RotArr)[0]->AsNumber(), (*RotArr)[1]->AsNumber(), (*RotArr)[2]->AsNumber());
    }

    double Volume = Ctx.GetNumber(TEXT("volume"), 1.0);
    double Pitch = Ctx.GetNumber(TEXT("pitch"), 1.0);
    double StartTime = Ctx.GetNumber(TEXT("startTime"), 0.0);

    if (!GEditor) return true;
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        Ctx.SendError(TEXT("NO_WORLD"), TEXT("No World Context"));
        return true;
    }

    UAudioComponent* AudioComp = UGameplayStatics::SpawnSoundAtLocation(
        World, Sound, Location, Rotation, (float)Volume, (float)Pitch,
        (float)StartTime, nullptr, nullptr, true);

    if (AudioComp)
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        AddAssetVerification(Resp, Sound);
        AddComponentVerification(Resp, AudioComp);
        Ctx.SendSuccess(Resp);
    }
    else
    {
        Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn sound"));
    }
    return true;
}

// ---------------------------------------------------------------------------
// audio.clear_sound_mix_class_override
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("audio.clear_sound_mix_class_override", "audio", "Clear a SoundMix class override",
    RPC_PARAMS(
        RPC_PARAM_REQ("mixName", "path", "Asset path or name of the SoundMix"),
        RPC_PARAM_REQ("soundClassName", "path", "Asset path or name of the SoundClass"),
        RPC_PARAM_OPT("fadeOutTime", "number", "Fade out time in seconds (default 1.0)")
    ))
{
    FString MixName = Ctx.GetString(TEXT("mixName"));
    FString ClassName = Ctx.GetString(TEXT("soundClassName"));

    USoundMix* Mix = ResolveSoundMix(MixName);
    USoundClass* Class = ResolveSoundClass(ClassName);

    if (!Mix || !Class)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), TEXT("Mix or Class not found"));
        return true;
    }

    double FadeTime = Ctx.GetNumber(TEXT("fadeOutTime"), 1.0);

    if (GEditor && GEditor->GetEditorWorldContext().World())
    {
        UGameplayStatics::ClearSoundMixClassOverride(
            GEditor->GetEditorWorldContext().World(), Mix, Class, (float)FadeTime);
        Ctx.SendSuccess(TEXT("Sound mix override cleared"));
    }
    else
    {
        Ctx.SendError(TEXT("NO_WORLD"), TEXT("No World Context"));
    }
    return true;
}

// ---------------------------------------------------------------------------
// audio.set_base_sound_mix
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("audio.set_base_sound_mix", "audio", "Set the base SoundMix for the world",
    RPC_PARAMS(
        RPC_PARAM_REQ("mixName", "path", "Asset path or name of the SoundMix")
    ))
{
    FString MixName = Ctx.GetString(TEXT("mixName"));
    USoundMix* Mix = ResolveSoundMix(MixName);
    if (!Mix)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), TEXT("Mix not found"));
        return true;
    }
    if (GEditor && GEditor->GetEditorWorldContext().World())
    {
        UGameplayStatics::SetBaseSoundMix(
            GEditor->GetEditorWorldContext().World(), Mix);
        Ctx.SendSuccess(TEXT("Base sound mix set"));
    }
    else
    {
        Ctx.SendError(TEXT("NO_WORLD"), TEXT("No World Context"));
    }
    return true;
}

// ---------------------------------------------------------------------------
// audio.prime_sound
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("audio.prime_sound", "audio", "Prime a sound asset for immediate playback",
    RPC_PARAMS(
        RPC_PARAM_REQ("soundPath", "path", "Asset path or name of the sound to prime")
    ))
{
    FString SoundPath = Ctx.GetString(TEXT("soundPath"));
    USoundBase* Sound = ResolveSoundAsset(SoundPath);
    if (!Sound)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), TEXT("Sound not found"));
        return true;
    }
    UGameplayStatics::PrimeSound(Sound);
    Ctx.SendSuccess(TEXT("Sound primed"));
    return true;
}

// ---------------------------------------------------------------------------
// audio.create_audio_component
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("audio.create_audio_component", "audio", "Create a UAudioComponent in the world (attached to an actor or freestanding at a location) bound to a USoundBase. Stays alive until destroyed; useful for runtime sources you'll later mutate (volume/pitch/play).",
    RPC_PARAMS(
        ParamAliasUtils::MakeAliasParamSpec(TEXT("soundPath"), TEXT("string"),
            TEXT("Asset path or name of the sound. The generic `path` spelling is also accepted."),
            /*bRequired=*/true, TArray<FString>({TEXT("soundPath"), TEXT("path")})),
        RPC_PARAM_OPT("location", "object", "World location {x,y,z}"),
        RPC_PARAM_OPT("rotation", "object", "World rotation {pitch,yaw,roll}"),
        RPC_PARAM_OPT("attachTo", "string", "Actor name to attach the component to"),
        RPC_PARAM_OPT("actorName", "string", "Alias for attachTo"),
        RPC_PARAM_OPT("volume", "string", "Volume multiplier as string"),
        RPC_PARAM_OPT("pitch", "string", "Pitch multiplier as string")
    ))
{
    FString SoundPath = Ctx.GetStringFirstOf({TEXT("soundPath"), TEXT("path")});
    if (SoundPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("soundPath required"));
        return true;
    }

    USoundBase* Sound = ResolveSoundAsset(SoundPath);
    if (!Sound)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Sound asset not found: %s"), *SoundPath));
        return true;
    }

    TSharedPtr<FJsonObject> RawPayload = Ctx.GetRawPayload();
    FVector Location = ExtractVectorField(RawPayload, TEXT("location"), FVector::ZeroVector);
    FRotator Rotation = ExtractRotatorField(RawPayload, TEXT("rotation"), FRotator::ZeroRotator);

    FString AttachTo = Ctx.GetStringFirstOf({TEXT("attachTo"), TEXT("actorName")});

    UAudioComponent* AudioComp = nullptr;
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;

    if (!World)
    {
        Ctx.SendError(TEXT("NO_WORLD"), TEXT("No editor world"));
        return true;
    }

    if (!AttachTo.IsEmpty())
    {
        AActor* ParentActor = FindAudioActorByName(AttachTo, World);
        if (ParentActor)
        {
            AudioComp = UGameplayStatics::SpawnSoundAttached(
                Sound, ParentActor->GetRootComponent(), NAME_None, Location,
                Rotation, EAttachLocation::KeepRelativeOffset, false);
        }
        else
        {
            UE_LOG(LogPinWrightSubsystem, Warning,
                   TEXT("create_audio_component: attachTo actor '%s' not found, spawning at location."),
                   *AttachTo);
        }
    }

    if (!AudioComp)
    {
        AudioComp = UGameplayStatics::SpawnSoundAtLocation(World, Sound, Location, Rotation);
    }

    if (AudioComp)
    {
        FString VolumeStr = Ctx.GetString(TEXT("volume"));
        if (!VolumeStr.IsEmpty())
            AudioComp->SetVolumeMultiplier(FCString::Atof(*VolumeStr));
        FString PitchStr = Ctx.GetString(TEXT("pitch"));
        if (!PitchStr.IsEmpty())
            AudioComp->SetPitchMultiplier(FCString::Atof(*PitchStr));

        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetBoolField(TEXT("success"), true);
        Resp->SetStringField(TEXT("componentPath"), AudioComp->GetPathName());
        Resp->SetStringField(TEXT("componentName"), AudioComp->GetName());
        Ctx.SendSuccess(Resp);
        return true;
    }
    Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create audio component"));
    return true;
}
