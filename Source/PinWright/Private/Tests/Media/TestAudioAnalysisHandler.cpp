// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for audio.analysis.analyze / decompose / compare / to_recipe / audit_folder.
//
// The bias is rpc-design.md §12: a test asserting that one of these verbs succeeded proves almost
// nothing, because every defect this family can ship LOOKS like a success. So the assertions that
// carry weight are the ones that break if a verb starts answering "yes":
//
//   - neither source supplied must ERROR, and both supplied must ERROR. There is no safe default
//     for "which sound did you mean", and a verb that guessed would report measurements of audio
//     the caller never asked about while looking exactly right (§3);
//   - an unknown asset path must name the IMPORT remedy, because "there is no file-reading path"
//     is only useful next to the sentence saying what to do instead (§7);
//   - a registry miss must keep the registry's DISTINCT code - CANDIDATE_EVICTED, NO_CANDIDATES
//     and CANDIDATE_NOT_FOUND are three different next moves, and flattening them is what makes
//     an agent "fix" an id it got right;
//   - a procedural and a multichannel SoundWave must keep the DECODER's own codes for the same
//     reason;
//   - compare against silence must FAIL, because a comparison against silence produces no
//     deviations at all, which reads as "these match" - the one false report it must never make;
//   - audit_folder over an empty folder must ERROR, because "0 flagged of 0" is how a typo in a
//     folder path passes for a clean sweep;
//   - a digitally silent asset must produce the silence finding AND NO level findings, because a
//     clipping check awarded a pass over a buffer with no signal is a measurement nobody took;
//   - the tick-unsafe registration is asserted through the shared table, never re-implemented;
//   - the response-size gates hold, so growth trips CI instead of a user's response.
//
// SIZE GATES. The wrapped MCP ToolResult carries the payload twice - escaped in content[0].text
// and verbatim in structuredContent - so the real ceiling for a bare result is about 4,250
// characters (board ticket E-spill-threshold-measured-post-wrap). Measurement uses the PRETTY
// writer, which is what JsonRpc::Serialize and HttpResponseSpill actually take; a condensed
// measurement understates the budget by roughly 20%.
//
// JOB VERBS UNDER TEST. FHandlerContext::StartJob invokes its bind delegate synchronously and the
// test context has no subsystem, so the captured response is the STARTED payload (ticket id,
// cancellable:false) and the terminal result lands in FJobRegistry - the same split
// TestAudioSynthGenerate.cpp asserts against for audio.synth.variations.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwCandidateRegistry.h"
#include "Dispatch/SafePoint.h"
#include "Handlers/ErrorCodes.h"
#include "State/JobRegistry.h"
#include "State/PluginState.h"
#include "Tests/TestAssetTeardown.h"
#include "Tests/TestUtils.h"

#include "Audio.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Compat/EngineVersionCompat.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Math/RandomStream.h"
#include "Memory/SharedBuffer.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sound/SoundWave.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

// Named (not anonymous) namespace: the module builds with bUseUnity = true and the sibling audio
// test TUs already own several anonymous helpers of similar name. See CLAUDE.md > Building.
namespace PwAudioAnalysisHandlerTestHelpers
{
    const TCHAR* const AnalyzeMethod     = TEXT("audio.analysis.analyze");
    const TCHAR* const DecomposeMethod   = TEXT("audio.analysis.decompose");
    const TCHAR* const CompareMethod     = TEXT("audio.analysis.compare");
    const TCHAR* const ToRecipeMethod    = TEXT("audio.analysis.to_recipe");
    const TCHAR* const AuditFolderMethod = TEXT("audio.analysis.audit_folder");

    constexpr int32 TestSampleRate = 48000;

    /** The wrapped-response ceiling every gate in this file holds against. */
    constexpr int32 WrappedResponseCeiling = 4250;

    // ---------------------------------------------------------------------------------------
    // Response plumbing
    // ---------------------------------------------------------------------------------------

    /**
     * Serializes with the SAME writer the transport uses - JsonRpc::Serialize and
     * HttpResponseSpill both take the default TJsonWriterFactory<>, i.e. the PRETTY policy, whose
     * per-field newline, indent and post-colon space add roughly 20% over a condensed encoding.
     */
    int32 MeasureResponseChars(const TSharedPtr<FJsonObject>& Object)
    {
        if (!Object.IsValid())
        {
            return 0;
        }
        FString Text;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
        FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
        return Text.Len();
    }

    FString StringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
    {
        FString Value;
        if (Object.IsValid())
        {
            Object->TryGetStringField(Key, Value);
        }
        return Value;
    }

