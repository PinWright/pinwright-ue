// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for audio.list_active_sounds.
//
// HOST SPLIT. The suite runs with -nosound, where no audio device manager exists
// (FAudioDeviceManager::GetOrCreate refuses when FApp::CanEverRenderAudio() is false), so nothing
// can play. Two tests cover the two halves:
//   - RowDescribesTheActiveSoundItWasGiven drives the row builder with a hand-made FActiveSound and
//     runs everywhere.
//   - FireAndForgetSoundIsListedOrDeviceAbsenceIsAnError pins BOTH directions through the handler:
//     no device must be the AUDIO_DEVICE_UNAVAILABLE error (never an empty list), and with a device
//     a PlaySoundAtLocation sound - which creates no UAudioComponent - must appear as a row. The
//     second half emits the skip marker on an audio-disabled host.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "UObject/StrongObjectPtr.h"

#include "Handlers/Audio/AudioActiveSoundSnapshot.h"
#include "Handlers/ErrorCodes.h"

#include "ActiveSound.h"
#include "AudioDevice.h"
#include "AudioDeviceManager.h"
#include "AudioThread.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/App.h"
#include "Sound/SoundWave.h"
#include "Sound/SoundWaveProcedural.h"

namespace TestAudioListActiveSoundsHelpers
{
    const TCHAR* const MethodName = TEXT("audio.list_active_sounds");

    TSharedPtr<FJsonObject> FindRowBySoundPath(const TSharedPtr<FJsonObject>& Result, const FString& SoundPath)
    {
        const TArray<TSharedPtr<FJsonValue>>* Sounds = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("sounds"), Sounds))
        {
            return nullptr;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Sounds)
        {
            const TSharedPtr<FJsonObject> Row = Value.IsValid() ? Value->AsObject() : nullptr;
            FString Path;
            if (Row.IsValid() && Row->TryGetStringField(TEXT("soundPath"), Path) && Path == SoundPath)
            {
                return Row;
            }
        }
        return nullptr;
    }

    // StopSoundsUsingResource matches by wave instance, which a sound has not grown before the
    // device's next update, so stop by FActiveSound::GetSound on the audio thread and fence.
    void StopSoundsPlaying(FAudioDevice* Device, const USoundBase* Sound)
    {
        FAudioThread::RunCommandOnAudioThread([Device, Sound]()
        {
            for (FActiveSound* ActiveSound : Device->GetActiveSounds())
            {
                if (ActiveSound && ActiveSound->GetSound() == Sound)
                {
                    Device->AddSoundToStop(ActiveSound);
                }
            }
        });
        FAudioCommandFence Fence;
        Fence.BeginFence();
        Fence.Wait();
    }
}

// =========================================================================
// A. The row reports the FActiveSound it was handed, and `virtualized` is the caller's container,
//    not a constant: the same sound flips with it, and only the live row carries a start time.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioListActiveSoundsRowDescribesTest,
    "PinWright.audio.list_active_sounds.RowDescribesTheActiveSoundItWasGiven",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioListActiveSoundsRowDescribesTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    TStrongObjectPtr<USoundWave> Wave(NewObject<USoundWave>(GetTransientPackage()));
    if (!TestNotNull(TEXT("editor world exists"), World) || !TestTrue(TEXT("fixture wave created"), Wave.IsValid()))
    {
        return false;
    }

    FActiveSound ActiveSound;
    ActiveSound.SetSound(Wave.Get());
    ActiveSound.SetWorld(World);
    ActiveSound.bLocationDefined = true;
    ActiveSound.Transform.SetTranslation(FVector(12.0, -34.0, 56.0));
    ActiveSound.PlaybackTime = 1.25f;
    ActiveSound.PlaybackTimeUnscaled = 1.0f;
    ActiveSound.RequestedStartTime = 0.5f;
    TestEqual(TEXT("precondition: no owning component (the fire-and-forget shape)"),
        ActiveSound.GetAudioComponentID(), static_cast<uint64>(0));

    const TSharedRef<FJsonObject> Live = PwActiveSoundSnapshot::DescribeActiveSound(ActiveSound, 7, false);
    TestEqual(TEXT("soundPath is the played asset"), Live->GetStringField(TEXT("soundPath")), Wave->GetPathName());
    TestEqual(TEXT("deviceId echoes the device it was read from"), Live->GetNumberField(TEXT("deviceId")), 7.0);
    TestFalse(TEXT("live row is not virtualized"), Live->GetBoolField(TEXT("virtualized")));
    TestTrue(TEXT("hasLocation follows bLocationDefined"), Live->GetBoolField(TEXT("hasLocation")));
    const TSharedPtr<FJsonObject> Loc = Live->GetObjectField(TEXT("location"));
    TestEqual(TEXT("location.x"), Loc->GetNumberField(TEXT("x")), 12.0);
    TestEqual(TEXT("location.y"), Loc->GetNumberField(TEXT("y")), -34.0);
    TestEqual(TEXT("location.z"), Loc->GetNumberField(TEXT("z")), 56.0);
    TestEqual(TEXT("playbackTimeSeconds"), Live->GetNumberField(TEXT("playbackTimeSeconds")), 1.25);
    TestEqual(TEXT("requestedStartTime"), Live->GetNumberField(TEXT("requestedStartTime")), 0.5);
    TestEqual(TEXT("world is the sound's world"), Live->GetStringField(TEXT("world")), World->GetPathName());
    TestEqual(TEXT("startWorldTimeSeconds = world time - unscaled playback (engine WorldTimeWhenPlayed)"),
        Live->GetNumberField(TEXT("startWorldTimeSeconds")), World->GetTimeSeconds() - 1.0, 1e-3);
    TestTrue(TEXT("no owning component reports audioComponentPath as JSON null"),
        Live->HasTypedField<EJson::Null>(TEXT("audioComponentPath")));
    TestTrue(TEXT("no owner reports ownerName as JSON null"),
        Live->HasTypedField<EJson::Null>(TEXT("ownerName")));

    const TSharedRef<FJsonObject> Virtual = PwActiveSoundSnapshot::DescribeActiveSound(ActiveSound, 7, true);
    TestTrue(TEXT("the same sound read from the virtual-loop container reports virtualized"),
        Virtual->GetBoolField(TEXT("virtualized")));
    TestFalse(TEXT("a virtualized row omits startWorldTimeSeconds (unscaled time is frozen while virtual)"),
        Virtual->HasField(TEXT("startWorldTimeSeconds")));
    return true;
}

