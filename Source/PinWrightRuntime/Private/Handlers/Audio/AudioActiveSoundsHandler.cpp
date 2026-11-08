// Copyright (c) 2026 Alexander Penkin. MIT License.

// audio.list_active_sounds - read-only snapshot of what the audio device(s) are playing.
//
// WHY THIS IS NOT A UOBJECT QUERY. UGameplayStatics::PlaySoundAtLocation / PlaySound2D create an
// FActiveSound inside the audio device and no UAudioComponent, so system.inspect.* (which walks
// UObjects) returns zero instances while those sounds are audibly playing. The device has to be
// asked directly.
//
// TWO CONTAINERS, AND THE ROW SAYS WHICH. A looping sound past its audible range or its
// concurrency limit is moved out of FAudioDevice::ActiveSounds into the private VirtualLoops map
// (AudioDevice.cpp AddVirtualLoop), so reading ActiveSounds alone reports a virtualized sound as
// "not playing". `virtualized` is the container the row came from, never a re-derived flag.

#include "Handlers/Audio/AudioActiveSoundSnapshot.h"

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"

#include "ActiveSound.h"
#include "AudioDevice.h"
#include "AudioDeviceManager.h"
#include "AudioThread.h"
#include "AudioVirtualLoop.h"
#include "Components/AudioComponent.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/App.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundClass.h"

// FAudioDevice::VirtualLoops is private with no accessor on any engine in the range, and it is the
// only place a virtualized sound exists. The engine's own private-member macro reads it in place,
// which beats keeping a second copy of the set (an IActiveSoundUpdateInterface subsystem, 5.5+
// only) that could drift from the authority. The macro must sit at global scope.
#if __has_include("Misc/DefinePrivateMemberPtr.h")
#include "Misc/DefinePrivateMemberPtr.h"
#define PINWRIGHT_CAN_READ_AUDIO_VIRTUAL_LOOPS 1
UE_DEFINE_PRIVATE_MEMBER_PTR((TMap<FActiveSound*, FAudioVirtualLoop>), GPinWrightAudioDeviceVirtualLoops,
    FAudioDevice, VirtualLoops);
#else
#define PINWRIGHT_CAN_READ_AUDIO_VIRTUAL_LOOPS 0
#endif

namespace PwActiveSoundSnapshot
{
    static void SetStringOrNull(const TSharedRef<FJsonObject>& Obj, const TCHAR* Field, const FString& Value)
    {
        if (Value.IsEmpty())
        {
            Obj->SetField(Field, MakeShared<FJsonValueNull>());
        }
        else
        {
            Obj->SetStringField(Field, Value);
        }
    }

    TSharedRef<FJsonObject> DescribeActiveSound(const FActiveSound& ActiveSound, uint32 DeviceId,
        bool bVirtualized)
    {
        TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetNumberField(TEXT("deviceId"), DeviceId);
        Row->SetNumberField(TEXT("playOrder"), ActiveSound.GetPlayOrder());

        const USoundBase* Sound = ActiveSound.GetSound();
        SetStringOrNull(Row, TEXT("soundPath"), Sound ? Sound->GetPathName() : FString());
        const USoundClass* SoundClass = ActiveSound.GetSoundClass();
        SetStringOrNull(Row, TEXT("soundClassPath"), SoundClass ? SoundClass->GetPathName() : FString());

        Row->SetBoolField(TEXT("virtualized"), bVirtualized);
        Row->SetBoolField(TEXT("playingAudio"), ActiveSound.IsPlayingAudio());
        Row->SetBoolField(TEXT("stopping"), ActiveSound.bIsStopping != 0);
        Row->SetBoolField(TEXT("looping"), ActiveSound.IsLooping());

        Row->SetBoolField(TEXT("hasLocation"), ActiveSound.bLocationDefined != 0);
        const FVector Location = ActiveSound.Transform.GetLocation();
        TSharedRef<FJsonObject> Loc = MakeShared<FJsonObject>();
        Loc->SetNumberField(TEXT("x"), Location.X);
        Loc->SetNumberField(TEXT("y"), Location.Y);
        Loc->SetNumberField(TEXT("z"), Location.Z);
        Row->SetObjectField(TEXT("location"), Loc);

        Row->SetNumberField(TEXT("playbackTimeSeconds"), ActiveSound.PlaybackTime);
        Row->SetNumberField(TEXT("requestedStartTime"), ActiveSound.RequestedStartTime);
        Row->SetNumberField(TEXT("volumeMultiplier"), ActiveSound.VolumeMultiplier);
        Row->SetNumberField(TEXT("pitchMultiplier"), ActiveSound.GetPitch());

        UWorld* World = ActiveSound.GetWorld();
        SetStringOrNull(Row, TEXT("world"), World ? World->GetPathName() : FString());
        SetStringOrNull(Row, TEXT("worldType"), World ? FString(LexToString(World->WorldType)) : FString());
        // The engine's own WorldTimeWhenPlayed (ActiveSound.cpp, the world-time parameter update).
        // Omitted for a virtualized row: PlaybackTimeUnscaled stops advancing while virtual
        // (AudioVirtualLoop.cpp Update advances PlaybackTime only), so the difference would drift.
        if (World && !bVirtualized)
        {
            Row->SetNumberField(TEXT("startWorldTimeSeconds"),
                World->GetTimeSeconds() - ActiveSound.PlaybackTimeUnscaled);
        }

        // Zero is the fire-and-forget case (PlaySoundAtLocation / PlaySound2D), itself the signal.
        const uint64 ComponentId = ActiveSound.GetAudioComponentID();
        Row->SetNumberField(TEXT("audioComponentId"), static_cast<double>(ComponentId));
        const UAudioComponent* Component = ComponentId > 0
            ? UAudioComponent::GetAudioComponentFromID(ComponentId) : nullptr;
        SetStringOrNull(Row, TEXT("audioComponentPath"), Component ? Component->GetPathName() : FString());

        // GetOwnerName answers the literal "None" when no owner was set (ActiveSound.cpp).
        const FString OwnerName = ActiveSound.GetOwnerName();
        SetStringOrNull(Row, TEXT("ownerName"), OwnerName == TEXT("None") ? FString() : OwnerName);
        return Row;
    }
}