    bool BoolField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, bool bDefault = false)
    {
        bool Value = bDefault;
        if (Object.IsValid())
        {
            Object->TryGetBoolField(Key, Value);
        }
        return Value;
    }

    int32 NumberField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, int32 Default = -1)
    {
        double Value = 0.0;
        return (Object.IsValid() && Object->TryGetNumberField(Key, Value))
            ? static_cast<int32>(Value) : Default;
    }

    TSharedPtr<FJsonObject> ObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
    {
        const TSharedPtr<FJsonObject>* Found = nullptr;
        if (Object.IsValid() && Object->TryGetObjectField(Key, Found) && Found)
        {
            return *Found;
        }
        return nullptr;
    }

    /** Terminal state of the ticket a job verb returned. */
    bool ReadJobTicket(const FTestResponseCapture& Capture, FJobTicket& Out)
    {
        const FString TicketId = StringField(Capture.Result, TEXT("ticket_id"));
        return !TicketId.IsEmpty() && FPluginState::Get().GetJobRegistry().Get(TicketId, Out);
    }

    // ---------------------------------------------------------------------------------------
    // Buffer fixtures
    // ---------------------------------------------------------------------------------------

    FPwAudioBuffer MakeSilentBuffer(int32 NumFrames = 4800)
    {
        FPwAudioBuffer Buffer;
        Buffer.SampleRate = TestSampleRate;
        Buffer.SetNumFrames(NumFrames, /*bZeroed=*/true);
        return Buffer;
    }

    /**
     * A bank of exponentially decaying sines over a low noise bed.
     *
     * Shaped for the decomposition, not for realism: four clearly separated decaying partials are
     * exactly what PwTrackPartials tracks and PwFitModes fits, and the noise bed is what gives
     * PwAnalyzeResidual something to summarise - so a decomposition of this buffer exercises the
     * modal path AND the residual path rather than half the pipeline.
     */
    FPwAudioBuffer MakeModalBuffer(double DurationMs, int32 Seed = 11)
    {
        const int32 NumFrames =
            FMath::Max(1, static_cast<int32>((DurationMs / 1000.0) * TestSampleRate));

        FPwAudioBuffer Buffer;
        Buffer.SampleRate = TestSampleRate;
        Buffer.SetNumFrames(NumFrames, /*bZeroed=*/true);

        static const double Frequencies[] = { 400.0, 920.0, 1580.0, 2310.0 };
        static const double DecaysMs[]    = { 420.0, 300.0,  210.0,  150.0 };
        static const double Gains[]       = {  0.45,  0.28,   0.16,   0.09 };

        FRandomStream Random(Seed);
        for (int32 Frame = 0; Frame < NumFrames; ++Frame)
        {
            const double TimeSeconds = static_cast<double>(Frame) / TestSampleRate;
            double Value = 0.0;
            for (int32 Mode = 0; Mode < 4; ++Mode)
            {
                const double Decay = FMath::Exp(-6.9078 * (TimeSeconds * 1000.0) / DecaysMs[Mode]);
                Value += Gains[Mode] * Decay
                    * FMath::Sin(2.0 * PI * Frequencies[Mode] * TimeSeconds);
            }
            // A quiet broadband bed, decaying more slowly than the modes so the residual is
            // measurable across the whole clip rather than only under the attack.
            const double NoiseDecay = FMath::Exp(-6.9078 * (TimeSeconds * 1000.0) / 600.0);
            Value += 0.02 * NoiseDecay * static_cast<double>(Random.FRandRange(-1.f, 1.f));

            const float Sample = static_cast<float>(FMath::Clamp(Value, -1.0, 1.0));
            Buffer.Left[Frame] = Sample;
            Buffer.Right[Frame] = Sample;
        }
        return Buffer;
    }

    FPwCandidateRegistry& Registry()
    {
        return FPluginState::Get().GetCandidateRegistry();
    }

    /** Lands a buffer in the session registry and returns its id. */
    FString AddCandidate(FPwAudioBuffer&& Buffer)
    {
        FPwCandidate Candidate;
        Candidate.Buffer = MoveTemp(Buffer);
        return Registry().Add(MoveTemp(Candidate));
    }

    // ---------------------------------------------------------------------------------------
    // SoundWave fixtures
    //
    // Pattern follows Tests/Media/TestPwAudioDecode.cpp (SerializeWaveFile + RawData.UpdatePayload,
    // and USoundWave::NumChannels must be set because GetImportedSoundWaveData asserts
    // check(NumChannels > 0)) but lands the asset under /Game/PinWrightTests/ rather than in the
    // transient package, because these verbs resolve their source BY ASSET PATH.
    // ---------------------------------------------------------------------------------------

    /**
     * A USoundWave at PackagePath carrying InterleavedPcm.
     *
     * bRegisterWithAssetRegistry is what audit_folder needs: a bare CreatePackage + NewObject asset
     * is invisible to FARFilter enumeration until FAssetRegistryModule::AssetCreated announces it,
     * which is exactly what the production create_* handlers do.
     */
    USoundWave* MakeWaveAsset(const FString& PackagePath, const TArray<int16>& InterleavedPcm,
                              int32 NumChannels, bool bRegisterWithAssetRegistry)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        UPackage* Package = CreatePackage(*PackagePath);
        USoundWave* Wave = NewObject<USoundWave>(Package, FName(*AssetName),
                                                 RF_Public | RF_Standalone);
        if (!Wave)
        {
            return nullptr;
        }
        Wave->AddToRoot();
        Wave->NumChannels = NumChannels;

        if (InterleavedPcm.Num() > 0)
        {
            TArray<uint8> WavBytes;
            SerializeWaveFile(WavBytes,
                reinterpret_cast<const uint8*>(InterleavedPcm.GetData()),
                InterleavedPcm.Num() * static_cast<int32>(sizeof(int16)),
                NumChannels, TestSampleRate);
            Wave->RawData.UpdatePayload(FSharedBuffer::Clone(WavBytes.GetData(), WavBytes.Num()));
        }

        if (bRegisterWithAssetRegistry)
        {
            FAssetRegistryModule::AssetCreated(Wave);
        }
        return Wave;
    }

    FString UniquePackagePath(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("/Game/PinWrightTests/%s_%s"), Prefix,
                               *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    /** Mono PCM of a decaying sine, comfortably inside the rails. */
    TArray<int16> MakeCleanPcm(int32 NumFrames)
    {
        TArray<int16> Pcm;
        Pcm.Reserve(NumFrames);
        for (int32 Frame = 0; Frame < NumFrames; ++Frame)
        {
            const double TimeSeconds = static_cast<double>(Frame) / TestSampleRate;
            const double Value = 0.5 * FMath::Exp(-8.0 * TimeSeconds)
                * FMath::Sin(2.0 * PI * 440.0 * TimeSeconds);
            Pcm.Add(static_cast<int16>(FMath::Clamp(FMath::RoundToInt32(Value * 32767.0),
                                                    -32768, 32767)));
        }
        return Pcm;
    }

    /**
     * Mono PCM that clips.
     *
     * The NEGATIVE rail is the one that counts, and that is not an accident of the fixture: the
     * decoder scales by 1/32768, so +32767 comes back as 0.99997 - BELOW
     * PwAudioAnalysisLimits::ClipThreshold (0.999999) - while -32768 comes back as exactly -1.0
     * and clears it. A fixture built to clip only positive would decode as clean audio and the
     * test would assert nothing.
     */
    TArray<int16> MakeClippedPcm(int32 NumFrames)
    {
        TArray<int16> Pcm;
        Pcm.Reserve(NumFrames);
        for (int32 Frame = 0; Frame < NumFrames; ++Frame)
        {
            const double TimeSeconds = static_cast<double>(Frame) / TestSampleRate;
            const double Value = 1.6 * FMath::Sin(2.0 * PI * 220.0 * TimeSeconds);
            Pcm.Add(static_cast<int16>(FMath::Clamp(FMath::RoundToInt32(Value * 32767.0),
                                                    -32768, 32767)));
        }
        return Pcm;
    }

    /** Mono PCM at a constant offset well past the audit's DC threshold. */
    TArray<int16> MakeDcOffsetPcm(int32 NumFrames)
    {
        TArray<int16> Pcm;
        Pcm.Init(static_cast<int16>(FMath::RoundToInt32(0.06 * 32767.0)), NumFrames);
        return Pcm;
    }

    /** Mono PCM of nothing at all. */
    TArray<int16> MakeSilentPcm(int32 NumFrames)
    {
        TArray<int16> Pcm;
        Pcm.Init(0, NumFrames);
        return Pcm;
    }

    /** True when the response carries an `assets` row for AssetPath holding the named check. */
    bool AssetRowHasFinding(const TSharedPtr<FJsonObject>& Result, const FString& AssetPath,
                            const TCHAR* Check)
    {
        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("assets"), Rows) || !Rows)
        {
            return false;
        }
        for (const TSharedPtr<FJsonValue>& RowValue : *Rows)
        {
            const TSharedPtr<FJsonObject> Row = RowValue->AsObject();
            if (!Row.IsValid() || StringField(Row, TEXT("assetPath")) != AssetPath)
            {
                continue;
            }
            const TArray<TSharedPtr<FJsonValue>>* Findings = nullptr;
            if (!Row->TryGetArrayField(TEXT("findings"), Findings) || !Findings)
            {
                return false;
            }
            for (const TSharedPtr<FJsonValue>& FindingValue : *Findings)
            {
                const TSharedPtr<FJsonObject> Finding = FindingValue->AsObject();
                if (Finding.IsValid() && StringField(Finding, TEXT("check")) == Check)
                {
                    return true;
                }
            }
            return false;
        }
        return false;
    }

    /** Number of findings the response's `assets` row for AssetPath carries; -1 when absent. */
    int32 AssetRowFindingCount(const TSharedPtr<FJsonObject>& Result, const FString& AssetPath)
    {
        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("assets"), Rows) || !Rows)
        {
            return -1;
        }
        for (const TSharedPtr<FJsonValue>& RowValue : *Rows)
        {
            const TSharedPtr<FJsonObject> Row = RowValue->AsObject();
            if (!Row.IsValid() || StringField(Row, TEXT("assetPath")) != AssetPath)
            {
                continue;
            }
            const TArray<TSharedPtr<FJsonValue>>* Findings = nullptr;
            return (Row->TryGetArrayField(TEXT("findings"), Findings) && Findings)
                ? Findings->Num() : 0;
        }
        return -1;
    }
}

