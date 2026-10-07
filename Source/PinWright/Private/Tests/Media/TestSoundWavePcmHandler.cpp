// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for audio.authoring.create_sound_wave_from_pcm.
//
// The success case is deliberately not "the handler returned success". What is
// asserted is the decode-back verification block the handler publishes, plus an
// INDEPENDENT decode performed here through the same production helper
// (PwDecodeSoundWave) - so a handler that started reporting its own inputs back
// would still fail this file (rpc-design.md §4, §12).
//
// The signal is asymmetric on purpose: left and right carry different frequencies
// AND different amplitudes, so collapsing the two channels, duplicating one over
// the other, or swapping them all move the per-channel RMS/peak signature far
// outside tolerance. A DC or symmetric test tone would pass all three.

#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestAssetTeardown.h"
#include "Tests/TestUtils.h"

#include "Compat/EngineVersionCompat.h"
#include "Dispatch/SafePoint.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwAudioDecode.h"
#include "AudioGen/PwAudioExport.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundWave.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"

// Named (not anonymous) namespace: the main module builds with Unity on, and two
// anonymous namespaces merged into one TU collide by name. See AGENTS.md > Building.
namespace PwSoundWavePcmTestInternal
{
    constexpr int32 TestSampleRate = 48000;
    constexpr int32 TestFrames = 512;

    // Same tolerance the handler publishes, restated here so a silent widening of the
    // production constant shows up as a test edit rather than as a quietly weaker check.
    constexpr double ExpectedToleranceAbs = 1.0e-3;

    FString MakeUniquePackagePath()
    {
        return FString::Printf(TEXT("/Game/PinWrightTests/SW_FromPcm_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // Asymmetric stereo: 5 cycles at 0.6 on the left, 11 cycles at 0.25 (phase-shifted)
    // on the right. Expected signatures are rmsL ~= 0.424 / peakL ~= 0.6 against
    // rmsR ~= 0.177 / peakR ~= 0.25 - more than two orders of magnitude apart from the
    // 1e-3 tolerance, so any channel confusion is unmissable.
    void BuildTestSignal(TArray<float>& OutLeft, TArray<float>& OutRight)
    {
        OutLeft.SetNumUninitialized(TestFrames);
        OutRight.SetNumUninitialized(TestFrames);
        for (int32 Frame = 0; Frame < TestFrames; ++Frame)
        {
            const float T = static_cast<float>(Frame) / static_cast<float>(TestFrames);
            OutLeft[Frame] = 0.6f * FMath::Sin(2.0f * UE_PI * 5.0f * T);
            OutRight[Frame] = 0.25f * FMath::Sin(2.0f * UE_PI * 11.0f * T + 0.7f);
        }
    }

    TArray<TSharedPtr<FJsonValue>> InterleaveToJson(const TArray<float>& Left, const TArray<float>& Right)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        Values.Reserve(Left.Num() * 2);
        for (int32 Frame = 0; Frame < Left.Num(); ++Frame)
        {
            Values.Add(MakeShared<FJsonValueNumber>(Left[Frame]));
            Values.Add(MakeShared<FJsonValueNumber>(Right[Frame]));
        }
        return Values;
    }

    TSharedPtr<FJsonObject> MakePayload(const FString& PackagePath,
        const TArray<TSharedPtr<FJsonValue>>& Samples, int32 Channels, int32 SampleRate)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), FPackageName::GetLongPackageAssetName(PackagePath));
        Payload->SetStringField(TEXT("path"), FPackageName::GetLongPackagePath(PackagePath));
        Payload->SetArrayField(TEXT("samples"), Samples);
        Payload->SetNumberField(TEXT("sampleRate"), SampleRate);
        Payload->SetNumberField(TEXT("channels"), Channels);
        // save:false everywhere: these fixtures must never leave a .uasset behind.
        Payload->SetBoolField(TEXT("save"), false);
        return Payload;
    }

    FString ToObjectPathForPackage(const FString& PackagePath)
    {
        return FString::Printf(TEXT("%s.%s"), *PackagePath,
            *FPackageName::GetLongPackageAssetName(PackagePath));
    }

    // Shared assertion for the failure-direction tests (rpc-design.md §12): the verb
    // must reject with the given code AND leave no object, no package and no file at the
    // target path. A verb that created the asset and then errored would pass a bare
    // error-code assertion.
    void AssertRejectedAndNothingCreated(FAutomationTestBase& Test, const FString& Label,
        const TSharedPtr<FJsonObject>& Payload, const FString& PackagePath,
        const FString& ExpectedErrorCode)
    {
        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("audio.authoring.create_sound_wave_from_pcm"), Payload, Capture);

        Test.TestTrue(*FString::Printf(TEXT("%s: handler registered"), *Label), bFound);
        Test.TestTrue(*FString::Printf(TEXT("%s: handler responded"), *Label), Capture.bWasCalled);
        Test.TestFalse(*FString::Printf(TEXT("%s: reports failure, not a fake success"), *Label),
            Capture.bSuccess);
        Test.TestEqual(*FString::Printf(TEXT("%s: error code"), *Label),
            Capture.ErrorCode, ExpectedErrorCode);

        Test.TestNull(*FString::Printf(TEXT("%s: no object left at the target path"), *Label),
            StaticFindObject(UObject::StaticClass(), nullptr, *ToObjectPathForPackage(PackagePath)));
        Test.TestNull(*FString::Printf(TEXT("%s: no package left at the target path"), *Label),
            FindPackage(nullptr, *PackagePath));
        Test.TestFalse(*FString::Printf(TEXT("%s: no .uasset on disk"), *Label),
            FPackageName::DoesPackageExist(PackagePath));
    }
}