REGISTER_RPC_HANDLER("audio.list_active_sounds", "audio",
    "Read-only snapshot of every sound the audio device(s) are playing right now, one row per engine "
    "FActiveSound - including fire-and-forget sounds (PlaySoundAtLocation / PlaySound2D) that have no "
    "UAudioComponent and looping sounds the device has virtualized. Rows carry soundPath, location, "
    "playback time, virtualized, and the owning component (null for fire-and-forget). Errors with "
    "AUDIO_DEVICE_UNAVAILABLE when no audio device exists (e.g. -nosound).",
    RPC_NO_PARAMS)
{
    FAudioDeviceManager* Manager = GEngine ? GEngine->GetAudioDeviceManager() : nullptr;

    int32 DevicesInspected = 0;
#if PINWRIGHT_CAN_READ_AUDIO_VIRTUAL_LOOPS
    int32 VirtualizedCount = 0;
#endif
    TArray<TPair<uint32, TSharedRef<FJsonObject>>> Rows;
    if (Manager)
    {
        // Freeze the audio thread so both containers are read as one consistent frame. Commands the
        // game thread queued earlier (a PlaySoundAtLocation just issued) run before the suspend
        // lands, so a sound started on this stack is already in the device when we look.
        FAudioThreadSuspendContext SuspendAudio;
        Manager->IterateOverAllDevices([&](Audio::FDeviceId DeviceId, FAudioDevice* Device)
        {
            if (!Device)
            {
                return;
            }
            ++DevicesInspected;
            for (const FActiveSound* ActiveSound : Device->GetActiveSounds())
            {
                if (ActiveSound)
                {
                    Rows.Emplace(ActiveSound->GetPlayOrder(),
                        PwActiveSoundSnapshot::DescribeActiveSound(*ActiveSound, DeviceId, false));
                }
            }
#if PINWRIGHT_CAN_READ_AUDIO_VIRTUAL_LOOPS
            for (const auto& Pair : Device->*GPinWrightAudioDeviceVirtualLoops)
            {
                if (Pair.Key)
                {
                    ++VirtualizedCount;
                    Rows.Emplace(Pair.Key->GetPlayOrder(),
                        PwActiveSoundSnapshot::DescribeActiveSound(*Pair.Key, DeviceId, true));
                }
            }
#endif
        });
    }

    if (DevicesInspected == 0)
    {
        // Unanswerable, not empty: with no device there is nothing that could be playing to look at,
        // and an empty list here would read as "nothing is playing".
        TSharedPtr<FJsonObject> Detail = MakeShared<FJsonObject>();
        Detail->SetBoolField(TEXT("audioDeviceManager"), Manager != nullptr);
        Detail->SetBoolField(TEXT("canEverRenderAudio"), FApp::CanEverRenderAudio());
        const FString Cause = FApp::CanEverRenderAudio()
            ? FString(TEXT("Audio rendering is allowed (canEverRenderAudio=true) but no device was created or ")
                TEXT("initialized; check the LogAudio startup lines for the device failure."))
            : FString(TEXT("Audio rendering is disabled for this process (canEverRenderAudio=false, e.g. ")
                TEXT("-nosound); relaunch the editor with audio enabled."));
        Ctx.SendError(ErrorCodes::ERR_AUDIO_DEVICE_UNAVAILABLE,
            TEXT("No audio device exists in this editor, so active sounds cannot be observed. ") + Cause,
            Detail);
        return true;
    }

    // Play order is the device's dispatch order, so two rows for one sound are two dispatches.
    Rows.StableSort([](const TPair<uint32, TSharedRef<FJsonObject>>& A,
        const TPair<uint32, TSharedRef<FJsonObject>>& B) { return A.Key < B.Key; });

    TArray<TSharedPtr<FJsonValue>> Sounds;
    for (const TPair<uint32, TSharedRef<FJsonObject>>& Row : Rows)
    {
        Sounds.Add(MakeShared<FJsonValueObject>(Row.Value));
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetNumberField(TEXT("devicesInspected"), DevicesInspected);
    Resp->SetNumberField(TEXT("count"), Sounds.Num());
    Resp->SetBoolField(TEXT("virtualizedEnumerated"), PINWRIGHT_CAN_READ_AUDIO_VIRTUAL_LOOPS != 0);
#if PINWRIGHT_CAN_READ_AUDIO_VIRTUAL_LOOPS
    Resp->SetNumberField(TEXT("virtualizedCount"), VirtualizedCount);
#else
    TArray<TSharedPtr<FJsonValue>> Warnings;
    Warnings.Add(MakeShared<FJsonValueString>(TEXT(
        "This engine lacks Misc/DefinePrivateMemberPtr.h, so the device's virtual-loop map was not read: ")
        TEXT("virtualized sounds are absent from `sounds`, and every row present is non-virtualized.")));
    Resp->SetArrayField(TEXT("warnings"), Warnings);
#endif
    Resp->SetArrayField(TEXT("sounds"), Sounds);
    Ctx.SendSuccess(Resp);
    return true;
}