// =================================================================================================
// A. analyze - §3: neither source supplied is an error naming BOTH ways in.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAnalysisAnalyzeNoSourceTest,
    "PinWright.audio.analysis.analyze.NeitherSourceIsAnError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAnalysisAnalyzeNoSourceTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioAnalysisHandlerTestHelpers;

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler found"),
        InvokeHandlerWithCapture(AnalyzeMethod, MakeShared<FJsonObject>(), Capture));

    // The whole point: an empty payload must NOT quietly measure something.
    TestFalse(TEXT("an empty payload is an error, not a default source"), Capture.bSuccess);
    TestEqual(TEXT("the code is INVALID_PARAMS"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_PARAMS));
    TestTrue(TEXT("the message names the candidate parameter"),
        Capture.Message.Contains(TEXT("candidateId")));
    TestTrue(TEXT("the message names the asset parameter"),
        Capture.Message.Contains(TEXT("assetPath")));
    return true;
}

// =================================================================================================
// B. analyze - §3: two sources is an error, because guessing measures the wrong sound.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAnalysisAnalyzeBothSourcesTest,
    "PinWright.audio.analysis.analyze.BothSourcesIsAnError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAnalysisAnalyzeBothSourcesTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioAnalysisHandlerTestHelpers;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("candidateId"), TEXT("cand-1"));
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Audio/SW_Whatever"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler found"), InvokeHandlerWithCapture(AnalyzeMethod, Payload, Capture));

    TestFalse(TEXT("two sources is an error, not a silent preference for one"), Capture.bSuccess);
    TestEqual(TEXT("the code is INVALID_PARAMS"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_PARAMS));
    TestTrue(TEXT("the message quotes both supplied values"),
        Capture.Message.Contains(TEXT("cand-1")) &&
        Capture.Message.Contains(TEXT("SW_Whatever")));
    return true;
}

// =================================================================================================
// C. analyze - §7: an unknown asset path and a filesystem path both NAME THE WAY OUT.
//    An error that only says "no" is a dead end with extra steps.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAnalysisAnalyzeImportRemedyTest,
    "PinWright.audio.analysis.analyze.MissingSourceNamesTheImportRemedy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAnalysisAnalyzeImportRemedyTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioAnalysisHandlerTestHelpers;

    // 1. A well-formed asset path that names nothing.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"),
            FString::Printf(TEXT("/Game/PinWrightTests/SW_Missing_%s"),
                *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(AnalyzeMethod, Payload, Capture);
        TestFalse(TEXT("an unknown asset path is an error"), Capture.bSuccess);
        TestEqual(TEXT("the code is ASSET_NOT_FOUND"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_ASSET_NOT_FOUND));
        TestTrue(TEXT("the message names importing as the remedy"),
            Capture.Message.Contains(TEXT("mport")));
    }

    // 2. A filesystem path handed to assetPath. Recognised as a file rather than reported as an
    //    invalid asset path, so the caller learns what to do instead of what they did wrong.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), TEXT("D:/audio/impact_metal_01.wav"));

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(AnalyzeMethod, Payload, Capture);
        TestFalse(TEXT("a filesystem path is an error"), Capture.bSuccess);
        TestTrue(TEXT("the message names importing as the remedy"),
            Capture.Message.Contains(TEXT("mport")));
        TestTrue(TEXT("the message names the parameter to pass afterwards"),
            Capture.Message.Contains(TEXT("assetPath")));
    }

    // 3. A `filePath` argument. Answered with the same remedy rather than silently ignored and
    //    reported as "no source supplied", which would point at a parameter the caller did use.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("filePath"), TEXT("/tmp/hit.wav"));

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(AnalyzeMethod, Payload, Capture);
        TestFalse(TEXT("a filePath argument is an error"), Capture.bSuccess);
        TestTrue(TEXT("the message names importing as the remedy"),
            Capture.Message.Contains(TEXT("mport")));
    }
    return true;
}

