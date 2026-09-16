// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for audio.authoring.set_sound_wave_properties.
//
// Fixture pattern mirrors TestSoundWaveDescribeHandler.cpp: a transient
// USoundWave under /Game/PinWrightTests/ kept alive via TStrongObjectPtr
// and torn down with ON_SCOPE_EXIT + CleanupTestAsset.

#include "Compat/EngineVersionCompat.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Misc/App.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

#include "Sound/SoundWave.h"
#include "Sound/SoundGroups.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
    // Build a transient USoundWave and fill OutAssetName/OutPackagePath.
    // Returns nullptr on failure (caller already asserted).
    USoundWave* MakeTransientWave(FString& OutPackagePath)
    {
        OutPackagePath = FString::Printf(TEXT("/Game/PinWrightTests/SW_SetProps_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString AssetName = FPackageName::GetLongPackageAssetName(OutPackagePath);
        UPackage* Package = CreatePackage(*OutPackagePath);
        USoundWave* Wave = NewObject<USoundWave>(Package, FName(*AssetName),
            RF_Public | RF_Standalone);
        return Wave;
    }
}

// =========================================================================
// A. Round-trip: flip every supported field, verify the response echoes the
//    new values and the live USoundWave state matches.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringSetSoundWavePropertiesRoundTripTest,
    "PinWright.audio.authoring.set_sound_wave_properties.RoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringSetSoundWavePropertiesRoundTripTest::RunTest(const FString& Parameters)
{
    FString PackagePath;
    USoundWave* Wave = MakeTransientWave(PackagePath);
    TestNotNull(TEXT("Transient SoundWave created"), Wave);
    if (!Wave) return false;
    TStrongObjectPtr<USoundWave> WaveOwner(Wave);

    ON_SCOPE_EXIT
    {
        WaveOwner.Reset();
        CleanupTestAsset(PackagePath);
    };

    // Preconditions — NewObject<USoundWave> leaves these at defaults.
    TestEqual(TEXT("Precondition: bLooping defaults to 0"),
        static_cast<int32>(Wave->bLooping), 0);
    TestTrue(TEXT("Precondition: SoundGroup defaults to SOUNDGROUP_Default"),
        Wave->SoundGroup == SOUNDGROUP_Default);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Wave->GetPathName());
    Payload->SetBoolField(TEXT("bLooping"), true);
    Payload->SetNumberField(TEXT("volume"), 0.75);
    Payload->SetNumberField(TEXT("pitch"), 1.25);
    Payload->SetStringField(TEXT("soundGroup"), TEXT("SOUNDGROUP_Voice"));
    Payload->SetNumberField(TEXT("compressionQuality"), 50);
    Payload->SetBoolField(TEXT("bMature"), true);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("audio.authoring.set_sound_wave_properties"), Payload, Capture);

    TestTrue(TEXT("Handler found in registration list"), bFound);
    TestTrue(TEXT("Handler responded"), Capture.bWasCalled);
    TestTrue(FString::Printf(TEXT("Handler succeeded (errorCode='%s')"), *Capture.ErrorCode),
        Capture.bSuccess);
    TestTrue(TEXT("Result returned"), Capture.Result.IsValid());
    if (!bFound || !Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    // Response echoes the new field values.
    bool RespLooping = false;
    Capture.Result->TryGetBoolField(TEXT("bLooping"), RespLooping);
    TestTrue(TEXT("Response bLooping is true"), RespLooping);

    double RespVolume = 0.0;
    Capture.Result->TryGetNumberField(TEXT("volume"), RespVolume);
    TestEqual(TEXT("Response volume is 0.75"), RespVolume, 0.75);

    double RespPitch = 0.0;
    Capture.Result->TryGetNumberField(TEXT("pitch"), RespPitch);
    TestEqual(TEXT("Response pitch is 1.25"), RespPitch, 1.25);

    FString RespGroup;
    Capture.Result->TryGetStringField(TEXT("soundGroup"), RespGroup);
    TestEqual(TEXT("Response soundGroup is SOUNDGROUP_Voice"),
        RespGroup, FString(TEXT("SOUNDGROUP_Voice")));

    int32 RespQuality = 0;
    Capture.Result->TryGetNumberField(TEXT("compressionQuality"), RespQuality);
    TestEqual(TEXT("Response compressionQuality is 50"), RespQuality, 50);

    bool RespMature = false;
    Capture.Result->TryGetBoolField(TEXT("bMature"), RespMature);
    TestTrue(TEXT("Response bMature is true"), RespMature);

    // Live wave state mirrors the response.
    TestTrue(TEXT("Live: bLooping non-zero"), Wave->bLooping != 0);
    TestEqual(TEXT("Live: Volume is 0.75"), Wave->Volume, 0.75f);
    TestEqual(TEXT("Live: Pitch is 1.25"), Wave->Pitch, 1.25f);
    TestTrue(TEXT("Live: SoundGroup is SOUNDGROUP_Voice"),
        Wave->SoundGroup == SOUNDGROUP_Voice);
    TestEqual(TEXT("Live: CompressionQuality is 50"),
        Wave->GetCompressionQuality(), 50);
    TestTrue(TEXT("Live: bMature non-zero"), Wave->bMature != 0);

    return true;
}