// =========================================================================
// A. RoundTrip - the decode-back verification is the assertion.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringCreateSoundWaveFromPcmRoundTripTest,
    "PinWright.audio.authoring.create_sound_wave_from_pcm.RoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringCreateSoundWaveFromPcmRoundTripTest::RunTest(const FString& Parameters)
{
    using namespace PwSoundWavePcmTestInternal;

    const FString PackagePath = MakeUniquePackagePath();
    ON_SCOPE_EXIT
    {
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPathForPackage(PackagePath));
    };

    TArray<float> Left;
    TArray<float> Right;
    BuildTestSignal(Left, Right);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("audio.authoring.create_sound_wave_from_pcm"),
        MakePayload(PackagePath, InterleaveToJson(Left, Right), 2, TestSampleRate),
        Capture);

    TestTrue(TEXT("Handler found in registration list"), bFound);
    TestTrue(TEXT("Handler responded"), Capture.bWasCalled);
    TestTrue(*FString::Printf(TEXT("Handler succeeded (errorCode='%s', message='%s')"),
        *Capture.ErrorCode, *Capture.Message), Capture.bSuccess);
    if (!bFound || !Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    FString AssetPath;
    Capture.Result->TryGetStringField(TEXT("assetPath"), AssetPath);
    TestEqual(TEXT("assetPath is the requested object path"),
        AssetPath, ToObjectPathForPackage(PackagePath));

    // Reported shape, all of it read off the decode by the handler.
    int32 Frames = 0;
    Capture.Result->TryGetNumberField(TEXT("frames"), Frames);
    TestEqual(TEXT("frames matches the source buffer"), Frames, TestFrames);

    int32 ReportedRate = 0;
    Capture.Result->TryGetNumberField(TEXT("sampleRate"), ReportedRate);
    TestEqual(TEXT("sampleRate matches the source buffer"), ReportedRate, TestSampleRate);

    int32 ReportedChannels = 0;
    Capture.Result->TryGetNumberField(TEXT("channels"), ReportedChannels);
    TestEqual(TEXT("the created wave is stereo"), ReportedChannels, PwExportChannels);

    double ReportedDuration = 0.0;
    Capture.Result->TryGetNumberField(TEXT("durationSeconds"), ReportedDuration);
    TestTrue(TEXT("durationSeconds is frames/sampleRate"),
        FMath::IsNearlyEqual(ReportedDuration,
            static_cast<double>(TestFrames) / static_cast<double>(TestSampleRate), 1.0e-6));

    // save:false must not claim persistence. Same triple as the save:true family.
    bool bSaveRequested = true;
    bool bSaved = true;
    Capture.Result->TryGetBoolField(TEXT("saveRequested"), bSaveRequested);
    Capture.Result->TryGetBoolField(TEXT("saved"), bSaved);
    TestFalse(TEXT("saveRequested is false for save:false"), bSaveRequested);
    TestFalse(TEXT("saved is false for save:false"), bSaved);
    TestFalse(TEXT("pendingFlush is absent when no save was requested"),
        Capture.Result->HasField(TEXT("pendingFlush")));

    // The verification block - the part that cannot be produced by echoing inputs.
    const TSharedPtr<FJsonObject>* VerificationPtr = nullptr;
    TestTrue(TEXT("response carries a verification block"),
        Capture.Result->TryGetObjectField(TEXT("verification"), VerificationPtr));
    if (!VerificationPtr || !VerificationPtr->IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject>& Verification = *VerificationPtr;

    bool bMeasured = false;
    Verification->TryGetBoolField(TEXT("measured"), bMeasured);
    TestTrue(TEXT("verification.measured"), bMeasured);

    bool bPass = false;
    Verification->TryGetBoolField(TEXT("pass"), bPass);
    TestTrue(TEXT("verification.pass - the decode-back agrees with the source"), bPass);

    double Tolerance = 0.0;
    Verification->TryGetNumberField(TEXT("toleranceAbs"), Tolerance);
    TestEqual(TEXT("verification tolerance is the documented 1e-3"), Tolerance, ExpectedToleranceAbs);

    for (const TCHAR* Flag : { TEXT("framesMatch"), TEXT("sampleRateMatch"),
                               TEXT("channelsMatch"), TEXT("payloadHeaderRead") })
    {
        bool bFlag = false;
        Verification->TryGetBoolField(Flag, bFlag);
        TestTrue(*FString::Printf(TEXT("verification.%s"), Flag), bFlag);
    }

    // The two channels must be measured as DIFFERENT, or a handler that duplicated one
    // channel over the other would still report pass.
    const TSharedPtr<FJsonObject>* LeftBlock = nullptr;
    const TSharedPtr<FJsonObject>* RightBlock = nullptr;
    if (Verification->TryGetObjectField(TEXT("left"), LeftBlock) &&
        Verification->TryGetObjectField(TEXT("right"), RightBlock))
    {
        double LeftRms = 0.0;
        double RightRms = 0.0;
        double LeftRmsDelta = 1.0;
        double RightRmsDelta = 1.0;
        double LeftPeakDelta = 1.0;
        double RightPeakDelta = 1.0;
        (*LeftBlock)->TryGetNumberField(TEXT("decodedRms"), LeftRms);
        (*RightBlock)->TryGetNumberField(TEXT("decodedRms"), RightRms);
        (*LeftBlock)->TryGetNumberField(TEXT("rmsDelta"), LeftRmsDelta);
        (*RightBlock)->TryGetNumberField(TEXT("rmsDelta"), RightRmsDelta);
        (*LeftBlock)->TryGetNumberField(TEXT("peakDelta"), LeftPeakDelta);
        (*RightBlock)->TryGetNumberField(TEXT("peakDelta"), RightPeakDelta);

        TestTrue(TEXT("left RMS round-trips inside tolerance"), LeftRmsDelta <= ExpectedToleranceAbs);
        TestTrue(TEXT("right RMS round-trips inside tolerance"), RightRmsDelta <= ExpectedToleranceAbs);
        TestTrue(TEXT("left peak round-trips inside tolerance"), LeftPeakDelta <= ExpectedToleranceAbs);
        TestTrue(TEXT("right peak round-trips inside tolerance"), RightPeakDelta <= ExpectedToleranceAbs);
        TestTrue(TEXT("the decoded channels are distinct (no collapse or duplication)"),
            FMath::Abs(LeftRms - RightRms) > 0.1);
    }
    else
    {
        AddError(TEXT("verification block is missing its per-channel comparisons"));
    }

    // Independent confirmation from the test side, through the same production decode
    // helper the handler uses (a locally re-implemented check would still pass after
    // reverting the production code).
    USoundWave* Wave = FindObject<USoundWave>(nullptr, *ToObjectPathForPackage(PackagePath));
    TestNotNull(TEXT("the created USoundWave is resolvable by path"), Wave);
    if (Wave)
    {
        FPwAudioBuffer Decoded;
        FString DecodeError;
        const bool bDecoded = PwDecodeSoundWave(Wave, Decoded, DecodeError);
        TestTrue(*FString::Printf(TEXT("the asset decodes independently (%s)"), *DecodeError), bDecoded);
        TestEqual(TEXT("independent decode returns the source frame count"),
            Decoded.NumFrames(), TestFrames);
        TestEqual(TEXT("independent decode returns the source sample rate"),
            Decoded.SampleRate, TestSampleRate);
        if (Decoded.NumFrames() == TestFrames)
        {
            float MaxLeftError = 0.0f;
            float MaxRightError = 0.0f;
            for (int32 Frame = 0; Frame < TestFrames; ++Frame)
            {
                MaxLeftError = FMath::Max(MaxLeftError, FMath::Abs(Decoded.Left[Frame] - Left[Frame]));
                MaxRightError = FMath::Max(MaxRightError, FMath::Abs(Decoded.Right[Frame] - Right[Frame]));
            }
            // Per-sample bound, tighter than the RMS/peak tolerance: one int16 LSB of
            // truncation plus the 32767-vs-32768 scale gap is ~6.2e-5.
            TestTrue(*FString::Printf(TEXT("every left sample round-trips within 1e-4 (worst %g)"),
                MaxLeftError), MaxLeftError < 1.0e-4f);
            TestTrue(*FString::Printf(TEXT("every right sample round-trips within 1e-4 (worst %g)"),
                MaxRightError), MaxRightError < 1.0e-4f);
        }
    }

    return true;
}

// =========================================================================
// B. Idempotent - a retried request converges instead of accumulating.
//    The transport's response-only timeout means a client retry is normal
//    traffic, not an edge case (rpc-design.md §8).
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringCreateSoundWaveFromPcmIdempotentTest,
    "PinWright.audio.authoring.create_sound_wave_from_pcm.Idempotent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringCreateSoundWaveFromPcmIdempotentTest::RunTest(const FString& Parameters)
{
    using namespace PwSoundWavePcmTestInternal;

    const FString PackagePath = MakeUniquePackagePath();
    ON_SCOPE_EXIT
    {
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPathForPackage(PackagePath));
    };

    TArray<float> Left;
    TArray<float> Right;
    BuildTestSignal(Left, Right);
    const TArray<TSharedPtr<FJsonValue>> Samples = InterleaveToJson(Left, Right);

    FTestResponseCapture First;
    InvokeHandlerWithCapture(TEXT("audio.authoring.create_sound_wave_from_pcm"),
        MakePayload(PackagePath, Samples, 2, TestSampleRate), First);
    TestTrue(*FString::Printf(TEXT("first call succeeds (errorCode='%s')"), *First.ErrorCode),
        First.bSuccess);
    if (!First.bSuccess || !First.Result.IsValid())
    {
        return false;
    }

    FString FirstMode;
    bool bFirstExisting = true;
    First.Result->TryGetStringField(TEXT("mode"), FirstMode);
    First.Result->TryGetBoolField(TEXT("existing"), bFirstExisting);
    TestEqual(TEXT("first call reports mode=created"), FirstMode, FString(TEXT("created")));
    TestFalse(TEXT("first call reports existing=false"), bFirstExisting);

    const USoundWave* FirstWave = FindObject<USoundWave>(nullptr, *ToObjectPathForPackage(PackagePath));
    TestNotNull(TEXT("first call produced a resolvable wave"), FirstWave);

    FTestResponseCapture Second;
    InvokeHandlerWithCapture(TEXT("audio.authoring.create_sound_wave_from_pcm"),
        MakePayload(PackagePath, Samples, 2, TestSampleRate), Second);
    TestTrue(*FString::Printf(TEXT("second call succeeds (errorCode='%s')"), *Second.ErrorCode),
        Second.bSuccess);
    if (!Second.bSuccess || !Second.Result.IsValid())
    {
        return false;
    }

    FString SecondMode;
    bool bSecondExisting = false;
    Second.Result->TryGetStringField(TEXT("mode"), SecondMode);
    Second.Result->TryGetBoolField(TEXT("existing"), bSecondExisting);
    TestEqual(TEXT("second call reports mode=updated_in_place"),
        SecondMode, FString(TEXT("updated_in_place")));
    TestTrue(TEXT("second call reports existing=true"), bSecondExisting);

    FString FirstPath;
    FString SecondPath;
    First.Result->TryGetStringField(TEXT("assetPath"), FirstPath);
    Second.Result->TryGetStringField(TEXT("assetPath"), SecondPath);
    TestEqual(TEXT("the retry converged on the same asset path"), SecondPath, FirstPath);

    // The retry must not have produced a second object beside the first: the engine
    // writer's NewObject reconstructs the existing wave in place, so the address is
    // unchanged and there is no "_1" sibling.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    TestSamePtr(TEXT("the retry reused the same USoundWave object"),
        static_cast<const USoundWave*>(
            FindObject<USoundWave>(nullptr, *ToObjectPathForPackage(PackagePath))),
        FirstWave);