// =================================================================================================
// D. analyze - §7: registry misses keep their DISTINCT codes.
//    Evicted is forced through the registry's COUNT budget (DefaultMaxCandidates), which is the
//    only budget a test can drive without allocating the byte budget's 256 MB.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAnalysisCandidateMissCodesTest,
    "PinWright.audio.analysis.analyze.CandidateMissKeepsItsCode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAnalysisCandidateMissCodesTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioAnalysisHandlerTestHelpers;

    const auto AnalyzeCandidate = [](const FString& CandidateId, FTestResponseCapture& Capture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("candidateId"), CandidateId);
        InvokeHandlerWithCapture(AnalyzeMethod, Payload, Capture);
    };

    // 1. Unknown id against a non-empty registry -> CANDIDATE_NOT_FOUND, never NO_CANDIDATES.
    const FString LiveId = AddCandidate(MakeModalBuffer(60.0));
    {
        FTestResponseCapture Capture;
        AnalyzeCandidate(TEXT("no-such-candidate-id"), Capture);
        TestFalse(TEXT("an unknown id is an error"), Capture.bSuccess);
        TestEqual(TEXT("an unknown id against a populated registry is CANDIDATE_NOT_FOUND"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_CANDIDATE_NOT_FOUND));
    }

    // 2. Evicted -> CANDIDATE_EVICTED, and NOT flattened into CANDIDATE_NOT_FOUND: the remedies
    //    diverge ("re-render, mind the budget" vs "your id is wrong"), and collapsing them is what
    //    makes an agent "fix" an id it got right.
    {
        // Tiny buffers: this drives the COUNT budget, not the byte budget.
        for (int32 Index = 0; Index <= FPwCandidateRegistry::DefaultMaxCandidates; ++Index)
        {
            FPwAudioBuffer Tiny;
            Tiny.SampleRate = TestSampleRate;
            Tiny.SetNumFrames(8, /*bZeroed=*/true);
            AddCandidate(MoveTemp(Tiny));
        }

        FTestResponseCapture Capture;
        AnalyzeCandidate(LiveId, Capture);
        TestFalse(TEXT("an evicted candidate is an error"), Capture.bSuccess);
        TestEqual(TEXT("an evicted candidate reports CANDIDATE_EVICTED, not a flattened not-found"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_CANDIDATE_EVICTED));
        // The recovery information travels as STRUCTURE, because that is what survives the
        // oversize-spill rewrite (§7).
        TestEqual(TEXT("the payload names the removal status"),
            StringField(Capture.Result, TEXT("status")), FString(TEXT("evicted")));
    }

    // 3. An empty registry -> NO_CANDIDATES, so the caller is told to generate something rather
    //    than sent off checking an id (§7: zero is not a small number).
    {
        Registry().DiscardAll();
        FTestResponseCapture Capture;
        AnalyzeCandidate(TEXT("cand-anything"), Capture);
        TestFalse(TEXT("an id against an empty registry is an error"), Capture.bSuccess);
        TestEqual(TEXT("an empty registry reports NO_CANDIDATES"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_NO_CANDIDATES));
    }
    return true;
}

// =================================================================================================
// E. analyze - §7: the decoder's own codes survive the handler. A procedural wave and a
//    multichannel wave are different problems with different remedies.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAnalysisDecoderCodesTest,
    "PinWright.audio.analysis.analyze.DecoderCodesAreForwarded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAnalysisDecoderCodesTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioAnalysisHandlerTestHelpers;

    const auto AnalyzeAsset = [](const FString& AssetPath, FTestResponseCapture& Capture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        InvokeHandlerWithCapture(AnalyzeMethod, Payload, Capture);
    };

    // 1. Procedural: samples are synthesized at playback time, so there is no payload at all.
    {
        const FString PackagePath = UniquePackagePath(TEXT("SW_Procedural"));
        USoundWave* Wave = MakeWaveAsset(PackagePath, MakeCleanPcm(4800), /*NumChannels=*/1,
                                         /*bRegisterWithAssetRegistry=*/false);
        TestNotNull(TEXT("procedural fixture created"), Wave);
        if (!Wave) return false;
        Wave->bProcedural = true;

        const FString ObjectPath = Wave->GetPathName();
        ON_SCOPE_EXIT
        {
            Wave->RemoveFromRoot();
            PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ObjectPath);
        };

        FTestResponseCapture Capture;
        AnalyzeAsset(ObjectPath, Capture);
        TestFalse(TEXT("a procedural wave is an error"), Capture.bSuccess);
        TestEqual(TEXT("a procedural wave keeps AUDIO_PROCEDURAL_UNSUPPORTED"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_AUDIO_PROCEDURAL_UNSUPPORTED));
    }

    // 2. Multichannel: a non-empty ChannelSizes is the engine's own tell for the >2-channel
    //    layout, where RawData holds N concatenated mono RIFF files.
    {
        const FString PackagePath = UniquePackagePath(TEXT("SW_Multichannel"));
        USoundWave* Wave = MakeWaveAsset(PackagePath, MakeCleanPcm(4800), /*NumChannels=*/6,
                                         /*bRegisterWithAssetRegistry=*/false);
        TestNotNull(TEXT("multichannel fixture created"), Wave);
        if (!Wave) return false;
        Wave->ChannelSizes.Add(64);

        const FString ObjectPath = Wave->GetPathName();
        ON_SCOPE_EXIT
        {
            Wave->RemoveFromRoot();
            PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ObjectPath);
        };

        FTestResponseCapture Capture;
        AnalyzeAsset(ObjectPath, Capture);
        TestFalse(TEXT("a multichannel wave is an error"), Capture.bSuccess);
        TestEqual(TEXT("a multichannel wave keeps AUDIO_MULTICHANNEL_UNSUPPORTED"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_AUDIO_MULTICHANNEL_UNSUPPORTED));
    }
    return true;
}