// =========================================================================
// B. ORDINARY PROPERTY PUBLICATION. A non-compression property must publish
// the refreshed working metadata to the runtime proxy.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringSetSoundWavePropertiesPublishesOrdinaryPropertiesTest,
    "PinWright.audio.authoring.set_sound_wave_properties.PublishesOrdinaryProperties",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringSetSoundWavePropertiesPublishesOrdinaryPropertiesTest::RunTest(const FString& Parameters)
{
    FString PackagePath;
    USoundWave* Wave = MakeTransientWave(PackagePath);
    TestNotNull(TEXT("Transient SoundWave created"), Wave);
    if (!Wave) return false;
    TStrongObjectPtr<USoundWave> WaveOwner(Wave);

    ON_SCOPE_EXIT
    {
        WaveOwner.Reset();
        CleanupTestAsset(PackagePath);
    };

    TestFalse(TEXT("Precondition: runtime proxy is not looping"),
        MCP_SOUND_WAVE_DATA(Wave)->IsLooping());

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Wave->GetPathName());
    Payload->SetBoolField(TEXT("bLooping"), true);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("audio.authoring.set_sound_wave_properties"), Payload, Capture);

    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler responded"), Capture.bWasCalled);
    TestTrue(FString::Printf(TEXT("Handler succeeded (errorCode='%s')"), *Capture.ErrorCode),
        Capture.bSuccess);
    TestTrue(TEXT("Result returned"), Capture.Result.IsValid());
    if (!bFound || !Capture.bWasCalled || !Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    bool bResponseLooping = false;
    Capture.Result->TryGetBoolField(TEXT("bLooping"), bResponseLooping);
    TestTrue(TEXT("Response bLooping is true"), bResponseLooping);

    // Proxy publication is unreachable on a host that cannot render audio:
    // USoundWave::UpdatePlatformData returns before Proxy->Publish when
    // FApp::CanEverRenderAudio() is false (SoundWave.cpp:4282-4293), and
    // FSoundWaveProxy::Publish is private with only USoundWave as a friend
    // (SoundWave.h:1922), so no handler code can substitute for it. The
    // compression-quality path publishes regardless because UpdateAsset goes
    // through InvalidateCompressedData (SoundWave.cpp:2184-2185), which is why
    // only this assertion is host-gated.
    if (!FApp::CanEverRenderAudio())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("audio-disabled-host"),
            TEXT("AUDIO-SKIP: host runs with audio disabled (-nosound), so "
                 "USoundWave::UpdatePlatformData skips FSoundWaveProxy publication and the "
                 "runtime FSoundWaveData can never observe an ordinary property edit."));
        return true;
    }

    const auto RuntimeData =
        MCP_SOUND_WAVE_DATA(Wave);
    TestTrue(TEXT("Runtime proxy received the ordinary loop-state update"),
        RuntimeData->IsLooping());

    return true;
}