#else
    // FAutomationTestBase::TestSamePtr arrived in UE 5.5. The assertion is pointer identity
    // either way.
    TestTrue(TEXT("the retry reused the same USoundWave object"),
        static_cast<const USoundWave*>(
            FindObject<USoundWave>(nullptr, *ToObjectPathForPackage(PackagePath)))
        == FirstWave);
#endif
    TestNull(TEXT("the retry created no deduplicated sibling asset"),
        StaticFindObject(UObject::StaticClass(), nullptr,
            *FString::Printf(TEXT("%s_1.%s_1"), *PackagePath,
                *FPackageName::GetLongPackageAssetName(PackagePath))));

    const TSharedPtr<FJsonObject>* Verification = nullptr;
    if (Second.Result->TryGetObjectField(TEXT("verification"), Verification) && Verification->IsValid())
    {
        bool bPass = false;
        (*Verification)->TryGetBoolField(TEXT("pass"), bPass);
        TestTrue(TEXT("the rewritten asset still verifies against the source"), bPass);
    }
    else
    {
        AddError(TEXT("second call returned no verification block"));
    }

    return true;
}

// =========================================================================
// C. Failure directions (rpc-design.md §12). Each must fail with its OWN code
//    and leave nothing behind - a verb that created the asset and then errored
//    would satisfy a bare error-code assertion.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringCreateSoundWaveFromPcmInvalidPathTest,
    "PinWright.audio.authoring.create_sound_wave_from_pcm.InvalidPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringCreateSoundWaveFromPcmInvalidPathTest::RunTest(const FString& Parameters)
{
    using namespace PwSoundWavePcmTestInternal;

    TArray<float> Left;
    TArray<float> Right;
    BuildTestSignal(Left, Right);

    // Valid samples throughout, so the only thing wrong is the path - otherwise the
    // test would pass for the wrong reason.
    const FString EscapePath = TEXT("/Game/PinWrightTests/../PinWrightEscaped");
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("SW_FromPcm_Escape"));
    Payload->SetStringField(TEXT("path"), EscapePath);
    Payload->SetArrayField(TEXT("samples"), InterleaveToJson(Left, Right));
    Payload->SetNumberField(TEXT("sampleRate"), TestSampleRate);
    Payload->SetNumberField(TEXT("channels"), 2);
    Payload->SetBoolField(TEXT("save"), false);

    AssertRejectedAndNothingCreated(*this, TEXT("InvalidPath"), Payload,
        TEXT("/Game/PinWrightEscaped/SW_FromPcm_Escape"), TEXT("INVALID_PATH"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringCreateSoundWaveFromPcmEmptySamplesTest,
    "PinWright.audio.authoring.create_sound_wave_from_pcm.EmptySamples",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringCreateSoundWaveFromPcmEmptySamplesTest::RunTest(const FString& Parameters)
{
    using namespace PwSoundWavePcmTestInternal;

    // Everything valid except the samples: an empty array must be its own outcome, not
    // a zero-length asset and not a length/channel arithmetic complaint.
    const FString PackagePath = MakeUniquePackagePath();
    AssertRejectedAndNothingCreated(*this, TEXT("EmptySamples"),
        MakePayload(PackagePath, TArray<TSharedPtr<FJsonValue>>(), 2, TestSampleRate),
        PackagePath, TEXT("AUDIO_EMPTY_BUFFER"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringCreateSoundWaveFromPcmChannelMismatchTest,
    "PinWright.audio.authoring.create_sound_wave_from_pcm.ChannelMismatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringCreateSoundWaveFromPcmChannelMismatchTest::RunTest(const FString& Parameters)
{
    using namespace PwSoundWavePcmTestInternal;

    TArray<float> Left;
    TArray<float> Right;
    BuildTestSignal(Left, Right);
    const TArray<TSharedPtr<FJsonValue>> Samples = InterleaveToJson(Left, Right);

    // C1. More channels than the deinterleaved-stereo buffer can hold. Refused rather
    //     than downmixed, which is exactly what FSoundWavePCMWriter would do silently.
    {
        const FString PackagePath = MakeUniquePackagePath();
        AssertRejectedAndNothingCreated(*this, TEXT("ChannelsAboveStereo"),
            MakePayload(PackagePath, Samples, 3, TestSampleRate),
            PackagePath, TEXT("AUDIO_MULTICHANNEL_UNSUPPORTED"));
    }

    // C2. The declared width does not divide the array - an incomplete final frame.
    //     A distinct code from C1 because the caller's fix is different: trim or pad
    //     the array, rather than reduce the channel count.
    {
        const FString PackagePath = MakeUniquePackagePath();
        TArray<TSharedPtr<FJsonValue>> Ragged = Samples;
        Ragged.SetNum(Ragged.Num() - 1);
        AssertRejectedAndNothingCreated(*this, TEXT("RaggedInterleave"),
            MakePayload(PackagePath, Ragged, 2, TestSampleRate),
            PackagePath, TEXT("INVALID_ARGUMENT"));
    }

    return true;
}

// =========================================================================
// D. The verb creates a package and saves it, so it must be gated off
//    UWorld::Tick (rpc-design.md §10). Pinned by name: dropping the entry from
//    Dispatch/SafePoint.cpp re-arms the crash silently.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringCreateSoundWaveFromPcmIsTickGatedTest,
    "PinWright.audio.authoring.create_sound_wave_from_pcm.IsTickGated",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringCreateSoundWaveFromPcmIsTickGatedTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("create_sound_wave_from_pcm is in the tick-unsafe method table"),
        PinWrightSafePoint::IsTickUnsafeMethod(
            TEXT("audio.authoring.create_sound_wave_from_pcm")));
    return true;
}

// =========================================================================
// E. channels:1 writes a MONO wave (board F-audio-no-mono-soundwave-path). It
//    used to be duplicated into a 2-channel payload, so a mono MetaSound paid
//    twice the memory and decode for a channel it discards. Read off the
//    payload header here, not off the response.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringCreateSoundWaveFromPcmMonoTest,
    "PinWright.audio.authoring.create_sound_wave_from_pcm.MonoInputWritesMonoWave",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringCreateSoundWaveFromPcmMonoTest::RunTest(const FString& Parameters)
{
    using namespace PwSoundWavePcmTestInternal;

    const FString PackagePath = MakeUniquePackagePath();
    ON_SCOPE_EXIT
    {
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPathForPackage(PackagePath));
    };

    TArray<float> Left;
    TArray<float> Right;
    BuildTestSignal(Left, Right);
    TArray<TSharedPtr<FJsonValue>> Samples;
    for (const float Sample : Left)
    {
        Samples.Add(MakeShared<FJsonValueNumber>(Sample));
    }

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("audio.authoring.create_sound_wave_from_pcm"),
        MakePayload(PackagePath, Samples, 1, TestSampleRate), Capture);
    TestTrue(*FString::Printf(TEXT("mono create succeeded (errorCode='%s', message='%s')"),
        *Capture.ErrorCode, *Capture.Message), Capture.bSuccess);
    if (!Capture.bSuccess)
    {
        return false;
    }

    int32 ReportedChannels = 0;
    Capture.Result->TryGetNumberField(TEXT("channels"), ReportedChannels);
    TestEqual(TEXT("the response reports a mono wave"), ReportedChannels, 1);

    USoundWave* Wave = Cast<USoundWave>(StaticFindObject(UObject::StaticClass(), nullptr,
        *ToObjectPathForPackage(PackagePath)));
    TestNotNull(TEXT("the wave exists"), Wave);
    if (!Wave)
    {
        return false;
    }

    TArray<uint8> Pcm;
    uint32 Rate = 0;
    uint16 Channels = 0;
    TestTrue(TEXT("the payload header reads"), Wave->GetImportedSoundWaveData(Pcm, Rate, Channels));
    TestEqual(TEXT("the payload carries ONE channel"), static_cast<int32>(Channels), 1);
    TestEqual(TEXT("the payload holds one int16 per frame"), Pcm.Num(),
        TestFrames * static_cast<int32>(sizeof(int16)));

    FPwAudioBuffer Decoded;
    FString DecodeError;
    TestTrue(TEXT("the mono wave decodes"), PwDecodeSoundWave(Wave, Decoded, DecodeError));
    float Worst = 0.0f;
    for (int32 Frame = 0; Frame < FMath::Min(Decoded.NumFrames(), Left.Num()); ++Frame)
    {
        Worst = FMath::Max(Worst, FMath::Abs(Decoded.Left[Frame] - Left[Frame]));
    }
    TestEqual(TEXT("every frame came back"), Decoded.NumFrames(), TestFrames);
    TestTrue(FString::Printf(TEXT("the samples are the input's (worst |delta| %g)"), Worst),
        Worst <= ExpectedToleranceAbs);
    return true;
}

// =========================================================================
// F. audio.authoring.set_sound_wave_gain (board F-rpc-audio-normalize-existing-wave).
//    The point of the verb is what it does NOT change: a level fix through a
//    `sample` layer collapsed every wide wave to mono with pass:true. So the
//    fixtures are an asymmetric stereo wave carrying routing, and a mono wave,
//    and both are read back off the object after the call.
// =========================================================================
namespace PwSoundWaveGainTestInternal
{
    using namespace PwSoundWavePcmTestInternal;

    const TCHAR* const GainMethod = TEXT("audio.authoring.set_sound_wave_gain");

    /** Writes Buffer as a wave at a fresh /Game/PinWrightTests path; returns its object path. */
    FString MakeWave(const FPwAudioBuffer& Buffer, int32 Channels)
    {
        const FString PackagePath = MakeUniquePackagePath();
        FPwSoundWaveWriteReport Report;
        FString Error;
        USoundWave* Wave = PwCreateSoundWaveAsset(Buffer, FPackageName::GetLongPackagePath(PackagePath),
            FPackageName::GetLongPackageAssetName(PackagePath), /*bSaveToDisk=*/false, Report, Error,
            Channels);
        return Wave ? Wave->GetPathName() : FString();
    }

    double Rms(const TArray<float>& Samples)
    {
        double Sum = 0.0;
        for (const float Sample : Samples)
        {
            Sum += static_cast<double>(Sample) * Sample;
        }
        return Samples.Num() > 0 ? FMath::Sqrt(Sum / Samples.Num()) : 0.0;
    }

    TSharedPtr<FJsonObject> GainPayload(const FString& ObjectPath, const TCHAR* Mode, double Target)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ObjectPath);
        Payload->SetStringField(TEXT("mode"), Mode);
        Payload->SetNumberField(TEXT("target"), Target);
        Payload->SetBoolField(TEXT("save"), false);
        return Payload;
    }

    /** Exported text of one reflected USoundWave property; empty when the engine lacks it. */
    FString ExportWaveProperty(const USoundWave* Wave, const TCHAR* PropertyName)
    {
        FString Text;
        if (const FProperty* Property = USoundWave::StaticClass()->FindPropertyByName(PropertyName))
        {
            Property->ExportText_InContainer(0, Text, Wave, /*Delta=*/nullptr,
                const_cast<USoundWave*>(Wave), PPF_None);
        }
        return Text;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringSetSoundWaveGainStereoTest,
    "PinWright.audio.authoring.set_sound_wave_gain.KeepsStereoImageAndRouting",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringSetSoundWaveGainStereoTest::RunTest(const FString& Parameters)
{
    using namespace PwSoundWaveGainTestInternal;

    TestTrue(TEXT("set_sound_wave_gain is tick-unsafe"),
        PinWrightSafePoint::IsTickUnsafeMethod(GainMethod));

    FPwAudioBuffer Source;
    Source.SampleRate = TestSampleRate;
    BuildTestSignal(Source.Left, Source.Right);
    const FString ObjectPath = MakeWave(Source, 2);
    ON_SCOPE_EXIT
    {
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ObjectPath);
    };
    USoundWave* Wave = Cast<USoundWave>(StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath));
    if (!TestNotNull(TEXT("fixture wave created"), Wave))
    {
        return false;
    }

    // A fixture precondition, asserted so a broken fixture does not read as a verb defect.
    FPwAudioBuffer Before;
    FString Error;
    if (!TestTrue(TEXT("fixture decodes"), PwDecodeSoundWave(Wave, Before, Error)))
    {
        return false;
    }
    const double LeftBefore = Rms(Before.Left);
    const double RightBefore = Rms(Before.Right);
    TestTrue(TEXT("fixture sides are distinct"), FMath::Abs(LeftBefore - RightBefore) > 0.1);

    USoundClass* SoundClass = NewObject<USoundClass>(GetTransientPackage());
    Wave->SoundClassObject = SoundClass;
    Wave->bLooping = true;

    // Source markers a gain change leaves valid (same frames, same rate). CuePoints through
    // reflection: it has no public setter before 5.6.
    FSoundWaveCuePoint Cue;
    Cue.CuePointID = 7;
    Cue.Label = TEXT("Hit");
    Cue.FramePosition = Before.NumFrames() / 2;
    if (const FProperty* CuePointsProperty = USoundWave::StaticClass()->FindPropertyByName(TEXT("CuePoints")))
    {
        CuePointsProperty->ContainerPtrToValuePtr<TArray<FSoundWaveCuePoint>>(Wave)->Add(Cue);
    }
    FSoundWaveTimecodeInfo Timecode;
    Timecode.NumSamplesSinceMidnight = static_cast<uint64>(TestSampleRate) * 3600;
    Timecode.NumSamplesPerSecond = TestSampleRate;
    Timecode.Description = TEXT("pinwright-gain-fixture");
    Wave->SetTimecodeInfo(Timecode);
    Wave->SetImportedSampleRate(TestSampleRate);
    const TCHAR* const MarkerProperties[] = {
        TEXT("CuePoints"), TEXT("TimecodeInfo"), TEXT("ImportedSampleRate") };
    TMap<FString, FString> MarkersBefore;
    for (const TCHAR* const Marker : MarkerProperties)
    {
        MarkersBefore.Add(Marker, ExportWaveProperty(Wave, Marker));
    }
    TestTrue(TEXT("fixture carries a cue point"), MarkersBefore[TEXT("CuePoints")].Contains(TEXT("Hit")));
    TestTrue(TEXT("fixture carries a timecode"),
        MarkersBefore[TEXT("TimecodeInfo")].Contains(TEXT("pinwright-gain-fixture")));

    // The fixture peaks at 0.6 (-4.44 dBFS); -12 dBFS is a -7.56 dB change.
    FTestResponseCapture Capture;
    TestTrue(TEXT("handler registered"),
        InvokeHandlerWithCapture(GainMethod, GainPayload(ObjectPath, TEXT("peak"), -12.0), Capture));
    TestTrue(*FString::Printf(TEXT("the gain call succeeded (code='%s', message='%s')"),
        *Capture.ErrorCode, *Capture.Message), Capture.bSuccess);
    if (!Capture.bSuccess)
    {
        return false;
    }

    double GainDb = 0.0;
    Capture.Result->TryGetNumberField(TEXT("gainDb"), GainDb);
    const double ExpectedGain = FMath::Pow(10.0, GainDb / 20.0);
    TestTrue(FString::Printf(TEXT("the reported gain is the peak move (%f dB)"), GainDb),
        FMath::IsNearlyEqual(GainDb, -12.0 - 20.0 * FMath::LogX(10.0, 0.6), 0.05));

    USoundWave* After = Cast<USoundWave>(StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath));
    if (!TestTrue(TEXT("the same object was rewritten"), After == Wave))
    {
        return false;
    }
    TestTrue(TEXT("the sound class survived"), After->SoundClassObject == SoundClass);
    TestTrue(TEXT("bLooping survived"), After->bLooping != 0);
    // A gain change keeps every marker on its sample; the shared rewrite resets them otherwise.
    for (const TCHAR* const Marker : MarkerProperties)
    {
        TestEqual(*FString::Printf(TEXT("%s survived"), Marker), ExportWaveProperty(After, Marker),
            MarkersBefore[Marker]);
    }
    const TSharedPtr<FJsonObject>* GainVerification = nullptr;
    bool bPropertiesPreserved = false;
    TestTrue(TEXT("verification.propertiesPreserved is true"),
        Capture.Result->TryGetObjectField(TEXT("verification"), GainVerification) && GainVerification &&
        (*GainVerification)->TryGetBoolField(TEXT("propertiesPreserved"), bPropertiesPreserved) &&
        bPropertiesPreserved);

    TArray<uint8> Pcm;
    uint32 Rate = 0;
    uint16 Channels = 0;
    After->GetImportedSoundWaveData(Pcm, Rate, Channels);
    TestEqual(TEXT("the wave is still stereo"), static_cast<int32>(Channels), 2);

    FPwAudioBuffer Decoded;
    if (!TestTrue(TEXT("the rewrite decodes"), PwDecodeSoundWave(After, Decoded, Error)))
    {
        return false;
    }
    // Each side moved by the SAME gain - the collapse this verb exists to avoid would have
    // made the two sides equal instead.
    TestTrue(FString::Printf(TEXT("left moved by the gain (%f vs %f)"), Rms(Decoded.Left) / LeftBefore,
        ExpectedGain), FMath::IsNearlyEqual(Rms(Decoded.Left) / LeftBefore, ExpectedGain, 0.01));
    TestTrue(FString::Printf(TEXT("right moved by the gain (%f vs %f)"), Rms(Decoded.Right) / RightBefore,
        ExpectedGain), FMath::IsNearlyEqual(Rms(Decoded.Right) / RightBefore, ExpectedGain, 0.01));

    const TSharedPtr<FJsonObject>* Routing = nullptr;
    FString ReportedClass;
    TestTrue(TEXT("the response carries routing"),
        Capture.Result->TryGetObjectField(TEXT("routing"), Routing) && Routing &&
        (*Routing)->TryGetStringField(TEXT("soundClass"), ReportedClass));
    TestEqual(TEXT("routing names the class that survived"), ReportedClass, SoundClass->GetPathName());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringSetSoundWaveGainMonoTest,
    "PinWright.audio.authoring.set_sound_wave_gain.MonoWaveStaysMono",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringSetSoundWaveGainMonoTest::RunTest(const FString& Parameters)
{
    using namespace PwSoundWaveGainTestInternal;

    FPwAudioBuffer Source;
    Source.SampleRate = TestSampleRate;
    TArray<float> Unused;
    BuildTestSignal(Source.Left, Unused);
    Source.Right = Source.Left;
    const FString ObjectPath = MakeWave(Source, 1);
    ON_SCOPE_EXIT
    {
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ObjectPath);
    };
    USoundWave* Wave = Cast<USoundWave>(StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath));
    if (!TestNotNull(TEXT("fixture wave created"), Wave))
    {
        return false;
    }
    TArray<uint8> Pcm;
    uint32 Rate = 0;
    uint16 Channels = 0;
    Wave->GetImportedSoundWaveData(Pcm, Rate, Channels);
    if (!TestEqual(TEXT("fixture is mono"), static_cast<int32>(Channels), 1))
    {
        return false;
    }

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(GainMethod, GainPayload(ObjectPath, TEXT("peak"), -1.0), Capture);
    TestTrue(*FString::Printf(TEXT("the gain call succeeded (code='%s', message='%s')"),
        *Capture.ErrorCode, *Capture.Message), Capture.bSuccess);

    Wave->GetImportedSoundWaveData(Pcm, Rate, Channels);
    TestEqual(TEXT("the rescaled wave is still mono"), static_cast<int32>(Channels), 1);

    FPwAudioBuffer Decoded;
    FString Error;
    TestTrue(TEXT("the rewrite decodes"), PwDecodeSoundWave(Wave, Decoded, Error));
    double Peak = 0.0;
    for (const float Sample : Decoded.Left)
    {
        Peak = FMath::Max(Peak, FMath::Abs(static_cast<double>(Sample)));
    }
    TestTrue(FString::Printf(TEXT("the decoded peak is -1 dBFS (%f dB)"), 20.0 * FMath::LogX(10.0, Peak)),
        FMath::IsNearlyEqual(20.0 * FMath::LogX(10.0, Peak), -1.0, 0.05));
    return true;
}

