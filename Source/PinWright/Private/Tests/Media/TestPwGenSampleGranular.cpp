// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the source-reading generators and the shared asset loader in
// AudioGen/PwGenSampleGranular.cpp: PwGenSample, PwGenGranular, PwResolveSourceBuffer.
//
// The assertions are numeric, not "it returned true". A sine fixture has a known frequency, so
// the sample player's transposition is checked against a spectrum rather than against itself, and
// the sample-rate-mismatch case asserts BOTH that the pitch is right AND that it is not the wrong
// pitch the un-converted read would produce - the defect that otherwise reads as "the sample is
// slightly off" instead of as a bug.
//
// Every rejection case (rpc-design.md §12) checks the SPECIFIC error code and that a pre-filled
// sentinel buffer came back untouched, so a regression that half-fills the output before bailing
// breaks the test instead of sliding past it.
//
// Fixture pattern follows TestPwAudioDecode.cpp: a transient USoundWave built with the engine's
// own SerializeWaveFile + RawData.UpdatePayload, kept alive with AddToRoot and released in
// ON_SCOPE_EXIT. USoundWave::NumChannels must be set because GetImportedSoundWaveData asserts
// check(NumChannels > 0). The waves are named with a GUID and live in the transient package, so
// their asset path is /Engine/Transient.<name> - which PwResolveSourceBuffer reaches through its
// FindObject-before-load step, exactly as it reaches an already-loaded project asset.

#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwSeededRandom.h"
#include "AudioGen/PwStft.h"
#include "AudioGen/PwSynthDsp.h"
#include "AudioGen/PwSynthRecipe.h"
#include "Handlers/ErrorCodes.h"

#include "Audio.h"
#include "Memory/SharedBuffer.h"
#include "Sound/SoundWave.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

// Named namespace, not anonymous: Tests/Media/ already carries anonymous-namespace helpers
// (TestSoundWaveAuthoringHandler.cpp's MakeTransientWave) and the module builds with
// bUseUnity = true, which would merge the TUs. See CLAUDE.md > Building.
namespace PwGenSourceTest
{
    constexpr int32 RenderRate = 48000;

    /** Sentinel written into an output buffer before a rejection call, to prove it stays untouched. */
    constexpr float Sentinel = 1234.5f;

    TArray<int16> MakeSinePcm(int32 SampleRate, int32 NumFrames, double Hz, double Amplitude)
    {
        TArray<int16> Pcm;
        Pcm.SetNumUninitialized(NumFrames);
        const double AngularStep = 2.0 * UE_DOUBLE_PI * Hz / static_cast<double>(SampleRate);
        for (int32 Frame = 0; Frame < NumFrames; ++Frame)
        {
            const double Value = Amplitude * FMath::Sin(AngularStep * Frame);
            Pcm[Frame] = static_cast<int16>(FMath::Clamp(FMath::RoundToInt32(Value * 32767.0), -32768, 32767));
        }
        return Pcm;
    }