// =========================================================================
// C. COMPRESSION QUALITY. CompressionQuality must take the engine UpdateAsset
// path, including compressed-data invalidation and proxy publication.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringSetSoundWavePropertiesRefreshesCompressionQualityTest,
    "PinWright.audio.authoring.set_sound_wave_properties.RefreshesCompressionQuality",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringSetSoundWavePropertiesRefreshesCompressionQualityTest::RunTest(const FString& Parameters)
{
    FString PackagePath;
    USoundWave* Wave = MakeTransientWave(PackagePath);
    TestNotNull(TEXT("Transient SoundWave created"), Wave);
    if (!Wave) return false;
    TStrongObjectPtr<USoundWave> WaveOwner(Wave);

    ON_SCOPE_EXIT
    {
        WaveOwner.Reset();
        CleanupTestAsset(PackagePath);
    };

    UPackage* const WavePackage = Wave->GetOutermost();
    const bool bPackageWasDirty = WavePackage && WavePackage->IsDirty();
    const FGuid BeforeGuid = MCP_SOUND_WAVE_GUID(Wave);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Wave->GetPathName());
    Payload->SetNumberField(TEXT("compressionQuality"), 61);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("audio.authoring.set_sound_wave_properties"), Payload, Capture);

    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler responded"), Capture.bWasCalled);
    TestTrue(FString::Printf(TEXT("Handler succeeded (errorCode='%s')"), *Capture.ErrorCode),
        Capture.bSuccess);
    TestTrue(TEXT("Result returned"), Capture.Result.IsValid());
    if (!bFound || !Capture.bWasCalled || !Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    int32 ResponseQuality = 0;
    Capture.Result->TryGetNumberField(TEXT("compressionQuality"), ResponseQuality);
    TestEqual(TEXT("Response compressionQuality is 61"), ResponseQuality, 61);
    if (WavePackage && !bPackageWasDirty)
    {
        TestFalse(TEXT("save=false preserves a clean package"), WavePackage->IsDirty());
    }

    TestTrue(TEXT("Compressed-data GUID changed after compression refresh"),
        MCP_SOUND_WAVE_GUID(Wave) != BeforeGuid);

    return true;
}

// =========================================================================
// D. INVALID_SOUND_GROUP — bad enum name string aborts the edit; the live
//    SoundGroup must stay at its default. (Counterfactual: if the handler
//    silently fell through, this assertion would fail.)
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringSetSoundWavePropertiesInvalidGroupTest,
    "PinWright.audio.authoring.set_sound_wave_properties.InvalidGroup",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringSetSoundWavePropertiesInvalidGroupTest::RunTest(const FString& Parameters)
{
    FString PackagePath;
    USoundWave* Wave = MakeTransientWave(PackagePath);
    TestNotNull(TEXT("Transient SoundWave created"), Wave);
    if (!Wave) return false;
    TStrongObjectPtr<USoundWave> WaveOwner(Wave);

    ON_SCOPE_EXIT
    {
        WaveOwner.Reset();
        CleanupTestAsset(PackagePath);
    };

    TestTrue(TEXT("Precondition: SoundGroup defaults to SOUNDGROUP_Default"),
        Wave->SoundGroup == SOUNDGROUP_Default);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Wave->GetPathName());
    Payload->SetStringField(TEXT("soundGroup"), TEXT("NOT_A_GROUP"));
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("audio.authoring.set_sound_wave_properties"), Payload, Capture);

    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler responded"), Capture.bWasCalled);
    TestFalse(TEXT("Handler reports failure for bogus sound group"), Capture.bSuccess);
    TestEqual(TEXT("ErrorCode is INVALID_SOUND_GROUP"),
        Capture.ErrorCode, FString(TEXT("INVALID_SOUND_GROUP")));
    TestTrue(TEXT("SoundGroup unchanged after rejection"),
        Wave->SoundGroup == SOUNDGROUP_Default);

    return true;
}

// =========================================================================
// E. SOUND_WAVE_NOT_FOUND — unresolved asset path surfaces the right error.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringSetSoundWavePropertiesNotFoundTest,
    "PinWright.audio.authoring.set_sound_wave_properties.NotFound",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringSetSoundWavePropertiesNotFoundTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/DoesNotExist"));
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("audio.authoring.set_sound_wave_properties"), Payload, Capture);

    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler responded"), Capture.bWasCalled);
    TestFalse(TEXT("Handler reports failure for missing asset"), Capture.bSuccess);
    TestEqual(TEXT("ErrorCode is SOUND_WAVE_NOT_FOUND"),
        Capture.ErrorCode, FString(TEXT("SOUND_WAVE_NOT_FOUND")));

    return true;
}