// =================================================================================================
// F. analyze - §3: `detail` is a closed set. An unrecognised level errors rather than degrading to
//    the summary, which would leave a caller who asked for the series concluding it does not exist.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAnalysisUnknownDetailTest,
    "PinWright.audio.analysis.analyze.UnknownDetailIsAnError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAnalysisUnknownDetailTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioAnalysisHandlerTestHelpers;

    const FString CandidateId = AddCandidate(MakeModalBuffer(120.0));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("candidateId"), CandidateId);
    Payload->SetStringField(TEXT("detail"), TEXT("verbose"));

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(AnalyzeMethod, Payload, Capture);

    TestFalse(TEXT("an unrecognised detail level is an error"), Capture.bSuccess);
    TestEqual(TEXT("the code is INVALID_ARGUMENT"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    TestTrue(TEXT("the message names the closed set"),
        Capture.Message.Contains(TEXT("summary")) && Capture.Message.Contains(TEXT("full")));
    return true;
}

// =================================================================================================
// G. analyze - the happy path plus the size gate. The gate is the assertion that matters: it is
//    what makes payload growth trip CI instead of a user's response.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAnalysisAnalyzeSummaryFitsCeilingTest,
    "PinWright.audio.analysis.analyze.SummaryFitsWrappedCeiling",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAnalysisAnalyzeSummaryFitsCeilingTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioAnalysisHandlerTestHelpers;

    // Long enough that every analysis family measures - the gated loudness meter needs roughly
    // 485 ms at 48 kHz, so a shorter fixture would silently shrink the payload being bounded.
    const FString CandidateId = AddCandidate(MakeModalBuffer(600.0));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("candidateId"), CandidateId);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler found"), InvokeHandlerWithCapture(AnalyzeMethod, Payload, Capture));
    TestTrue(FString::Printf(TEXT("analyze succeeded (errorCode='%s', message='%s')"),
        *Capture.ErrorCode, *Capture.Message), Capture.bSuccess);
    if (!Capture.bSuccess) return false;

    TestEqual(TEXT("the default detail level is the summary"),
        StringField(Capture.Result, TEXT("detail")), FString(TEXT("summary")));
    TestTrue(TEXT("the analysis block is present"),
        ObjectField(Capture.Result, TEXT("analysis")).IsValid());

    const TSharedPtr<FJsonObject> Source = ObjectField(Capture.Result, TEXT("source"));
    TestEqual(TEXT("the source block names the kind it resolved"),
        StringField(Source, TEXT("kind")), FString(TEXT("candidate")));
    TestEqual(TEXT("the source block echoes the candidate id"),
        StringField(Source, TEXT("candidateId")), CandidateId);

    // Nothing may return audio inline, in either direction.
    TestFalse(TEXT("no sample array travels in the response"),
        Capture.Result.IsValid() && (Capture.Result->HasField(TEXT("samples")) ||
                                     Capture.Result->HasField(TEXT("pcm"))));

    const int32 Chars = MeasureResponseChars(Capture.Result);
    AddInfo(FString::Printf(TEXT("analyze summary: %d chars (ceiling %d)"),
        Chars, WrappedResponseCeiling));
    TestTrue(FString::Printf(TEXT("the analyze summary (%d chars) fits the wrapped ceiling (%d)"),
        Chars, WrappedResponseCeiling), Chars <= WrappedResponseCeiling);
    return true;
}

// =================================================================================================
// H. decompose - §9: a ticket that does not claim a cancellation it cannot deliver, and a job
//    result that fits the ceiling.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAnalysisDecomposeJobTest,
    "PinWright.audio.analysis.decompose.TicketIsHonestAndResultFitsCeiling",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAnalysisDecomposeJobTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioAnalysisHandlerTestHelpers;

    const FString CandidateId = AddCandidate(MakeModalBuffer(600.0));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("candidateId"), CandidateId);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler found"), InvokeHandlerWithCapture(DecomposeMethod, Payload, Capture));
    TestTrue(TEXT("the request was accepted"), Capture.bSuccess);

    TestEqual(TEXT("the immediate response is a job ticket"),
        StringField(Capture.Result, TEXT("status")), FString(TEXT("running")));
    TestFalse(TEXT("the ticket carries an id"),
        StringField(Capture.Result, TEXT("ticket_id")).IsEmpty());
    // §1: nothing here registers a cancel callback, so the flag must say so rather than imply a
    // stop that would never happen.
    TestFalse(TEXT("the ticket states it cannot be cancelled"),
        BoolField(Capture.Result, TEXT("cancellable"), true));
    TestTrue(TEXT("the started message names the code job_cancel will return"),
        StringField(Capture.Result, TEXT("message")).Contains(TEXT("JOB_CANCEL_UNSUPPORTED")));

    FJobTicket Ticket;
    TestTrue(TEXT("the ticket resolves in the job registry"), ReadJobTicket(Capture, Ticket));
    TestEqual(FString::Printf(TEXT("the job completed (error='%s')"), *Ticket.Error),
        Ticket.Status, FString(TEXT("completed")));
    if (Ticket.Status != TEXT("completed")) return false;

    const TSharedPtr<FJsonObject> Decomposition =
        ObjectField(Ticket.Result, TEXT("decomposition"));
    TestTrue(TEXT("the job result carries a decomposition"), Decomposition.IsValid());
    TestEqual(TEXT("the default detail level is the summary"),
        StringField(Ticket.Result, TEXT("detail")), FString(TEXT("summary")));

    const int32 Chars = MeasureResponseChars(Ticket.Result);
    AddInfo(FString::Printf(TEXT("decompose summary: %d chars (ceiling %d)"),
        Chars, WrappedResponseCeiling));
    TestTrue(FString::Printf(TEXT("the decompose summary (%d chars) fits the wrapped ceiling (%d)"),
        Chars, WrappedResponseCeiling), Chars <= WrappedResponseCeiling);
    return true;
}