// =========================================================================
// B. Both directions through the handler. No device: an error, not an empty success. A device: a
//    sound started with UGameplayStatics::PlaySoundAtLocation (no UAudioComponent) is listed.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioListActiveSoundsFireAndForgetTest,
    "PinWright.audio.list_active_sounds.FireAndForgetSoundIsListedOrDeviceAbsenceIsAnError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioListActiveSoundsFireAndForgetTest::RunTest(const FString& Parameters)
{
    using namespace TestAudioListActiveSoundsHelpers;

    // Independent of the handler's IterateOverAllDevices walk.
    FAudioDeviceManager* Manager = GEngine ? GEngine->GetAudioDeviceManager() : nullptr;
    const bool bHasDevice = Manager && Manager->GetNumActiveAudioDevices() > 0;

    if (!bHasDevice)
    {
        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(MethodName, MakeShared<FJsonObject>(), Capture);
        TestTrue(TEXT("audio.list_active_sounds is registered"), bFound);
        TestTrue(TEXT("handler responded"), Capture.bWasCalled);
        TestFalse(TEXT("no audio device is an error, never an empty list"), Capture.bSuccess);
        TestEqual(TEXT("error code"), Capture.ErrorCode, FString(ErrorCodes::ERR_AUDIO_DEVICE_UNAVAILABLE));
        bool bCanRender = !FApp::CanEverRenderAudio();
        TestTrue(TEXT("error payload carries canEverRenderAudio"),
            Capture.Result.IsValid() && Capture.Result->TryGetBoolField(TEXT("canEverRenderAudio"), bCanRender));
        TestEqual(TEXT("canEverRenderAudio matches the host"), bCanRender, FApp::CanEverRenderAudio());
        PinWrightTestSkip::SkipAssertions(*this, TEXT("suite-runs-nosound"),
            FString::Printf(TEXT("no audio device (canEverRenderAudio=%s), so the fire-and-forget row ")
                TEXT("half was not measured; run with audio enabled."),
                FApp::CanEverRenderAudio() ? TEXT("true") : TEXT("false")));
        return true;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    FAudioDevice* Device = World ? World->GetAudioDeviceRaw() : nullptr;
    if (!TestNotNull(TEXT("editor world exists"), World) || !TestNotNull(TEXT("editor world has an audio device"), Device))
    {
        return false;
    }
    if (!World->bAllowAudioPlayback)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("audio-playback-disallowed"),
            TEXT("the editor world has bAllowAudioPlayback=false, so PlaySoundAtLocation is a no-op here."));
        return true;
    }

    // A fresh USoundWaveProcedural has bLooping=false and Duration=-1, so set bLooping: GetDuration()
    // then reports INDEFINITELY_LOOPING_DURATION, PlaySoundAtLocation skips its audibility cull and the
    // sound cannot finish between the play and the read. No attenuation and no concurrency, so it
    // cannot be virtualized either and the stop below reaches it in the active list.
    TStrongObjectPtr<USoundWaveProcedural> Wave(NewObject<USoundWaveProcedural>(GetTransientPackage()));
    Wave->bLooping = true;
    const FVector Location(321.0, 654.0, 987.0);
    UGameplayStatics::PlaySoundAtLocation(World, Wave.Get(), Location, FRotator::ZeroRotator);

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(MethodName, MakeShared<FJsonObject>(), Capture);
    StopSoundsPlaying(Device, Wave.Get());

    TestTrue(FString::Printf(TEXT("handler succeeded (errorCode='%s')"), *Capture.ErrorCode), Capture.bSuccess);
    const TSharedPtr<FJsonObject> Row = FindRowBySoundPath(Capture.Result, Wave->GetPathName());
    if (!TestTrue(TEXT("the PlaySoundAtLocation sound is listed although no UAudioComponent exists"), Row.IsValid()))
    {
        return true;
    }
    TestTrue(TEXT("fire-and-forget row has a null audioComponentPath"),
        Row->HasTypedField<EJson::Null>(TEXT("audioComponentPath")));
    TestEqual(TEXT("fire-and-forget row has audioComponentId 0"), Row->GetNumberField(TEXT("audioComponentId")), 0.0);
    TestFalse(TEXT("an unattenuated, unlimited sound is in the active list, not virtualized"),
        Row->GetBoolField(TEXT("virtualized")));
    const TSharedPtr<FJsonObject> Loc = Row->GetObjectField(TEXT("location"));
    TestEqual(TEXT("location.x is the play location"), Loc->GetNumberField(TEXT("x")), Location.X);
    TestEqual(TEXT("location.z is the play location"), Loc->GetNumberField(TEXT("z")), Location.Z);
    return true;
}
