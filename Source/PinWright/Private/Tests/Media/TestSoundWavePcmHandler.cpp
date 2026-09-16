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
#include "Sound/SoundWave.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"

// Named (not anonymous) namespace: the main module builds with Unity on, and two
// anonymous namespaces merged into one TU collide by name. See CLAUDE.md > Building.
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