// =================================================================================================
// I. compare - the summary REPORTS what it trimmed, and fits the ceiling. An emptied array with no
//    truncation record would read as "nothing matched", which is a measurement nobody took.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAnalysisCompareSummaryTest,
    "PinWright.audio.analysis.compare.SummaryReportsTrimsAndFitsCeiling",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAnalysisCompareSummaryTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioAnalysisHandlerTestHelpers;

    const FString ReferenceId = AddCandidate(MakeModalBuffer(600.0, /*Seed=*/11));
    const FString CandidateId = AddCandidate(MakeModalBuffer(600.0, /*Seed=*/29));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("referenceCandidateId"), ReferenceId);
    Payload->SetStringField(TEXT("candidateId"), CandidateId);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler found"), InvokeHandlerWithCapture(CompareMethod, Payload, Capture));
    TestTrue(TEXT("the request was accepted"), Capture.bSuccess);
    TestFalse(TEXT("the ticket states it cannot be cancelled"),
        BoolField(Capture.Result, TEXT("cancellable"), true));

    FJobTicket Ticket;
    TestTrue(TEXT("the ticket resolves in the job registry"), ReadJobTicket(Capture, Ticket));
    TestEqual(FString::Printf(TEXT("the job completed (error='%s')"), *Ticket.Error),
        Ticket.Status, FString(TEXT("completed")));
    if (Ticket.Status != TEXT("completed")) return false;

    const TSharedPtr<FJsonObject> Comparison = ObjectField(Ticket.Result, TEXT("comparison"));
    TestTrue(TEXT("the job result carries a comparison"), Comparison.IsValid());
    if (!Comparison.IsValid()) return false;

    const TSharedPtr<FJsonObject> Modes = ObjectField(Comparison, TEXT("modes"));
    TestTrue(TEXT("the comparison carries a modes block"), Modes.IsValid());
    if (Modes.IsValid())
    {
        // The summary form removes the per-mode arrays and REPLACES them with counts. Removed
        // rather than emptied, so an absent array can never be read as "nothing matched".
        TestFalse(TEXT("the summary does not inline the matched-mode array"),
            Modes->HasField(TEXT("matched")));
        TestTrue(TEXT("the summary publishes the matched-mode count instead"),
            Modes->HasField(TEXT("matchedCount")));
        TestTrue(TEXT("the summary publishes the missing-mode count"),
            Modes->HasField(TEXT("missingCount")));
        TestTrue(TEXT("the summary publishes the extra-mode count"),
            Modes->HasField(TEXT("extraCount")));
    }

    // The histogram is over EVERY deviation, so a capped list still reports the whole shape.
    TestTrue(TEXT("the summary carries a diagnosis histogram"),
        Comparison->HasField(TEXT("diagnosisCounts")));
    TestTrue(TEXT("the job result publishes the total deviation count"),
        Ticket.Result.IsValid() && Ticket.Result->HasField(TEXT("deviationCount")));

    const int32 Chars = MeasureResponseChars(Ticket.Result);
    AddInfo(FString::Printf(TEXT("compare summary: %d chars (ceiling %d)"),
        Chars, WrappedResponseCeiling));
    TestTrue(FString::Printf(TEXT("the compare summary (%d chars) fits the wrapped ceiling (%d)"),
        Chars, WrappedResponseCeiling), Chars <= WrappedResponseCeiling);
    return true;
}

// =================================================================================================
// J. compare - §1: a comparison against silence must FAIL. It would otherwise produce no
//    deviations at all, and an empty deviation list is the wire shape of "these match".
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAnalysisCompareSilenceFailsTest,
    "PinWright.audio.analysis.compare.SilenceIsNotAMatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAnalysisCompareSilenceFailsTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioAnalysisHandlerTestHelpers;

    const FString ReferenceId = AddCandidate(MakeModalBuffer(600.0));
    const FString SilentId = AddCandidate(MakeSilentBuffer(28800));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("referenceCandidateId"), ReferenceId);
    Payload->SetStringField(TEXT("candidateId"), SilentId);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler found"), InvokeHandlerWithCapture(CompareMethod, Payload, Capture));

    FJobTicket Ticket;
    TestTrue(TEXT("the ticket resolves in the job registry"), ReadJobTicket(Capture, Ticket));
    TestEqual(TEXT("comparing against silence fails rather than reporting a match"),
        Ticket.Status, FString(TEXT("failed")));
    // A failed job carries only an error STRING on the wire, so the code rides in the result.
    TestEqual(TEXT("the failure carries the code, not just prose"),
        StringField(Ticket.Result, TEXT("errorCode")),
        FString(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER));
    TestFalse(TEXT("no match verdict is published on a failure"),
        Ticket.Result.IsValid() && Ticket.Result->HasField(TEXT("match")));
    return true;
}

// =================================================================================================
// K. compare - §3: the reference side has its own required source, and its absence names its own
//    parameters rather than the candidate side's.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAnalysisCompareNoReferenceTest,
    "PinWright.audio.analysis.compare.MissingReferenceNamesItsOwnParameters",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAnalysisCompareNoReferenceTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioAnalysisHandlerTestHelpers;

    const FString CandidateId = AddCandidate(MakeModalBuffer(120.0));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("candidateId"), CandidateId);

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(CompareMethod, Payload, Capture);

    TestFalse(TEXT("a missing reference is an error, not a comparison against nothing"),
        Capture.bSuccess);
    TestEqual(TEXT("the code is INVALID_PARAMS"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_PARAMS));
    TestTrue(TEXT("the message names the reference parameters"),
        Capture.Message.Contains(TEXT("referenceCandidateId")) &&
        Capture.Message.Contains(TEXT("referenceAssetPath")));
    TestTrue(TEXT("the message says it is the reference that is missing"),
        Capture.Message.Contains(TEXT("reference")));
    return true;
}