// The refusals leave the wave byte-identical: a clamped or partly applied gain would be a
// waveform change reported as a level change.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringSetSoundWaveGainRefusalTest,
    "PinWright.audio.authoring.set_sound_wave_gain.RefusesUnreachableTargets",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringSetSoundWaveGainRefusalTest::RunTest(const FString& Parameters)
{
    using namespace PwSoundWaveGainTestInternal;

    // 0.6 s - long enough for BS.1770 - of a quiet tone with one near-full-scale spike, so any
    // loud LUFS target needs more gain than the spike has headroom for.
    FPwAudioBuffer Source;
    Source.SampleRate = TestSampleRate;
    Source.SetNumFrames(TestSampleRate * 6 / 10);
    for (int32 Frame = 0; Frame < Source.NumFrames(); ++Frame)
    {
        Source.Left[Frame] = 0.01f * FMath::Sin(2.0f * UE_PI * 440.0f * Frame / TestSampleRate);
    }
    Source.Left[Source.NumFrames() / 2] = 0.9f;
    Source.Right = Source.Left;
    const FString ObjectPath = MakeWave(Source, 1);
    ON_SCOPE_EXIT
    {
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ObjectPath);
    };
    USoundWave* Wave = Cast<USoundWave>(StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath));
    if (!TestNotNull(TEXT("fixture wave created"), Wave))
    {
        return false;
    }
    TArray<uint8> PcmBefore;
    uint32 Rate = 0;
    uint16 Channels = 0;
    Wave->GetImportedSoundWaveData(PcmBefore, Rate, Channels);

    const auto ExpectRefused = [&](const TCHAR* Label, const TCHAR* Mode, double Target)
    {
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(GainMethod, GainPayload(ObjectPath, Mode, Target), Capture);
        TestFalse(*FString::Printf(TEXT("%s: refused"), Label), Capture.bSuccess);
        TestEqual(*FString::Printf(TEXT("%s: INVALID_ARGUMENT"), Label), Capture.ErrorCode,
            FString(TEXT("INVALID_ARGUMENT")));
        TArray<uint8> PcmAfter;
        Wave->GetImportedSoundWaveData(PcmAfter, Rate, Channels);
        TestTrue(*FString::Printf(TEXT("%s: payload untouched"), Label), PcmAfter == PcmBefore);
        return Capture;
    };

    // BS.1770 needs Audio::FLKFSAnalyzer, which arrived in UE 5.8 (PwAudioFeatures.cpp); before
    // that a lufs request is refused by the loudness gate, not by the clip guard under test.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
    const FTestResponseCapture Clip = ExpectRefused(TEXT("clipping lufs target"), TEXT("lufs"), -6.0);
    double MaxTarget = 0.0;
    TestTrue(TEXT("the clip refusal names the highest reachable target"),
        Clip.Result.IsValid() && Clip.Result->TryGetNumberField(TEXT("maxTarget"), MaxTarget) &&
        MaxTarget < -6.0);
#else
    PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-has-no-lkfs-analyzer"),
        TEXT("the clipping-lufs case needs Audio::FLKFSAnalyzer (UE 5.8+); the peak and mode "
             "refusals below still run"));
#endif
    ExpectRefused(TEXT("peak above full scale"), TEXT("peak"), 1.0);
    ExpectRefused(TEXT("unknown mode"), TEXT("rms"), -12.0);
    return true;
}