    /** Transient mono USoundWave carrying a sine. Caller owns RemoveFromRoot. */
    USoundWave* MakeSineWave(int32 SampleRate, int32 NumFrames, double Hz, double Amplitude)
    {
        const FString Name = FString::Printf(TEXT("PwGenSourceFixture_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));

        USoundWave* Wave = NewObject<USoundWave>(GetTransientPackage(), FName(*Name), RF_Transient);
        if (!Wave)
        {
            return nullptr;
        }
        Wave->AddToRoot();
        Wave->NumChannels = 1;

        const TArray<int16> Pcm = MakeSinePcm(SampleRate, NumFrames, Hz, Amplitude);

        TArray<uint8> WavBytes;
        SerializeWaveFile(WavBytes,
            reinterpret_cast<const uint8*>(Pcm.GetData()),
            Pcm.Num() * static_cast<int32>(sizeof(int16)),
            /*NumChannels=*/1, SampleRate);

        Wave->RawData.UpdatePayload(FSharedBuffer::Clone(WavBytes.GetData(), WavBytes.Num()));
        return Wave;
    }

    // ---------------------------------------------------------------------
    // Parameter bags. Built by hand rather than through ParseSynthRecipe, which also exercises
    // the generators' spec-table default fallback: only the keys a test cares about are present.
    // ---------------------------------------------------------------------

    void SetNum(FPwSynthParams& Params, const TCHAR* Key, double Value)
    {
        FPwSynthParamValue Entry;
        Entry.Type = EPwSynthParamType::Number;
        Entry.Number = Value;
        Params.Values.Add(FName(Key), Entry);
    }

    void SetStr(FPwSynthParams& Params, const TCHAR* Key, const FString& Value)
    {
        FPwSynthParamValue Entry;
        Entry.Type = EPwSynthParamType::String;
        Entry.String = Value;
        Params.Values.Add(FName(Key), Entry);
    }

    void SetBool(FPwSynthParams& Params, const TCHAR* Key, bool bValue)
    {
        FPwSynthParamValue Entry;
        Entry.Type = EPwSynthParamType::Boolean;
        Entry.bBool = bValue;
        Entry.Number = bValue ? 1.0 : 0.0;
        Params.Values.Add(FName(Key), Entry);
    }

    FPwSynthParams SampleParams(const FString& SourcePath, double PlaybackRate, bool bLoop,
        double SourceStartMs = 0.0)
    {
        FPwSynthParams Params;
        SetStr(Params, TEXT("sourcePath"), SourcePath);
        SetNum(Params, TEXT("sourceStartMs"), SourceStartMs);
        SetNum(Params, TEXT("playbackRate"), PlaybackRate);
        SetBool(Params, TEXT("loop"), bLoop);
        return Params;
    }

    FPwSynthParams GranularParams(const FString& SourcePath, double GrainMs, double DensityHz)
    {
        FPwSynthParams Params;
        SetStr(Params, TEXT("sourcePath"), SourcePath);
        SetNum(Params, TEXT("grainMs"), GrainMs);
        SetNum(Params, TEXT("densityHz"), DensityHz);
        return Params;
    }

    // ---------------------------------------------------------------------
    // Runners
    // ---------------------------------------------------------------------

    bool RunSample(const FPwSynthParams& Params, int32 Seed, TArray<float>& Out,
        FString& OutErrorCode, FString& OutError)
    {
        const TArray<FPwSynthPitchPoint> NoPitch;
        const FPwSynthModulation NoModulation;
        FPwSeededRandom Rng(Seed);
        return PwGenSample(Params, NoPitch, NoModulation, RenderRate, Rng,
            TArrayView<float>(Out), OutErrorCode, OutError);
    }

    bool RunGranular(const FPwSynthParams& Params, int32 Seed, TArray<float>& Out,
        FString& OutErrorCode, FString& OutError)
    {
        const TArray<FPwSynthPitchPoint> NoPitch;
        const FPwSynthModulation NoModulation;
        FPwSeededRandom Rng(Seed);
        return PwGenGranular(Params, NoPitch, NoModulation, RenderRate, Rng,
            TArrayView<float>(Out), OutErrorCode, OutError);
    }

    TArray<float> MakeSentinelBuffer(int32 NumSamples)
    {
        TArray<float> Buffer;
        Buffer.Init(Sentinel, NumSamples);
        return Buffer;
    }

    bool IsAllSentinel(const TArray<float>& Buffer)
    {
        for (const float Value : Buffer)
        {
            if (Value != Sentinel)
            {
                return false;
            }
        }
        return true;
    }

    // ---------------------------------------------------------------------
    // Measurement
    // ---------------------------------------------------------------------

    /**
     * Peak bin of the time-averaged magnitude spectrum, in Hz. Returns a negative value when the
     * STFT itself refused (too short, silent), so a caller's frequency assertion fails loudly
     * instead of comparing against a fabricated zero.
     */
    float DominantHz(const TArray<float>& Mono, int32 SampleRate)
    {
        FPwStftSettings Settings;
        Settings.FftSize = 8192;
        Settings.HopSize = 2048;
        Settings.Window = Audio::EWindowType::Hann;

        FPwStftResult Result;
        FPwStftError Error;
        if (!PwComputeStft(TArrayView<const float>(Mono), SampleRate, Settings, Result, &Error))
        {
            return -1.f;
        }

        TArray<float> Averaged;
        Averaged.SetNumZeroed(Result.NumBins);
        for (int32 Frame = 0; Frame < Result.NumFrames; ++Frame)
        {
            for (int32 Bin = 0; Bin < Result.NumBins; ++Bin)
            {
                Averaged[Bin] += PwStftMagnitudeAt(Result, Frame, Bin);
            }
        }

        int32 BestBin = 0;
        for (int32 Bin = 1; Bin < Averaged.Num(); ++Bin)
        {
            if (Averaged[Bin] > Averaged[BestBin])
            {
                BestBin = Bin;
            }
        }
        return BestBin * Result.BinHz;
    }

    /** 8192-point analysis at 48 kHz puts a bin every 5.86 Hz; 15 Hz is 2.5 bins of slack. */
    constexpr float FrequencyToleranceHz = 15.f;

    bool AllFinite(const TArray<float>& Buffer)
    {
        for (const float Value : Buffer)
        {
            if (!FMath::IsFinite(Value))
            {
                return false;
            }
        }
        return true;
    }

    float PeakAbs(const TArray<float>& Buffer, int32 First = 0, int32 Last = MAX_int32)
    {
        float Peak = 0.f;
        // Last is INCLUSIVE, and its default of MAX_int32 makes the obvious "Last + 1" overflow
        // to INT_MIN: the clamped end goes negative, the loop body never runs, and the helper
        // reports 0 for a buffer full of signal. Widen to int64 before the increment; the clamp
        // against Buffer.Num() brings the result back into int32 range.
        const int32 End = static_cast<int32>(
            FMath::Min<int64>(static_cast<int64>(Last) + 1, Buffer.Num()));
        for (int32 Index = FMath::Max(0, First); Index < End; ++Index)
        {
            Peak = FMath::Max(Peak, FMath::Abs(Buffer[Index]));
        }
        return Peak;
    }

    /** Largest sample-to-sample jump. The click a missing grain window produces shows up here. */
    float MaxStep(const TArray<float>& Buffer)
    {
        float Worst = 0.f;
        for (int32 Index = 1; Index < Buffer.Num(); ++Index)
        {
            Worst = FMath::Max(Worst, FMath::Abs(Buffer[Index] - Buffer[Index - 1]));
        }
        return Worst;
    }

    bool BitwiseEqual(const TArray<float>& A, const TArray<float>& B)
    {
        return A.Num() == B.Num()
            && FMemory::Memcmp(A.GetData(), B.GetData(), A.Num() * sizeof(float)) == 0;
    }

    bool RangeBitwiseEqual(const TArray<float>& A, const TArray<float>& B, int32 Count)
    {
        return A.Num() >= Count && B.Num() >= Count
            && FMemory::Memcmp(A.GetData(), B.GetData(), Count * sizeof(float)) == 0;
    }
}

// =========================================================================
// A. sample @ rate 1.0 reproduces the source frequency, and the source's own
//    sample rate is honoured rather than assumed.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenSampleUnityRateKeepsPitchTest,
    "PinWright.audio.gen.sample.UnityRateKeepsPitch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenSampleUnityRateKeepsPitchTest::RunTest(const FString& Parameters)
{
    using namespace PwGenSourceTest;

    USoundWave* Wave = MakeSineWave(RenderRate, RenderRate, /*Hz=*/440.0, /*Amplitude=*/0.9);
    TestNotNull(TEXT("Fixture wave created"), Wave);
    if (!Wave) return false;
    ON_SCOPE_EXIT { Wave->RemoveFromRoot(); };

    TArray<float> Out;
    Out.SetNumZeroed(RenderRate / 2);

    FString Code;
    FString Error;
    const bool bOk = RunSample(SampleParams(Wave->GetPathName(), /*PlaybackRate=*/1.0, /*bLoop=*/false),
        /*Seed=*/7, Out, Code, Error);

    TestTrue(FString::Printf(TEXT("Generated (code='%s', error='%s')"), *Code, *Error), bOk);
    if (!bOk) return false;

    TestTrue(TEXT("Output is finite"), AllFinite(Out));
    TestTrue(TEXT("Output is not silent"), PeakAbs(Out) > 0.5f);
    TestEqual(TEXT("Playback at rate 1.0 keeps the source frequency"),
        DominantHz(Out, RenderRate), 440.f, FrequencyToleranceHz);

    return true;
}

// =========================================================================
// B. sample @ rate 2.0 transposes an octave up. playbackRate is documented as
//    "resample ratio; also shifts pitch", so this is the contract, not a bug.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenSampleDoubleRateIsAnOctaveUpTest,
    "PinWright.audio.gen.sample.DoubleRateIsAnOctaveUp",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenSampleDoubleRateIsAnOctaveUpTest::RunTest(const FString& Parameters)
{
    using namespace PwGenSourceTest;

    USoundWave* Wave = MakeSineWave(RenderRate, RenderRate, /*Hz=*/440.0, /*Amplitude=*/0.9);
    TestNotNull(TEXT("Fixture wave created"), Wave);
    if (!Wave) return false;
    ON_SCOPE_EXIT { Wave->RemoveFromRoot(); };

    TArray<float> Out;
    Out.SetNumZeroed(RenderRate / 4);

    FString Code;
    FString Error;
    const bool bOk = RunSample(SampleParams(Wave->GetPathName(), /*PlaybackRate=*/2.0, /*bLoop=*/false),
        /*Seed=*/7, Out, Code, Error);

    TestTrue(FString::Printf(TEXT("Generated (code='%s', error='%s')"), *Code, *Error), bOk);
    if (!bOk) return false;

    TestTrue(TEXT("Output is finite"), AllFinite(Out));
    TestEqual(TEXT("Rate 2.0 doubles the frequency"),
        DominantHz(Out, RenderRate), 880.f, FrequencyToleranceHz);

    return true;
}

// =========================================================================
// C. THE MISMATCH TEST for `sample`. A source recorded at a rate other than
//    the render rate must be resampled, not replayed frame-for-frame.
//
//    Why this class of bug matters more than "the pitch is a bit off":
//    PwResolveSourceBuffer hands back the ASSET's native rate, and
//    FPwAudioBuffer::MixInto refuses a rate mismatch outright - it mixes
//    nothing and returns 0. A generator that forwarded the native rate instead
//    of converting would therefore produce an inaudible layer with no error
//    anywhere (rpc-design.md §1), and one that converted wrongly would produce
//    a transposed one. Both directions are asserted (§6).
//
//    44.1 kHz into 48 kHz on purpose: the ratio 0.91875 is not a clean
//    fraction, so it exercises real fractional interpolation rather than the
//    exact half-sample positions a 2:1 fixture would only ever land on.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenSampleResamplesRateMismatchTest,
    "PinWright.audio.gen.sample.ResamplesRateMismatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenSampleResamplesRateMismatchTest::RunTest(const FString& Parameters)
{
    using namespace PwGenSourceTest;

    constexpr int32 SourceRate = 44100;

    // 440 Hz read one frame per output sample instead of 0.91875 would come out here.
    constexpr float UnconvertedHz = 440.f * 48000.f / 44100.f;    // 478.9

    USoundWave* Wave = MakeSineWave(SourceRate, SourceRate, /*Hz=*/440.0, /*Amplitude=*/0.9);
    TestNotNull(TEXT("Fixture wave created"), Wave);
    if (!Wave) return false;
    ON_SCOPE_EXIT { Wave->RemoveFromRoot(); };

    TArray<float> Out;
    Out.SetNumZeroed(RenderRate / 2);

    FString Code;
    FString Error;
    const bool bOk = RunSample(SampleParams(Wave->GetPathName(), /*PlaybackRate=*/1.0, /*bLoop=*/false),
        /*Seed=*/7, Out, Code, Error);

    TestTrue(FString::Printf(TEXT("Generated (code='%s', error='%s')"), *Code, *Error), bOk);
    if (!bOk) return false;

    TestTrue(TEXT("The layer is audible, not the silence a refused mix would leave"),
        PeakAbs(Out) > 0.5f);

    const float Measured = DominantHz(Out, RenderRate);
    TestEqual(TEXT("A 44.1 kHz source rendered at 48 kHz still comes out at 440 Hz"),
        Measured, 440.f, FrequencyToleranceHz);
    TestTrue(FString::Printf(TEXT("It is not the %.1f Hz an un-resampled read would produce (got %.1f)"),
            UnconvertedHz, Measured),
        FMath::Abs(Measured - UnconvertedHz) > 20.f);

    return true;
}

// =========================================================================
// C2. THE SAME MISMATCH GUARD for `granular`. Each grain reads from the source
//     and must land at the render rate, so the source/render term belongs in
//     the per-grain read step too - a separate code path from the sample
//     player's, and therefore a separate regression guard.
//
//     Parameters chosen so the measurement is about pitch and nothing else:
//     200 ms grains at 5 Hz butt-join at unity overlap, so the only amplitude
//     modulation the windowing imposes is at 5 Hz - inside a single 5.86 Hz
//     analysis bin - and all jitter is off.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenGranularResamplesRateMismatchTest,
    "PinWright.audio.gen.granular.ResamplesRateMismatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenGranularResamplesRateMismatchTest::RunTest(const FString& Parameters)
{
    using namespace PwGenSourceTest;

    constexpr int32 SourceRate = 44100;
    constexpr float UnconvertedHz = 440.f * 48000.f / 44100.f;    // 478.9

    // 1.5 s of source, so a 200 ms grain still fits at positionEnd = 1.0.
    USoundWave* Wave = MakeSineWave(SourceRate, SourceRate * 3 / 2, /*Hz=*/440.0, /*Amplitude=*/0.9);
    TestNotNull(TEXT("Fixture wave created"), Wave);
    if (!Wave) return false;
    ON_SCOPE_EXIT { Wave->RemoveFromRoot(); };

    FPwSynthParams Params = GranularParams(Wave->GetPathName(), /*GrainMs=*/200.0, /*DensityHz=*/5.0);
    SetNum(Params, TEXT("positionStart"), 0.0);
    SetNum(Params, TEXT("positionEnd"), 1.0);
    SetNum(Params, TEXT("positionJitter"), 0.0);
    SetNum(Params, TEXT("pitchJitterCents"), 0.0);
    SetNum(Params, TEXT("reverseChance"), 0.0);

    TArray<float> Out;
    Out.SetNumZeroed(RenderRate);

    FString Code;
    FString Error;
    const bool bOk = RunGranular(Params, /*Seed=*/31, Out, Code, Error);
    TestTrue(FString::Printf(TEXT("Generated (code='%s', error='%s')"), *Code, *Error), bOk);
    if (!bOk) return false;

    TestTrue(TEXT("The cloud is audible, not the silence a refused mix would leave"),
        PeakAbs(Out) > 0.1f);

    const float Measured = DominantHz(Out, RenderRate);
    TestEqual(TEXT("Grains from a 44.1 kHz source rendered at 48 kHz still sound at 440 Hz"),
        Measured, 440.f, FrequencyToleranceHz);
    TestTrue(FString::Printf(TEXT("It is not the %.1f Hz an un-resampled grain read would produce (got %.1f)"),
            UnconvertedHz, Measured),
        FMath::Abs(Measured - UnconvertedHz) > 20.f);

    return true;
}

// =========================================================================
// D. loop=false runs to silence past the end of the source; loop=true does
//    not. The counterfactual is the point - without it, a generator that
//    emitted silence everywhere would pass the first half.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenSampleLoopFlagIsHonouredTest,
    "PinWright.audio.gen.sample.LoopFlagIsHonoured",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenSampleLoopFlagIsHonouredTest::RunTest(const FString& Parameters)
{
    using namespace PwGenSourceTest;

    // 2400 frames = 50 ms at the render rate, so the source runs out one sixth of the way in.
    constexpr int32 SourceFrames = 2400;
    constexpr int32 OutSamples = 14400;

    USoundWave* Wave = MakeSineWave(RenderRate, SourceFrames, /*Hz=*/440.0, /*Amplitude=*/0.9);
    TestNotNull(TEXT("Fixture wave created"), Wave);
    if (!Wave) return false;
    ON_SCOPE_EXIT { Wave->RemoveFromRoot(); };

    const FString Path = Wave->GetPathName();

    TArray<float> OneShot;
    OneShot.SetNumZeroed(OutSamples);
    FString Code;
    FString Error;
    const bool bOneShot = RunSample(SampleParams(Path, 1.0, /*bLoop=*/false), 7, OneShot, Code, Error);
    TestTrue(FString::Printf(TEXT("One-shot generated (code='%s')"), *Code), bOneShot);
    if (!bOneShot) return false;

    TestTrue(TEXT("The source itself did play"), PeakAbs(OneShot, 0, SourceFrames - 1) > 0.5f);

    // Read positions are integral here (rate 1.0, matching sample rates, no pitch envelope), so
    // the tail is not "small", it is exactly zero.
    TestEqual(TEXT("Past the end of a non-looping source the output is exactly silent"),
        PeakAbs(OneShot, SourceFrames, OutSamples - 1), 0.f);

    TArray<float> Looped;
    Looped.SetNumZeroed(OutSamples);
    const bool bLooped = RunSample(SampleParams(Path, 1.0, /*bLoop=*/true), 7, Looped, Code, Error);
    TestTrue(FString::Printf(TEXT("Looped generated (code='%s')"), *Code), bLooped);
    if (!bLooped) return false;

    TestTrue(TEXT("loop=true keeps producing signal past the end of the source"),
        PeakAbs(Looped, SourceFrames, OutSamples - 1) > 0.5f);
    TestEqual(TEXT("Looping preserves the pitch"),
        DominantHz(Looped, RenderRate), 440.f, FrequencyToleranceHz);

    return true;
}

// =========================================================================
// E. sample determinism: same seed, same bytes.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenSampleIsDeterministicTest,
    "PinWright.audio.gen.sample.IsDeterministic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenSampleIsDeterministicTest::RunTest(const FString& Parameters)
{
    using namespace PwGenSourceTest;

    USoundWave* Wave = MakeSineWave(44100, 44100, /*Hz=*/330.0, /*Amplitude=*/0.8);
    TestNotNull(TEXT("Fixture wave created"), Wave);
    if (!Wave) return false;
    ON_SCOPE_EXIT { Wave->RemoveFromRoot(); };

    const FPwSynthParams Params = SampleParams(Wave->GetPathName(), /*PlaybackRate=*/1.37,
        /*bLoop=*/true, /*SourceStartMs=*/123.0);

    TArray<float> First;
    First.SetNumZeroed(RenderRate / 4);
    TArray<float> Second;
    Second.SetNumZeroed(RenderRate / 4);

    FString Code;
    FString Error;
    const bool bFirst = RunSample(Params, /*Seed=*/1234, First, Code, Error);
    const bool bSecond = RunSample(Params, /*Seed=*/1234, Second, Code, Error);

    TestTrue(TEXT("Both renders succeeded"), bFirst && bSecond);
    if (!bFirst || !bSecond) return false;

    TestTrue(TEXT("Two renders with the same seed are byte-identical"), BitwiseEqual(First, Second));
    TestTrue(TEXT("The comparison is not vacuous: the render is non-silent"), PeakAbs(First) > 0.1f);

    return true;
}

// =========================================================================
// F. granular produces finite, non-silent, sanely scaled output.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenGranularProducesUsableCloudTest,
    "PinWright.audio.gen.granular.ProducesUsableCloud",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenGranularProducesUsableCloudTest::RunTest(const FString& Parameters)
{
    using namespace PwGenSourceTest;

    USoundWave* Wave = MakeSineWave(RenderRate, RenderRate, /*Hz=*/220.0, /*Amplitude=*/0.8);
    TestNotNull(TEXT("Fixture wave created"), Wave);
    if (!Wave) return false;
    ON_SCOPE_EXIT { Wave->RemoveFromRoot(); };

    FPwSynthParams Params = GranularParams(Wave->GetPathName(), /*GrainMs=*/50.0, /*DensityHz=*/40.0);
    SetNum(Params, TEXT("positionStart"), 0.0);
    SetNum(Params, TEXT("positionEnd"), 1.0);
    SetNum(Params, TEXT("positionJitter"), 0.3);
    SetNum(Params, TEXT("pitchJitterCents"), 300.0);
    SetNum(Params, TEXT("reverseChance"), 0.5);

    TArray<float> Out;
    Out.SetNumZeroed(RenderRate / 2);

    FString Code;
    FString Error;
    const bool bOk = RunGranular(Params, /*Seed=*/99, Out, Code, Error);
    TestTrue(FString::Printf(TEXT("Generated (code='%s', error='%s')"), *Code, *Error), bOk);
    if (!bOk) return false;

    TestTrue(TEXT("No NaN or Inf anywhere in the cloud"), AllFinite(Out));

    const float Peak = PeakAbs(Out);
    TestTrue(FString::Printf(TEXT("Cloud is not silent (peak %g)"), Peak), Peak > 0.05f);

    // Overlap normalization: 40 Hz x 50 ms = 2 grains sounding at once. Without the divide, two
    // random-phase copies of a 0.8 source would routinely exceed 1.5.
    TestTrue(FString::Printf(TEXT("Overlap normalization keeps the cloud near source level (peak %g)"), Peak),
        Peak < 1.5f);

    return true;
}

// =========================================================================
// G. GRAIN WINDOWING. Every grain is Hann-windowed, so no grain boundary can
//    step the signal.
//
//    Threshold derivation. The source is a 0.8-amplitude 220 Hz sine at 48 kHz:
//    its steepest per-sample delta is 0.8 * 2*pi*220/48000 = 0.023. With 50 ms
//    grains at 40 Hz at most three overlap at once, and the cloud is divided by
//    the expected overlap of 2, so the honest bound is 3 * 0.023 / 2 = 0.035;
//    the Hann envelope's own slope is three orders of magnitude below that.
//    0.15 is ~4x that bound. An UNWINDOWED grain starts at whatever phase its
//    jittered read position lands on, injecting a step of up to 0.8 / 2 = 0.4 -
//    well clear of the threshold in the other direction, which is what makes
//    this an assertion rather than a formality.
//
//    pitchJitterCents is 0 here on purpose: a jittered rate scales the source
//    slope and would make the bound rate-dependent. positionJitter stays on,
//    because random read positions are precisely what puts an arbitrary phase
//    at each grain boundary.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenGranularWindowsEveryGrainTest,
    "PinWright.audio.gen.granular.WindowsEveryGrain",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenGranularWindowsEveryGrainTest::RunTest(const FString& Parameters)
{
    using namespace PwGenSourceTest;

    USoundWave* Wave = MakeSineWave(RenderRate, RenderRate, /*Hz=*/220.0, /*Amplitude=*/0.8);
    TestNotNull(TEXT("Fixture wave created"), Wave);
    if (!Wave) return false;
    ON_SCOPE_EXIT { Wave->RemoveFromRoot(); };

    FPwSynthParams Params = GranularParams(Wave->GetPathName(), /*GrainMs=*/50.0, /*DensityHz=*/40.0);
    SetNum(Params, TEXT("positionStart"), 0.0);
    SetNum(Params, TEXT("positionEnd"), 1.0);
    SetNum(Params, TEXT("positionJitter"), 0.3);
    SetNum(Params, TEXT("pitchJitterCents"), 0.0);
    SetNum(Params, TEXT("reverseChance"), 0.5);

    TArray<float> Out;
    Out.SetNumZeroed(RenderRate / 2);

    FString Code;
    FString Error;
    const bool bOk = RunGranular(Params, /*Seed=*/4242, Out, Code, Error);
    TestTrue(FString::Printf(TEXT("Generated (code='%s', error='%s')"), *Code, *Error), bOk);
    if (!bOk) return false;

    const float Step = MaxStep(Out);
    TestTrue(FString::Printf(TEXT("No grain-boundary click: max sample-to-sample step %g < 0.15"), Step),
        Step < 0.15f);
    TestTrue(TEXT("The measurement is not vacuous: the cloud is audible"), PeakAbs(Out) > 0.05f);

    return true;
}

// =========================================================================
// H. granular determinism, including every jitter draw. Byte-identical across
//    two runs with the same seed, and NOT identical across two seeds - without
//    the second half an all-zero generator would pass.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenGranularIsDeterministicTest,
    "PinWright.audio.gen.granular.IsDeterministic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenGranularIsDeterministicTest::RunTest(const FString& Parameters)
{
    using namespace PwGenSourceTest;

    USoundWave* Wave = MakeSineWave(RenderRate, RenderRate, /*Hz=*/220.0, /*Amplitude=*/0.8);
    TestNotNull(TEXT("Fixture wave created"), Wave);
    if (!Wave) return false;
    ON_SCOPE_EXIT { Wave->RemoveFromRoot(); };

    FPwSynthParams Params = GranularParams(Wave->GetPathName(), /*GrainMs=*/30.0, /*DensityHz=*/60.0);
    SetNum(Params, TEXT("positionStart"), 0.1);
    SetNum(Params, TEXT("positionEnd"), 0.9);
    SetNum(Params, TEXT("positionJitter"), 0.25);
    SetNum(Params, TEXT("pitchJitterCents"), 400.0);
    SetNum(Params, TEXT("reverseChance"), 0.4);

    TArray<float> First;
    First.SetNumZeroed(RenderRate / 4);
    TArray<float> Second;
    Second.SetNumZeroed(RenderRate / 4);
    TArray<float> OtherSeed;
    OtherSeed.SetNumZeroed(RenderRate / 4);

    FString Code;
    FString Error;
    const bool bFirst = RunGranular(Params, /*Seed=*/5150, First, Code, Error);
    const bool bSecond = RunGranular(Params, /*Seed=*/5150, Second, Code, Error);
    const bool bOther = RunGranular(Params, /*Seed=*/5151, OtherSeed, Code, Error);

    TestTrue(TEXT("All three renders succeeded"), bFirst && bSecond && bOther);
    if (!bFirst || !bSecond || !bOther) return false;

    TestTrue(TEXT("Same seed gives byte-identical grain jitter"), BitwiseEqual(First, Second));
    TestFalse(TEXT("A different seed gives a different cloud"), BitwiseEqual(First, OtherSeed));
    TestTrue(TEXT("The comparison is not vacuous: the cloud is non-silent"), PeakAbs(First) > 0.05f);

    return true;
}

// =========================================================================
// I. Grain N's random draws depend only on N, never on how many grains
//    preceded it. Two renders differing ONLY in densityHz must still agree
//    exactly on grain 0.
//
//    Both densities sit below unity overlap (10 ms grains at 20 Hz and 10 Hz
//    are 0.2 and 0.1), so the overlap normalization is 1.0 in both and cannot
//    account for the match; and grain 1 lands at 2400 / 4800 samples, so the
//    first 480 samples carry grain 0 alone in both renders.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenGranularSeedingIsIndexStableTest,
    "PinWright.audio.gen.granular.SeedingIsIndexStable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenGranularSeedingIsIndexStableTest::RunTest(const FString& Parameters)
{
    using namespace PwGenSourceTest;

    constexpr int32 GrainSamples = 480;     // 10 ms at 48 kHz

    USoundWave* Wave = MakeSineWave(RenderRate, RenderRate, /*Hz=*/220.0, /*Amplitude=*/0.8);
    TestNotNull(TEXT("Fixture wave created"), Wave);
    if (!Wave) return false;
    ON_SCOPE_EXIT { Wave->RemoveFromRoot(); };

    const FString Path = Wave->GetPathName();

    auto MakeParams = [&Path](double DensityHz)
    {
        FPwSynthParams Params = GranularParams(Path, /*GrainMs=*/10.0, DensityHz);
        SetNum(Params, TEXT("positionStart"), 0.0);
        SetNum(Params, TEXT("positionEnd"), 1.0);
        SetNum(Params, TEXT("positionJitter"), 0.4);
        SetNum(Params, TEXT("pitchJitterCents"), 500.0);
        SetNum(Params, TEXT("reverseChance"), 0.3);
        return Params;
    };

    TArray<float> Sparse;
    Sparse.SetNumZeroed(RenderRate / 4);
    TArray<float> Denser;
    Denser.SetNumZeroed(RenderRate / 4);

    FString Code;
    FString Error;
    const bool bSparse = RunGranular(MakeParams(10.0), /*Seed=*/808, Sparse, Code, Error);
    const bool bDenser = RunGranular(MakeParams(20.0), /*Seed=*/808, Denser, Code, Error);

    TestTrue(TEXT("Both renders succeeded"), bSparse && bDenser);
    if (!bSparse || !bDenser) return false;

    TestTrue(TEXT("Grain 0 is bit-identical at both densities"),
        RangeBitwiseEqual(Sparse, Denser, GrainSamples));
    TestFalse(TEXT("The two renders do differ overall, so the match above means something"),
        BitwiseEqual(Sparse, Denser));
    TestTrue(TEXT("Grain 0 actually produced signal"), PeakAbs(Sparse, 0, GrainSamples - 1) > 0.01f);

    return true;
}

// =========================================================================
// J. Failure direction (rpc-design.md §12): a missing asset. Both generators
//    report ASSET_NOT_FOUND and leave the caller's buffer alone.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenRejectsMissingSourceTest,
    "PinWright.audio.gen.sample.RejectsMissingSource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenRejectsMissingSourceTest::RunTest(const FString& Parameters)
{
    using namespace PwGenSourceTest;

    const FString Missing = TEXT("/Game/PinWrightTests/NoSuchWave_9f1c.NoSuchWave_9f1c");

    TArray<float> SampleOut = MakeSentinelBuffer(64);
    FString Code;
    FString Error;
    const bool bSample = RunSample(SampleParams(Missing, 1.0, false), 7, SampleOut, Code, Error);

    TestFalse(TEXT("sample refuses a source that resolves to nothing"), bSample);
    TestEqual(TEXT("sample reports ASSET_NOT_FOUND"), Code, FString(ErrorCodes::ERR_ASSET_NOT_FOUND));
    TestTrue(TEXT("sample names the path in the message"), Error.Contains(TEXT("NoSuchWave_9f1c")));
    TestTrue(TEXT("sample left the output buffer untouched"), IsAllSentinel(SampleOut));

    TArray<float> GranularOut = MakeSentinelBuffer(64);
    const bool bGranular = RunGranular(GranularParams(Missing, 20.0, 50.0), 7, GranularOut, Code, Error);

    TestFalse(TEXT("granular refuses a source that resolves to nothing"), bGranular);
    TestEqual(TEXT("granular reports ASSET_NOT_FOUND"), Code, FString(ErrorCodes::ERR_ASSET_NOT_FOUND));
    TestTrue(TEXT("granular left the output buffer untouched"), IsAllSentinel(GranularOut));

    return true;
}

// =========================================================================
// K. Failure direction: non-positive grainMs / densityHz and an inverted
//    position range are errors, never quietly repaired (rpc-design.md §1/§3).
//    The source here is real, so the rejection can only come from the
//    parameter under test.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwGenGranularRejectsBadParamsTest,
    "PinWright.audio.gen.granular.RejectsBadParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwGenGranularRejectsBadParamsTest::RunTest(const FString& Parameters)
{
    using namespace PwGenSourceTest;

    USoundWave* Wave = MakeSineWave(RenderRate, RenderRate / 4, /*Hz=*/220.0, /*Amplitude=*/0.8);
    TestNotNull(TEXT("Fixture wave created"), Wave);
    if (!Wave) return false;
    ON_SCOPE_EXIT { Wave->RemoveFromRoot(); };

    const FString Path = Wave->GetPathName();

    // Sanity: the same source with sane parameters DOES render, so the rejections below are
    // attributable to the parameter and not to the fixture.
    {
        TArray<float> Control;
        Control.SetNumZeroed(4800);
        FString Code;
        FString Error;
        TestTrue(TEXT("Control render with the same source succeeds"),
            RunGranular(GranularParams(Path, 20.0, 50.0), 7, Control, Code, Error));
    }

    struct FCase
    {
        const TCHAR* What;
        double GrainMs;
        double DensityHz;
        double PositionStart;
        double PositionEnd;
        const TCHAR* MessageFragment;
    };

    const FCase Cases[] = {
        { TEXT("grainMs = 0"),        0.0,  50.0, 0.0, 1.0, TEXT("grainMs") },
        { TEXT("grainMs negative"),  -5.0,  50.0, 0.0, 1.0, TEXT("grainMs") },
        { TEXT("densityHz = 0"),     20.0,   0.0, 0.0, 1.0, TEXT("densityHz") },
        { TEXT("densityHz negative"),20.0, -12.0, 0.0, 1.0, TEXT("densityHz") },
        { TEXT("inverted positions"),20.0,  50.0, 0.9, 0.2, TEXT("positionStart") }
    };

    for (const FCase& Case : Cases)
    {
        FPwSynthParams Params = GranularParams(Path, Case.GrainMs, Case.DensityHz);
        SetNum(Params, TEXT("positionStart"), Case.PositionStart);
        SetNum(Params, TEXT("positionEnd"), Case.PositionEnd);

        TArray<float> Out = MakeSentinelBuffer(64);
        FString Code;
        FString Error;
        const bool bOk = RunGranular(Params, 7, Out, Code, Error);

        TestFalse(FString::Printf(TEXT("%s is refused"), Case.What), bOk);
        TestEqual(FString::Printf(TEXT("%s reports INVALID_PARAMS"), Case.What),
            Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestTrue(FString::Printf(TEXT("%s names the offending parameter (got '%s')"), Case.What, *Error),
            Error.Contains(Case.MessageFragment));
        TestTrue(FString::Printf(TEXT("%s left the output buffer untouched"), Case.What),
            IsAllSentinel(Out));
    }

    return true;
}

// =========================================================================
// L. PwResolveSourceBuffer reports the asset's NATIVE sample rate, and
//    PROPAGATES the decoder's code instead of flattening every failure to
//    DECODE_FAILED (rpc-design.md §7).
//
//    The fixture is 44.1 kHz on purpose. FPwAudioBuffer defaults SampleRate to
//    48000, so a loader that forgot to carry the rate through would still look
//    right against a 48 kHz fixture - and the resulting native-rate buffer,
//    forwarded into a render, is refused by FPwAudioBuffer::MixInto and mixes
//    to inaudible silence with no error. Asserting a rate the default cannot
//    fake is what makes this a measurement.
//
//    A procedural wave is the cleanest probe for the code propagation: it has
//    a distinct code and is reachable without touching the filesystem.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwResolveSourcePropagatesDecodeCodeTest,
    "PinWright.audio.gen.source.PropagatesDecodeCode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwResolveSourcePropagatesDecodeCodeTest::RunTest(const FString& Parameters)
{
    using namespace PwGenSourceTest;

    constexpr int32 NativeRate = 44100;

    USoundWave* Wave = MakeSineWave(NativeRate, 4800, /*Hz=*/440.0, /*Amplitude=*/0.5);
    TestNotNull(TEXT("Fixture wave created"), Wave);
    if (!Wave) return false;
    ON_SCOPE_EXIT { Wave->RemoveFromRoot(); };

    const FString Path = Wave->GetPathName();

    // First prove the path resolves and decodes at all, so the rejection below is attributable to
    // the procedural flag rather than to an unresolvable path.
    {
        FPwAudioBuffer Buffer;
        FString Code;
        FString Error;
        const bool bOk = PwResolveSourceBuffer(Path, Buffer, Code, Error);
        TestTrue(FString::Printf(TEXT("Transient wave resolves and decodes (code='%s')"), *Code), bOk);
        TestEqual(TEXT("Decoded frame count matches the fixture"), Buffer.NumFrames(), 4800);
        TestEqual(TEXT("The buffer carries the asset's NATIVE rate, not the render rate"),
            Buffer.SampleRate, NativeRate);
        TestNotEqual(TEXT("...and not FPwAudioBuffer's 48000 default"), Buffer.SampleRate, RenderRate);
    }

    Wave->bProcedural = 1;

    FPwAudioBuffer Buffer;
    FString Code;
    FString Error;
    const bool bOk = PwResolveSourceBuffer(Path, Buffer, Code, Error);

    TestFalse(TEXT("A procedural wave is refused"), bOk);
    TestEqual(TEXT("The decoder's own code survives the resolve"),
        Code, FString(ErrorCodes::ERR_AUDIO_PROCEDURAL_UNSUPPORTED));
    TestNotEqual(TEXT("It is not flattened into the generic decode failure"),
        Code, FString(ErrorCodes::ERR_DECODE_FAILED));
    TestEqual(TEXT("The output buffer is left empty"), Buffer.NumFrames(), 0);

    return true;
}

// =========================================================================
// M. PwResolveSourceBuffer separates a caller bug from missing content: an
//    empty path is INVALID_PARAMS, a real-looking path that resolves nowhere
//    is ASSET_NOT_FOUND. Two different fixes, so two different codes.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwResolveSourceDistinguishesEmptyFromMissingTest,
    "PinWright.audio.gen.source.DistinguishesEmptyFromMissing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwResolveSourceDistinguishesEmptyFromMissingTest::RunTest(const FString& Parameters)
{
    using namespace PwGenSourceTest;

    FPwAudioBuffer Buffer;
    FString Code;
    FString Error;

    TestFalse(TEXT("An empty path is refused"),
        PwResolveSourceBuffer(FString(), Buffer, Code, Error));
    TestEqual(TEXT("An empty path is a caller bug: INVALID_PARAMS"),
        Code, FString(ErrorCodes::ERR_INVALID_PARAMS));

    const FString Missing = TEXT("/Game/PinWrightTests/AbsentWave_b73e.AbsentWave_b73e");
    TestFalse(TEXT("An unresolvable path is refused"),
        PwResolveSourceBuffer(Missing, Buffer, Code, Error));
    TestEqual(TEXT("An unresolvable path is missing content: ASSET_NOT_FOUND"),
        Code, FString(ErrorCodes::ERR_ASSET_NOT_FOUND));
    TestTrue(TEXT("The message names the path so the caller can fix it"),
        Error.Contains(TEXT("AbsentWave_b73e")));
    TestTrue(TEXT("The message says assets only, so the caller does not go looking for a file path"),
        Error.Contains(TEXT("import")));

    return true;
}