// =================================================================================================
// L. to_recipe - the recipe AND the note. The note is what tells the caller what to trust, so its
//    field must be present whether or not the mapping had anything to report.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAnalysisToRecipeTest,
    "PinWright.audio.analysis.to_recipe.ReturnsRecipeAndNote",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAnalysisToRecipeTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioAnalysisHandlerTestHelpers;

    // Short because the pipeline is the expensive part, not because the gate depends on it: the
    // draft's two lists that used to scale with the analysed duration - the modal bank and the
    // residual contour - are capped by TrimRecipeToDraft, so a longer reference returns the same
    // shape with a `truncated` block saying what it dropped. The uncapped draft measured 8,595
    // characters on this very fixture, against a 4,250 ceiling.
    const FString CandidateId = AddCandidate(MakeModalBuffer(250.0));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("candidateId"), CandidateId);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler found"), InvokeHandlerWithCapture(ToRecipeMethod, Payload, Capture));
    TestTrue(TEXT("the request was accepted"), Capture.bSuccess);
    TestFalse(TEXT("the ticket states it cannot be cancelled"),
        BoolField(Capture.Result, TEXT("cancellable"), true));

    FJobTicket Ticket;
    TestTrue(TEXT("the ticket resolves in the job registry"), ReadJobTicket(Capture, Ticket));
    TestEqual(FString::Printf(TEXT("the job completed (error='%s')"), *Ticket.Error),
        Ticket.Status, FString(TEXT("completed")));
    if (Ticket.Status != TEXT("completed")) return false;

    const TSharedPtr<FJsonObject> Recipe = ObjectField(Ticket.Result, TEXT("recipe"));
    TestTrue(TEXT("the job result carries a recipe"), Recipe.IsValid());
    TestTrue(TEXT("the draft has at least one layer"),
        NumberField(Ticket.Result, TEXT("layers"), 0) >= 1);

    // The note is how the caller learns what was dropped, capped or estimated. Always emitted -
    // an absent field would be indistinguishable from a mapping that reported nothing because it
    // never looked.
    TestTrue(TEXT("the note field is always present"),
        Ticket.Result.IsValid() && Ticket.Result->HasField(TEXT("note")));

    // What the draft was fitted from, so a caller can judge how much the mapping had to work with.
    const TSharedPtr<FJsonObject> FittedFrom = ObjectField(Ticket.Result, TEXT("fittedFrom"));
    TestTrue(TEXT("the response says what the draft was fitted from"), FittedFrom.IsValid());

    const int32 Chars = MeasureResponseChars(Ticket.Result);
    AddInfo(FString::Printf(TEXT("to_recipe draft: %d chars (ceiling %d)"),
        Chars, WrappedResponseCeiling));
    TestTrue(FString::Printf(TEXT("the to_recipe draft (%d chars) fits the wrapped ceiling (%d)"),
        Chars, WrappedResponseCeiling), Chars <= WrappedResponseCeiling);
    return true;
}

// =================================================================================================
// M. audit_folder - §8 / §7: an empty match set is an ERROR. "0 flagged of 0" is exactly how a
//    typo in a folder path passes for a clean sweep.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAnalysisAuditEmptyFolderTest,
    "PinWright.audio.analysis.audit_folder.EmptyFolderIsAnError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAnalysisAuditEmptyFolderTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioAnalysisHandlerTestHelpers;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("folder"),
        FString::Printf(TEXT("/Game/PinWrightTests/EmptyAudit_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler found"), InvokeHandlerWithCapture(AuditFolderMethod, Payload, Capture));

    TestFalse(TEXT("an empty folder is an error, not a zero-item success"), Capture.bSuccess);
    TestEqual(TEXT("the code is NO_ASSETS_MATCHED"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_NO_ASSETS_MATCHED));
    // The error must not be a ticket: a caller that got a job would poll for a result that says
    // nothing was wrong.
    TestTrue(TEXT("the rejection is synchronous, not a job ticket"),
        StringField(Capture.Result, TEXT("ticket_id")).IsEmpty());
    return true;
}

// =================================================================================================
// N. audit_folder - §7: a filesystem path is refused with the import remedy rather than swept.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAnalysisAuditFilesystemFolderTest,
    "PinWright.audio.analysis.audit_folder.FilesystemFolderIsAnError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAnalysisAuditFilesystemFolderTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioAnalysisHandlerTestHelpers;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("folder"), TEXT("D:/Projects/Audio/SFX"));

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(AuditFolderMethod, Payload, Capture);

    TestFalse(TEXT("a filesystem folder is an error"), Capture.bSuccess);
    TestEqual(TEXT("the code is INVALID_PATH"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_PATH));
    TestTrue(TEXT("the message names the expected shape"),
        Capture.Message.Contains(TEXT("/Game/Audio")));
    return true;
}

// =================================================================================================
// O. audit_folder - the sweep itself: defects are flagged, a silent asset does NOT collect level
//    findings, an offset past the end errors, and the response fits the ceiling.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAnalysisAuditSweepTest,
    "PinWright.audio.analysis.audit_folder.FlagsDefectsAndFitsCeiling",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAnalysisAuditSweepTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioAnalysisHandlerTestHelpers;

    const FString Folder = FString::Printf(TEXT("/Game/PinWrightTests/Audit_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    constexpr int32 FixtureFrames = 4800;   // 100 ms at 48 kHz

    struct FFixture
    {
        const TCHAR*  Name;
        TArray<int16> Pcm;
    };
    TArray<FFixture> Fixtures;
    Fixtures.Add(FFixture{TEXT("SW_Clean"),   MakeCleanPcm(FixtureFrames)});
    Fixtures.Add(FFixture{TEXT("SW_Clipped"), MakeClippedPcm(FixtureFrames)});
    Fixtures.Add(FFixture{TEXT("SW_Silent"),  MakeSilentPcm(FixtureFrames)});
    Fixtures.Add(FFixture{TEXT("SW_Dc"),      MakeDcOffsetPcm(FixtureFrames)});

    TArray<USoundWave*> Waves;
    TArray<FString> ObjectPaths;
    for (const FFixture& Fixture : Fixtures)
    {
        USoundWave* Wave = MakeWaveAsset(FString::Printf(TEXT("%s/%s"), *Folder, Fixture.Name),
                                         Fixture.Pcm, /*NumChannels=*/1,
                                         /*bRegisterWithAssetRegistry=*/true);
        if (!Wave)
        {
            AddError(FString::Printf(TEXT("could not create fixture %s"), Fixture.Name));
            break;
        }
        Waves.Add(Wave);
        ObjectPaths.Add(Wave->GetPathName());
    }

    ON_SCOPE_EXIT
    {
        for (USoundWave* Wave : Waves)
        {
            Wave->RemoveFromRoot();
        }
        for (const FString& Path : ObjectPaths)
        {
            PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(Path);
        }
    };

    if (Waves.Num() != Fixtures.Num()) return false;

    // Probe the registry directly before asserting anything about the sweep, so a registration
    // failure reads as "the fixtures never became enumerable" rather than as a defect in the verb.
    {
        FAssetRegistryModule& RegistryModule =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        FARFilter Filter;
        Filter.ClassPaths.Add(USoundWave::StaticClass()->GetClassPathName());
        Filter.bRecursiveClasses = true;
        Filter.bRecursivePaths = true;
        Filter.PackagePaths.Add(FName(*Folder));
        TArray<FAssetData> Found;
        RegistryModule.Get().GetAssets(Filter, Found);
        if (Found.Num() != Fixtures.Num())
        {
            AddError(FString::Printf(
                TEXT("the asset registry enumerated %d of %d fixtures under '%s'; ")
                TEXT("FAssetRegistryModule::AssetCreated did not make the in-memory assets ")
                TEXT("visible, so the sweep assertions below would test the wrong thing"),
                Found.Num(), Fixtures.Num(), *Folder));
            return false;
        }
    }

    // ---- the sweep -------------------------------------------------------------------------
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("folder"), Folder);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler found"), InvokeHandlerWithCapture(AuditFolderMethod, Payload, Capture));
    TestTrue(TEXT("the sweep was accepted"), Capture.bSuccess);
    TestFalse(TEXT("the ticket states it cannot be cancelled"),
        BoolField(Capture.Result, TEXT("cancellable"), true));

    FJobTicket Ticket;
    TestTrue(TEXT("the ticket resolves in the job registry"), ReadJobTicket(Capture, Ticket));
    TestEqual(FString::Printf(TEXT("the job completed (error='%s')"), *Ticket.Error),
        Ticket.Status, FString(TEXT("completed")));
    if (Ticket.Status != TEXT("completed")) return false;

    TestEqual(TEXT("every fixture was matched"),
        NumberField(Ticket.Result, TEXT("total")), Fixtures.Num());
    TestEqual(TEXT("every matched fixture was examined"),
        NumberField(Ticket.Result, TEXT("examined")), Fixtures.Num());
    TestFalse(TEXT("a single page carries no nextOffset"),
        Ticket.Result.IsValid() && Ticket.Result->HasField(TEXT("nextOffset")));

    const TSharedPtr<FJsonObject> Counts = ObjectField(Ticket.Result, TEXT("findingCounts"));
    TestTrue(TEXT("the histogram is present"), Counts.IsValid());
    if (Counts.IsValid())
    {
        // Every token is published, zeros included: a zero is the measurement "this check ran
        // over the page and found nothing", which an omitted key could not say.
        TestTrue(TEXT("clipping was found"), NumberField(Counts, TEXT("clipping"), 0) >= 1);
        TestTrue(TEXT("digital silence was found"),
            NumberField(Counts, TEXT("digital_silence"), 0) >= 1);
        TestTrue(TEXT("the DC offset was found"), NumberField(Counts, TEXT("dc_offset"), 0) >= 1);
        // One format across the whole page, so no format outlier may be claimed.
        TestEqual(TEXT("no sample-rate outlier is invented on a uniform page"),
            NumberField(Counts, TEXT("sample_rate_outlier"), -1), 0);
        TestEqual(TEXT("no channel-count outlier is invented on a uniform page"),
            NumberField(Counts, TEXT("channel_count_outlier"), -1), 0);
    }

    TestEqual(TEXT("three of the four fixtures are flagged"),
        NumberField(Ticket.Result, TEXT("flagged")), 3);
    TestEqual(TEXT("the clean fixture is counted clean"),
        NumberField(Ticket.Result, TEXT("clean")), 1);

    // §7 ordering at the handler layer: a silent asset carries the silence finding and NOTHING
    // else. Awarding it a clipping or DC pass would be a check reported over a buffer that has no
    // signal to check.
    const FString SilentPath = ObjectPaths[2];
    TestTrue(TEXT("the silent fixture carries the silence finding"),
        AssetRowHasFinding(Ticket.Result, SilentPath, TEXT("digital_silence")));
    TestEqual(TEXT("the silent fixture collects no level findings beside it"),
        AssetRowFindingCount(Ticket.Result, SilentPath), 1);

    // Coverage is published rather than implied: these short fixtures have no integrated
    // loudness, so a zero loudness-outlier count must not read as "every asset was checked".
    const TSharedPtr<FJsonObject> Loudness = ObjectField(Ticket.Result, TEXT("loudness"));
    TestTrue(TEXT("the loudness coverage is published"),
        Loudness.IsValid() && Loudness->HasField(TEXT("measured")));

    const int32 Chars = MeasureResponseChars(Ticket.Result);
    AddInfo(FString::Printf(TEXT("audit_folder page: %d chars (ceiling %d)"),
        Chars, WrappedResponseCeiling));
    TestTrue(FString::Printf(TEXT("the audit page (%d chars) fits the wrapped ceiling (%d)"),
        Chars, WrappedResponseCeiling), Chars <= WrappedResponseCeiling);

    // ---- an offset past the end -------------------------------------------------------------
    // Not a zero-item success either: an empty page reported as a clean sweep is the same false
    // report as an empty folder reported as one.
    {
        TSharedPtr<FJsonObject> PastEnd = MakeShared<FJsonObject>();
        PastEnd->SetStringField(TEXT("folder"), Folder);
        PastEnd->SetNumberField(TEXT("offset"), 99);

        FTestResponseCapture OffsetCapture;
        InvokeHandlerWithCapture(AuditFolderMethod, PastEnd, OffsetCapture);
        TestFalse(TEXT("an offset past the last asset is an error"), OffsetCapture.bSuccess);
        TestEqual(TEXT("the code is INVALID_PARAMS"),
            OffsetCapture.ErrorCode, FString(ErrorCodes::ERR_INVALID_PARAMS));
    }
    return true;
}

// =================================================================================================
// P. §10: every verb here is gated through the SHARED table, never a hand-written gate.
//    All five can be pointed at an assetPath, and that path runs a decode whose payload read
//    blocks on a bulk-data future - the same hazard audio.synth.export is listed for.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAnalysisGatedAsTickUnsafeTest,
    "PinWright.audio.analysis.SafePoint.EveryVerbIsGated",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAnalysisGatedAsTickUnsafeTest::RunTest(const FString& Parameters)
{
    using namespace PwAudioAnalysisHandlerTestHelpers;

    const TArray<const TCHAR*> Methods = {
        AnalyzeMethod, DecomposeMethod, CompareMethod, ToRecipeMethod, AuditFolderMethod
    };
    for (const TCHAR* Method : Methods)
    {
        TestTrue(FString::Printf(TEXT("%s is in the tick-unsafe method table"), Method),
            PinWrightSafePoint::IsTickUnsafeMethod(FString(Method)));
        // Spelled the same as the registered handler: a typo in the table fails silently, which
        // is exactly the failure this second assertion exists to catch.
        TestTrue(FString::Printf(TEXT("the %s table entry matches the registered name"), Method),
            PinWrightSafePoint::GetTickUnsafeMethods().Contains(FString(Method)));
    }
    return true;
}
