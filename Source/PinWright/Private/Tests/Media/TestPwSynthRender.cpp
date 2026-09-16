// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the recipe renderer (AudioGen/PwSynthDsp.h - PwRenderRecipe).
//
// The property under test is DETERMINISM, and it is asserted the only way that means anything:
// byte-for-byte. A "the two renders sound the same" tolerance would pass a renderer whose noise
// depends on layer evaluation order, which is the exact defect FPwSeededRandom::Derive exists to
// prevent. So the substream test renders two recipes that differ ONLY in layer 2 and requires
// layer 1's channel to be bit-identical, not close.
//
// Everything else is measured against ground truth rather than against "it returned true": a
// synthesised sine has a known STFT bin, a layer at 200 ms has a known first non-zero sample, a
// -6 dBFS normalize target has a known resulting peak, and a fade has an exactly-zero first
// sample. Per rpc-design.md §12 the failure direction carries equal weight - an Unspecified
// generator, an unknown effect and a generator that rejects its own parameters must each abort
// the whole render and leave the caller with an EMPTY buffer and an unmeasured report, because a
// half-rendered buffer that reports success is worse than no buffer at all.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwStft.h"
#include "AudioGen/PwSynthDsp.h"
#include "AudioGen/PwSynthRecipe.h"
#include "Compat/EngineVersionCompat.h"
#include "Handlers/ErrorCodes.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Named (not anonymous) namespace: the module builds with bUseUnity = true.
namespace PwSynthRenderTestHelpers
{
    constexpr int32 TestSampleRate = 48000;
    constexpr double Ln10 = 2.30258509299404568402;

    /** Fixtures go through the real parser, so every test renders the bag production produces. */
    bool BuildRecipe(const FString& Json, FPwSynthRecipe& Out, FString& OutError)
    {
        TSharedPtr<FJsonObject> Root;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
        if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
        {
            OutError = TEXT("fixture is not valid JSON");
            return false;
        }
        return ParseSynthRecipe(Root, Out, OutError);
    }

    FString OscLayerJson(const TCHAR* Waveform, double FrequencyHz, double Pan,
        double StartMs = 0.0, double GainDb = 0.0)
    {
        return FString::Printf(
            TEXT("{\"startMs\":%f,\"gainDb\":%f,\"pan\":%f,\"generator\":{\"kind\":\"osc\",")
            TEXT("\"params\":{\"waveform\":\"%s\",\"frequencyHz\":%f}}}"),
            StartMs, GainDb, Pan, Waveform, FrequencyHz);
    }

    FString NoiseLayerJson(const TCHAR* Color, double Pan, double LowCutHz = 20.0)
    {
        return FString::Printf(
            TEXT("{\"pan\":%f,\"generator\":{\"kind\":\"noise\",")
            TEXT("\"params\":{\"color\":\"%s\",\"lowCutHz\":%f}}}"),
            Pan, Color, LowCutHz);
    }

    FString RecipeJson(int32 Seed, double DurationMs, const TArray<FString>& Layers,
        const FString& MasterJson = TEXT("{}"))
    {
        return FString::Printf(
            TEXT("{\"version\":1,\"seed\":%d,\"sampleRate\":%d,\"durationMs\":%f,")
            TEXT("\"layers\":[%s],\"master\":%s}"),
            Seed, TestSampleRate, DurationMs, *FString::Join(Layers, TEXT(",")), *MasterJson);
    }

    /** Bit-for-bit, not "within tolerance". A tolerance here would pass the defect under test. */
    bool ChannelsAreByteIdentical(const TArray<float>& A, const TArray<float>& B)
    {
        if (A.Num() != B.Num())
        {
            return false;
        }
        return A.Num() == 0
            || FMemory::Memcmp(A.GetData(), B.GetData(), A.Num() * sizeof(float)) == 0;
    }

    bool BuffersAreByteIdentical(const FPwAudioBuffer& A, const FPwAudioBuffer& B)
    {
        return A.SampleRate == B.SampleRate
            && ChannelsAreByteIdentical(A.Left, B.Left)
            && ChannelsAreByteIdentical(A.Right, B.Right);
    }

    bool AllExactlyZero(const TArray<float>& Channel)
    {
        for (const float Sample : Channel)
        {
            if (Sample != 0.f)
            {
                return false;
            }
        }
        return true;
    }

    float MaxAbs(const TArray<float>& Channel, int32 First, int32 Count)
    {
        float Peak = 0.f;
        const int32 Last = FMath::Min(First + Count, Channel.Num());
        for (int32 Index = FMath::Max(First, 0); Index < Last; ++Index)
        {
            Peak = FMath::Max(Peak, FMath::Abs(Channel[Index]));
        }
        return Peak;
    }

    double PeakDbOf(const FPwAudioBuffer& Buffer)
    {
        const double Peak = FMath::Max(
            static_cast<double>(MaxAbs(Buffer.Left, 0, Buffer.Left.Num())),
            static_cast<double>(MaxAbs(Buffer.Right, 0, Buffer.Right.Num())));
        return (Peak > 0.0) ? 20.0 * FMath::Loge(Peak) / Ln10 : PwSynthRender::SilenceFloorDb;
    }

    /**
     * The full "nothing survived the failure" contract, asserted the same way everywhere.
     *
     * Every abort test checks BOTH halves: the error code, and that `Out` and `OutReport` still
     * hold nothing. A renderer that aborted but left half a buffer behind would pass a code-only
     * assertion and hand the caller samples it never finished writing.
     */
    void CheckAbortedRender(FAutomationTestBase& Test, const FString& What,
        bool bReturned, const FPwAudioBuffer& Out, const FPwRenderReport& Report,
        const FString& Code, const FString& Error, const FString& ExpectedCode,
        const FString& ExpectedFieldPath)
    {
        Test.TestFalse(What + TEXT(" aborts the render"), bReturned);
        Test.TestEqual(What + TEXT(" reports its code"), Code, ExpectedCode);
        Test.TestTrue(What + TEXT(" names the recipe field path in the message: ") + Error,
            Error.Contains(ExpectedFieldPath));
        Test.TestEqual(What + TEXT(" leaves the output buffer empty"), Out.NumFrames(), 0);
        Test.TestFalse(What + TEXT(" leaves the report unmeasured"), Report.bMeasured);
        Test.TestEqual(What + TEXT(" leaves no partial layer rows"), Report.Layers.Num(), 0);
    }

    int32 PeakBin(const FPwStftResult& Result, int32 FrameIndex)
    {
        int32 BestBin = INDEX_NONE;
        float BestMagnitude = -1.f;
        for (int32 Bin = 0; Bin < Result.NumBins; ++Bin)
        {
            const float Magnitude = PwStftMagnitudeAt(Result, FrameIndex, Bin);
            if (Magnitude > BestMagnitude)
            {
                BestMagnitude = Magnitude;
                BestBin = Bin;
            }
        }
        return BestBin;
    }
}

// =========================================================================
// Determinism
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSynthRenderSameSeedTest,
    "PinWright.audio.synth.render.SameSeedRendersByteIdentical",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSynthRenderSameSeedTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthRenderTestHelpers;

    // A noise layer is what makes this test worth running: an all-oscillator recipe would be
    // deterministic even with a broken RNG.
    const FString Json = RecipeJson(4242, 250.0, {
        OscLayerJson(TEXT("sine"), 440.0, -0.5),
        NoiseLayerJson(TEXT("white"), 0.5)
    });

    FPwSynthRecipe Recipe;
    FString ParseError;
    if (!BuildRecipe(Json, Recipe, ParseError))
    {
        AddError(FString::Printf(TEXT("fixture did not parse: %s"), *ParseError));
        return false;
    }

    FPwAudioBuffer FirstBuffer;
    FPwAudioBuffer SecondBuffer;
    FPwRenderReport FirstReport;
    FPwRenderReport SecondReport;
    FString Code;
    FString Error;

    if (!PwRenderRecipe(Recipe, FirstBuffer, FirstReport, Code, Error) ||
        !PwRenderRecipe(Recipe, SecondBuffer, SecondReport, Code, Error))
    {
        AddError(FString::Printf(TEXT("render failed: [%s] %s"), *Code, *Error));
        return false;
    }

    TestTrue(TEXT("The render reports that it measured itself"), FirstReport.bMeasured);
    TestEqual(TEXT("250 ms at 48 kHz is 12000 frames"), FirstBuffer.NumFrames(), 12000);
    TestTrue(TEXT("The same recipe and seed render a byte-identical buffer"),
        BuffersAreByteIdentical(FirstBuffer, SecondBuffer));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSynthRenderLayerPeakStagesTest,
    "PinWright.audio.synth.render.LayerReportPublishesPreAndPostGainPeaks",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSynthRenderLayerPeakStagesTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthRenderTestHelpers;

    FPwSynthRecipe FullGain;
    FPwSynthRecipe ReducedGain;
    FString ParseError;
    const FString NoMasterFades = TEXT("{\"fadeInMs\":0,\"fadeOutMs\":0}");
    if (!BuildRecipe(RecipeJson(3, 120.0,
            {OscLayerJson(TEXT("sine"), 500.0, 0.25, 0.0, 0.0)}, NoMasterFades),
            FullGain, ParseError) ||
        !BuildRecipe(RecipeJson(3, 120.0,
            {OscLayerJson(TEXT("sine"), 500.0, 0.25, 0.0, -40.0)}, NoMasterFades),
            ReducedGain, ParseError))
    {
        AddError(FString::Printf(TEXT("fixture did not parse: %s"), *ParseError));
        return false;
    }

    FPwAudioBuffer FullBuffer;
    FPwAudioBuffer ReducedBuffer;
    FPwRenderReport FullReport;
    FPwRenderReport ReducedReport;
    FString Code;
    FString Error;
    if (!PwRenderRecipe(FullGain, FullBuffer, FullReport, Code, Error) ||
        !PwRenderRecipe(ReducedGain, ReducedBuffer, ReducedReport, Code, Error))
    {
        AddError(FString::Printf(TEXT("render failed: [%s] %s"), *Code, *Error));
        return false;
    }

    TestEqual(TEXT("the full-gain render has one measured layer row"), FullReport.Layers.Num(), 1);
    TestEqual(TEXT("the reduced-gain render has one measured layer row"),
        ReducedReport.Layers.Num(), 1);
    if (FullReport.Layers.Num() != 1 || ReducedReport.Layers.Num() != 1)
    {
        return false;
    }

    const FPwLayerReport& FullLayer = FullReport.Layers[0];
    const FPwLayerReport& ReducedLayer = ReducedReport.Layers[0];
    TestTrue(TEXT("the pre-gain peak is measured for the full-gain layer"),
        FullLayer.PeakLinear > 0.0);
    TestTrue(TEXT("the deliberate pre-gain diagnostic is unchanged by gainDb"),
        FMath::IsNearlyEqual(FullLayer.PeakLinear, ReducedLayer.PeakLinear, 1e-6));
    TestTrue(TEXT("the post-gain peak is lower after a -40 dB trim"),
        ReducedLayer.PeakLinearPostGain < FullLayer.PeakLinearPostGain);
    TestTrue(TEXT("the post-gain peak reflects the 40 dB trim"),
        FMath::IsNearlyEqual(
            FullLayer.PeakLinearPostGain / ReducedLayer.PeakLinearPostGain, 100.0, 0.01));

    const double FullRenderedStereoPeak = FMath::Max(
        static_cast<double>(MaxAbs(FullBuffer.Left, 0, FullBuffer.Left.Num())),
        static_cast<double>(MaxAbs(FullBuffer.Right, 0, FullBuffer.Right.Num())));
    const double ReducedRenderedStereoPeak = FMath::Max(
        static_cast<double>(MaxAbs(ReducedBuffer.Left, 0, ReducedBuffer.Left.Num())),
        static_cast<double>(MaxAbs(ReducedBuffer.Right, 0, ReducedBuffer.Right.Num())));
    TestTrue(TEXT("the full post-gain peak equals the rendered stereo peak"),
        FMath::IsNearlyEqual(FullLayer.PeakLinearPostGain, FullRenderedStereoPeak, 1e-6));
    TestTrue(TEXT("the reduced post-gain peak equals the rendered stereo peak"),
        FMath::IsNearlyEqual(ReducedLayer.PeakLinearPostGain, ReducedRenderedStereoPeak, 1e-6));
    TestFalse(TEXT("the non-center pan produces different left and right peaks"),
        FMath::IsNearlyEqual(
            static_cast<double>(MaxAbs(FullBuffer.Left, 0, FullBuffer.Left.Num())),
            static_cast<double>(MaxAbs(FullBuffer.Right, 0, FullBuffer.Right.Num())), 1e-6));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSynthRenderSeedChangesOutputTest,
    "PinWright.audio.synth.render.DifferentSeedChangesOutput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSynthRenderSeedChangesOutputTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthRenderTestHelpers;

    // The control for the test above: byte-identity would also be satisfied by a renderer that
    // ignores the seed entirely, so the seed has to be shown to matter.
    const TArray<FString> Layers = { NoiseLayerJson(TEXT("white"), 0.0) };

    FPwSynthRecipe SeedOne;
    FPwSynthRecipe SeedTwo;
    FString ParseError;
    if (!BuildRecipe(RecipeJson(1, 120.0, Layers), SeedOne, ParseError) ||
        !BuildRecipe(RecipeJson(2, 120.0, Layers), SeedTwo, ParseError))
    {
        AddError(FString::Printf(TEXT("fixture did not parse: %s"), *ParseError));
        return false;
    }

    FPwAudioBuffer BufferOne;
    FPwAudioBuffer BufferTwo;
    FPwRenderReport ReportOne;
    FPwRenderReport ReportTwo;
    FString Code;
    FString Error;

    if (!PwRenderRecipe(SeedOne, BufferOne, ReportOne, Code, Error) ||
        !PwRenderRecipe(SeedTwo, BufferTwo, ReportTwo, Code, Error))
    {
        AddError(FString::Printf(TEXT("render failed: [%s] %s"), *Code, *Error));
        return false;
    }

    TestFalse(TEXT("A different seed produces a different noise layer"),
        BuffersAreByteIdentical(BufferOne, BufferTwo));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSynthRenderSubstreamOrderTest,
    "PinWright.audio.synth.render.LayerSubstreamsAreOrderIndependent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSynthRenderSubstreamOrderTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthRenderTestHelpers;

    // THE assertion that proves Root.Derive(LayerIndex) rather than one advancing stream.
    // Both layers are noise, so both consume randomness; layer 0 is hard left and layer 1 hard
    // right, so each owns one channel outright. Changing layer 1's parameters changes how much
    // randomness layer 1 draws - with a shared advancing stream that would move layer 0 too.
    FPwSynthRecipe Before;
    FPwSynthRecipe After;
    FString ParseError;
    if (!BuildRecipe(RecipeJson(77, 150.0, {
            NoiseLayerJson(TEXT("white"), -1.0),
            NoiseLayerJson(TEXT("white"), 1.0, 20.0)
        }), Before, ParseError) ||
        !BuildRecipe(RecipeJson(77, 150.0, {
            NoiseLayerJson(TEXT("white"), -1.0),
            NoiseLayerJson(TEXT("pink"), 1.0, 500.0)
        }), After, ParseError))
    {
        AddError(FString::Printf(TEXT("fixture did not parse: %s"), *ParseError));
        return false;
    }

    FPwAudioBuffer BeforeBuffer;
    FPwAudioBuffer AfterBuffer;
    FPwRenderReport BeforeReport;
    FPwRenderReport AfterReport;
    FString Code;
    FString Error;

    if (!PwRenderRecipe(Before, BeforeBuffer, BeforeReport, Code, Error) ||
        !PwRenderRecipe(After, AfterBuffer, AfterReport, Code, Error))
    {
        AddError(FString::Printf(TEXT("render failed: [%s] %s"), *Code, *Error));
        return false;
    }

    TestTrue(TEXT("Editing layer 1 leaves layer 0's channel bit-identical"),
        ChannelsAreByteIdentical(BeforeBuffer.Left, AfterBuffer.Left));
    TestFalse(TEXT("Editing layer 1 does change layer 1's channel"),
        ChannelsAreByteIdentical(BeforeBuffer.Right, AfterBuffer.Right));

    return true;
}

// =========================================================================
// Ground truth: frequency, offsets, pan, level, edges
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSynthRenderSineFrequencyTest,
    "PinWright.audio.synth.render.SineLayerLandsOnItsFrequencyBin",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSynthRenderSineFrequencyTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthRenderTestHelpers;

    constexpr double RequestedHz = 1000.0;

    FPwSynthRecipe Recipe;
    FString ParseError;
    if (!BuildRecipe(RecipeJson(0, 500.0, { OscLayerJson(TEXT("sine"), RequestedHz, 0.0) }),
            Recipe, ParseError))
    {
        AddError(FString::Printf(TEXT("fixture did not parse: %s"), *ParseError));
        return false;
    }

    FPwAudioBuffer Rendered;
    FPwRenderReport Report;
    FString Code;
    FString Error;
    if (!PwRenderRecipe(Recipe, Rendered, Report, Code, Error))
    {
        AddError(FString::Printf(TEXT("render failed: [%s] %s"), *Code, *Error));
        return false;
    }

    FPwStftSettings Settings;
    Settings.FftSize = 2048;
    Settings.HopSize = 512;

    FPwStftResult Spectrum;
    FPwStftError StftError;
    if (!PwComputeStft(Rendered.Left, Rendered.SampleRate, Settings, Spectrum, &StftError))
    {
        AddError(FString::Printf(TEXT("STFT failed: [%s] %s"), *StftError.Code, *StftError.Message));
        return false;
    }

    // Frame 5 is well past the analysis ramp-in and well inside a 500 ms render.
    const int32 Bin = PeakBin(Spectrum, 5);
    const double MeasuredHz = static_cast<double>(Bin) * static_cast<double>(Spectrum.BinHz);

    TestTrue(FString::Printf(
            TEXT("A 1000 Hz sine peaks within one bin of 1000 Hz (measured %.1f Hz, bin width %.2f Hz)"),
            MeasuredHz, Spectrum.BinHz),
        FMath::Abs(MeasuredHz - RequestedHz) <= static_cast<double>(Spectrum.BinHz));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSynthRenderStartOffsetTest,
    "PinWright.audio.synth.render.LayersLandAtTheirStartOffsets",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSynthRenderStartOffsetTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthRenderTestHelpers;

    constexpr double LateStartMs = 200.0;
    constexpr int32 LateStartFrame = 9600;      // 200 ms at 48 kHz
    constexpr int32 TotalFrames = 24000;        // 500 ms at 48 kHz

    // Hard-panned so each layer owns one channel and the offset is readable off the samples
    // rather than inferred from a sum.
    FPwSynthRecipe Recipe;
    FString ParseError;
    if (!BuildRecipe(RecipeJson(0, 500.0, {
            OscLayerJson(TEXT("sine"), 1000.0, -1.0, 0.0),
            OscLayerJson(TEXT("sine"), 1000.0, 1.0, LateStartMs)
        }), Recipe, ParseError))
    {
        AddError(FString::Printf(TEXT("fixture did not parse: %s"), *ParseError));
        return false;
    }

    FPwAudioBuffer Rendered;
    FPwRenderReport Report;
    FString Code;
    FString Error;
    if (!PwRenderRecipe(Recipe, Rendered, Report, Code, Error))
    {
        AddError(FString::Printf(TEXT("render failed: [%s] %s"), *Code, *Error));
        return false;
    }

    if (!TestEqual(TEXT("Both layers are reported"), Report.Layers.Num(), 2))
    {
        return false;
    }

    TestEqual(TEXT("The layer at 0 ms mixes the whole render"),
        Report.Layers[0].FramesMixed, TotalFrames);
    TestEqual(TEXT("The layer at 200 ms mixes only what fits after its start"),
        Report.Layers[1].FramesMixed, TotalFrames - LateStartFrame);

    TestTrue(TEXT("The right channel is untouched before the late layer starts"),
        MaxAbs(Rendered.Right, 0, LateStartFrame) == 0.f);
    // A quarter cycle of 1000 Hz past the start, so the sine is at its crest rather than its
    // zero crossing - a test reading the very first sample would pass on a silent layer.
    TestTrue(TEXT("The right channel carries signal a quarter cycle after the late start"),
        FMath::Abs(Rendered.Right[LateStartFrame + 12]) > 0.1f);
    TestTrue(TEXT("The left channel carries signal from the very beginning"),
        MaxAbs(Rendered.Left, 0, 48) > 0.1f);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSynthRenderPastEndLayerTest,
    "PinWright.audio.synth.render.LayerPastTheEndIsMeasuredNotSkipped",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSynthRenderPastEndLayerTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthRenderTestHelpers;

    // The refusal is only useful if it is discoverable, so the log line is declared: if it is
    // ever dropped, this test fails on the unmatched expectation.
    AddExpectedMessage(TEXT("at or past the"), ELogVerbosity::Warning,
        EAutomationExpectedMessageFlags::Contains, 1);

    FPwSynthRecipe Recipe;
    FString ParseError;
    if (!BuildRecipe(RecipeJson(0, 100.0, {
            OscLayerJson(TEXT("sine"), 500.0, 0.0, 0.0),
            OscLayerJson(TEXT("sine"), 500.0, 0.0, 50.0)
        }), Recipe, ParseError))
    {
        AddError(FString::Printf(TEXT("fixture did not parse: %s"), *ParseError));
        return false;
    }

    // The parser rejects startMs >= durationMs, so the out-of-range value is injected after
    // parsing: the renderer must still MEASURE it rather than quietly drop the layer.
    Recipe.Layers[1].StartMs = 250.0;

    FPwAudioBuffer Rendered;
    FPwRenderReport Report;
    FString Code;
    FString Error;
    if (!PwRenderRecipe(Recipe, Rendered, Report, Code, Error))
    {
        AddError(FString::Printf(TEXT("render failed: [%s] %s"), *Code, *Error));
        return false;
    }

    if (!TestEqual(TEXT("An out-of-range layer still gets a report row"), Report.Layers.Num(), 2))
    {
        return false;
    }

    TestTrue(TEXT("The out-of-range layer is reported as MEASURED, not skipped"),
        Report.Layers[1].bMeasured);
    TestEqual(TEXT("The out-of-range layer mixed zero frames"), Report.Layers[1].FramesMixed, 0);
    TestEqual(TEXT("The row still names its layer"), Report.Layers[1].LayerIndex, 1);
    TestEqual(TEXT("The in-range layer mixed the whole render"),
        Report.Layers[0].FramesMixed, 4800);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSynthRenderHardPanTest,
    "PinWright.audio.synth.render.HardPanLeavesTheOppositeChannelSilent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSynthRenderHardPanTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthRenderTestHelpers;

    // Exact zero, not "below the noise floor": cos(pi/2) is 6.12e-17 rather than 0, so an
    // unguarded equal-power law leaks a non-silent opposite channel and this is what catches it.
    FPwSynthRecipe HardLeft;
    FPwSynthRecipe HardRight;
    FString ParseError;
    if (!BuildRecipe(RecipeJson(0, 100.0, { OscLayerJson(TEXT("sine"), 800.0, -1.0) }),
            HardLeft, ParseError) ||
        !BuildRecipe(RecipeJson(0, 100.0, { OscLayerJson(TEXT("sine"), 800.0, 1.0) }),
            HardRight, ParseError))
    {
        AddError(FString::Printf(TEXT("fixture did not parse: %s"), *ParseError));
        return false;
    }

    FPwAudioBuffer LeftRender;
    FPwAudioBuffer RightRender;
    FPwRenderReport LeftReport;
    FPwRenderReport RightReport;
    FString Code;
    FString Error;
    if (!PwRenderRecipe(HardLeft, LeftRender, LeftReport, Code, Error) ||
        !PwRenderRecipe(HardRight, RightRender, RightReport, Code, Error))
    {
        AddError(FString::Printf(TEXT("render failed: [%s] %s"), *Code, *Error));
        return false;
    }

    TestTrue(TEXT("pan -1 leaves the right channel exactly silent"),
        AllExactlyZero(LeftRender.Right));
    TestTrue(TEXT("pan -1 still writes the left channel"), MaxAbs(LeftRender.Left, 0, MAX_int32) > 0.1f);
    TestTrue(TEXT("pan +1 leaves the left channel exactly silent"),
        AllExactlyZero(RightRender.Left));
    TestTrue(TEXT("pan +1 still writes the right channel"), MaxAbs(RightRender.Right, 0, MAX_int32) > 0.1f);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSynthRenderPeakNormalizeTest,
    "PinWright.audio.synth.render.PeakNormalizeHitsTheRequestedTarget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSynthRenderPeakNormalizeTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthRenderTestHelpers;

    constexpr double TargetDb = -6.0;

    // Layer gain is -20 dB so the normalizer has to BOOST; a target below the natural peak
    // would also pass on a renderer that only ever attenuates.
    FPwSynthRecipe Recipe;
    FString ParseError;
    if (!BuildRecipe(RecipeJson(0, 200.0,
            { OscLayerJson(TEXT("sine"), 1000.0, 0.0, 0.0, -20.0) },
            TEXT("{\"normalize\":{\"mode\":\"peak\",\"target\":-6.0}}")), Recipe, ParseError))
    {
        AddError(FString::Printf(TEXT("fixture did not parse: %s"), *ParseError));
        return false;
    }

    FPwAudioBuffer Rendered;
    FPwRenderReport Report;
    FString Code;
    FString Error;
    if (!PwRenderRecipe(Recipe, Rendered, Report, Code, Error))
    {
        AddError(FString::Printf(TEXT("render failed: [%s] %s"), *Code, *Error));
        return false;
    }

    const double MeasuredPeakDb = PeakDbOf(Rendered);

    TestTrue(FString::Printf(TEXT("The rendered peak reaches the -6 dBFS target (measured %.4f dBFS)"),
            MeasuredPeakDb),
        FMath::Abs(MeasuredPeakDb - TargetDb) < 0.01);

    // NormalizeGainDb is the gain that was APPLIED, so it must reconcile with the pre-normalize
    // peak the same report published. A hardcoded or requested-but-unapplied value fails here.
    TestTrue(FString::Printf(
            TEXT("NormalizeGainDb (%.4f) is the gain actually applied to the measured %.4f dBFS peak"),
            Report.NormalizeGainDb, Report.PeakDbBeforeNormalize),
        FMath::Abs((Report.PeakDbBeforeNormalize + Report.NormalizeGainDb) - TargetDb) < 1.0e-9);
    TestTrue(TEXT("The pre-normalize peak was measured, not floored"),
        Report.PeakDbBeforeNormalize > -30.0 && Report.PeakDbBeforeNormalize < -15.0);
    TestEqual(TEXT("Normalizing to -6 dBFS clips nothing"), Report.ClampedSamples, 0);

    // Peak mode keys on the peak, so the two published measurements must coincide - and must
    // be flagged as measured, not left at the 0.0 default.
    TestTrue(TEXT("Peak normalization records that it measured something"),
        Report.bNormalizeMeasured);
    TestTrue(TEXT("In Peak mode the normalizer's input IS the measured peak"),
        Report.NormalizeInputDb == Report.PeakDbBeforeNormalize);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSynthRenderFadesTest,
    "PinWright.audio.synth.render.FadesRemoveEdgeDiscontinuities",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSynthRenderFadesTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthRenderTestHelpers;

    constexpr int32 FadeFrames = 480;           // 10 ms at 48 kHz

    // A square wave, so the un-faded buffer would start and end on a full-scale step - the
    // discontinuity the fades exist to remove.
    FPwSynthRecipe Recipe;
    FString ParseError;
    if (!BuildRecipe(RecipeJson(0, 200.0,
            { OscLayerJson(TEXT("square"), 300.0, 0.0) },
            TEXT("{\"fadeInMs\":10.0,\"fadeOutMs\":10.0}")), Recipe, ParseError))
    {
        AddError(FString::Printf(TEXT("fixture did not parse: %s"), *ParseError));
        return false;
    }

    FPwAudioBuffer Rendered;
    FPwRenderReport Report;
    FString Code;
    FString Error;
    if (!PwRenderRecipe(Recipe, Rendered, Report, Code, Error))
    {
        AddError(FString::Printf(TEXT("render failed: [%s] %s"), *Code, *Error));
        return false;
    }

    const int32 Last = Rendered.NumFrames() - 1;
    TestTrue(TEXT("The first left sample is exactly zero"), Rendered.Left[0] == 0.f);
    TestTrue(TEXT("The first right sample is exactly zero"), Rendered.Right[0] == 0.f);
    TestTrue(TEXT("The last left sample is exactly zero"), Rendered.Left[Last] == 0.f);
    TestTrue(TEXT("The last right sample is exactly zero"), Rendered.Right[Last] == 0.f);

    // A fade, not a mute: the edges are attenuated relative to the body, and the body survives.
    const float BodyPeak = MaxAbs(Rendered.Left, FadeFrames, Rendered.NumFrames() - 2 * FadeFrames);
    TestTrue(TEXT("The body of the render is untouched"), BodyPeak > 0.1f);
    TestTrue(TEXT("The fade-in region is quieter than the body"),
        MaxAbs(Rendered.Left, 0, FadeFrames) < BodyPeak);
    TestTrue(TEXT("The fade-out region is quieter than the body"),
        MaxAbs(Rendered.Left, Rendered.NumFrames() - FadeFrames, FadeFrames) < BodyPeak);

    // This fixture carries no normalize block, so the normalizer measured nothing. The flag has
    // to say so rather than let the 0.0 default read as "measured 0 dB".
    TestFalse(TEXT("A recipe with no normalize block reports no normalize measurement"),
        Report.bNormalizeMeasured);
    TestTrue(TEXT("... and applied no normalize gain"), Report.NormalizeGainDb == 0.0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSynthRenderLufsNormalizeTest,
    "PinWright.audio.synth.render.LufsNormalizeReportsTheMeasuredLoudness",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSynthRenderLufsNormalizeTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthRenderTestHelpers;

    // Hard-panned 200 Hz so the loudness and the peak are provably DIFFERENT numbers: one
    // channel at amplitude 0.1 peaks at -20 dBFS but measures near -23 LUFS, and 200 Hz sits
    // where K-weighting is flat so the gap is not an artefact of the weighting curve. A
    // centre-panned sine would have been useless here - its peak and its LUFS coincide to
    // within hundredths of a dB, so a renderer that had silently fallen back to peak
    // normalization would have passed.
    //
    // 2 s clears the analyzer's 0.4 s window plus ramp-in at every supported sample rate.
    const TArray<FString> Layers = { OscLayerJson(TEXT("sine"), 200.0, -1.0, 0.0, -20.0) };

    FPwSynthRecipe First;
    FPwSynthRecipe Second;
    FString ParseError;
    if (!BuildRecipe(RecipeJson(0, 2000.0, Layers,
            TEXT("{\"normalize\":{\"mode\":\"lufs\",\"target\":-16.0}}")), First, ParseError) ||
        !BuildRecipe(RecipeJson(0, 2000.0, Layers,
            TEXT("{\"normalize\":{\"mode\":\"lufs\",\"target\":-23.0}}")), Second, ParseError))
    {
        AddError(FString::Printf(TEXT("fixture did not parse: %s"), *ParseError));
        return false;
    }

#if !UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
    // LUFS normalization measures with Audio::FLKFSAnalyzer, the engine's BS.1770 meter, which
    // ships from UE 5.8 only. The whole point of the mode split is that a target expressed in
    // LUFS is never met with a peak measurement, so on this engine the render must FAIL and say
    // why - the same direction as the too-short test below, for a different reason. The recipe
    // itself still parses: 'lufs' is a valid mode; it is this editor that cannot serve it.
    {
        FPwAudioBuffer Buffer;
        FPwRenderReport Report;
        FString RefusalCode;
        FString RefusalError;
        TestFalse(TEXT("a LUFS-normalized render is refused rather than served from the peak"),
            PwRenderRecipe(First, Buffer, Report, RefusalCode, RefusalError));
        TestFalse(TEXT("nothing claims to have been measured"), Report.bNormalizeMeasured);
        TestTrue(FString::Printf(TEXT("the refusal names the engine that carries the meter ([%s] %s)"),
            *RefusalCode, *RefusalError), RefusalError.Contains(TEXT("5.8")));
    }
    return true;
#else
    constexpr double FirstTarget = -16.0;
    constexpr double SecondTarget = -23.0;

    FPwAudioBuffer FirstBuffer;
    FPwAudioBuffer SecondBuffer;
    FPwRenderReport FirstReport;
    FPwRenderReport SecondReport;
    FString Code;
    FString Error;
    if (!PwRenderRecipe(First, FirstBuffer, FirstReport, Code, Error) ||
        !PwRenderRecipe(Second, SecondBuffer, SecondReport, Code, Error))
    {
        AddError(FString::Printf(TEXT("render failed: [%s] %s"), *Code, *Error));
        return false;
    }

    TestTrue(TEXT("LUFS normalization records that it measured something"),
        FirstReport.bNormalizeMeasured);
    TestTrue(TEXT("... in both renders"), SecondReport.bNormalizeMeasured);

    // Not the 0.0 default and not a sentinel: a real programme level.
    TestTrue(FString::Printf(TEXT("The measured loudness is a plausible level (%.3f LUFS)"),
            FirstReport.NormalizeInputDb),
        FirstReport.NormalizeInputDb < 0.0 && FirstReport.NormalizeInputDb > -60.0);

    // Not the peak wearing a loudness label. This is the assertion that catches a silent
    // fallback to peak normalization in LUFS mode - the §3 defect the mode split exists for.
    TestTrue(FString::Printf(
            TEXT("The loudness (%.3f LUFS) is a different measurement from the peak (%.3f dBFS)"),
            FirstReport.NormalizeInputDb, FirstReport.PeakDbBeforeNormalize),
        FMath::Abs(FirstReport.NormalizeInputDb - FirstReport.PeakDbBeforeNormalize) > 1.0);

    // Not a back-derivation of Target - NormalizeGainDb. The pre-normalize audio is identical
    // in the two renders, so the analyzer must return the identical number while the GAIN moves
    // with the target. A field computed from the target would track the target instead.
    TestTrue(TEXT("The same audio measures the same loudness whatever the target"),
        FirstReport.NormalizeInputDb == SecondReport.NormalizeInputDb);
    TestTrue(TEXT("The applied gains differ by exactly the difference between the targets"),
        FMath::Abs((FirstReport.NormalizeGainDb - SecondReport.NormalizeGainDb)
            - (FirstTarget - SecondTarget)) < 1.0e-9);

    // The arithmetic contract: measured + applied == requested.
    TestTrue(TEXT("measured LUFS + applied gain reaches the -16 LUFS target"),
        FMath::Abs((FirstReport.NormalizeInputDb + FirstReport.NormalizeGainDb) - FirstTarget) < 1.0e-9);
    TestTrue(TEXT("measured LUFS + applied gain reaches the -23 LUFS target"),
        FMath::Abs((SecondReport.NormalizeInputDb + SecondReport.NormalizeGainDb) - SecondTarget) < 1.0e-9);

    return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSynthRenderLufsTooShortTest,
    "PinWright.audio.synth.render.LufsNormalizeBelowTheAnalysisWindowIsRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSynthRenderLufsTooShortTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthRenderTestHelpers;

    // The parser has no opinion on render length versus the loudness analysis window, so this
    // recipe is valid and reaches the renderer. A render too short to measure must ERROR: a
    // quiet degradation to peak normalization would treat -16 LUFS as -16 dBFS and publish a
    // level the recipe never asked for (rpc-design.md §3).
    FPwSynthRecipe Recipe;
    FString ParseError;
    if (!BuildRecipe(RecipeJson(0, 100.0,
            { OscLayerJson(TEXT("sine"), 1000.0, 0.0) },
            TEXT("{\"normalize\":{\"mode\":\"lufs\",\"target\":-16.0}}")), Recipe, ParseError))
    {
        AddError(FString::Printf(TEXT("fixture did not parse: %s"), *ParseError));
        return false;
    }

    FPwAudioBuffer Rendered;
    FPwRenderReport Report;
    FString Code;
    FString Error;
    const bool bReturned = PwRenderRecipe(Recipe, Rendered, Report, Code, Error);

    CheckAbortedRender(*this, TEXT("A LUFS target on a render shorter than the analysis window"),
        bReturned, Rendered, Report, Code, Error, ErrorCodes::ERR_INVALID_RECIPE,
        TEXT("master.normalize"));

    return true;
}

// =========================================================================
// Failure direction (rpc-design.md §12) - see CheckAbortedRender above for what each of these
// asserts beyond the error code.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSynthRenderUnspecifiedGeneratorTest,
    "PinWright.audio.synth.render.UnspecifiedGeneratorAbortsTheRender",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSynthRenderUnspecifiedGeneratorTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthRenderTestHelpers;

    FPwSynthRecipe Recipe;
    FString ParseError;
    if (!BuildRecipe(RecipeJson(0, 100.0, { OscLayerJson(TEXT("sine"), 440.0, 0.0) }),
            Recipe, ParseError))
    {
        AddError(FString::Printf(TEXT("fixture did not parse: %s"), *ParseError));
        return false;
    }

    // The zero value of the kind enum is a failure by construction (rpc-design.md §2); the
    // renderer must not degrade it into "some default oscillator".
    Recipe.Layers[0].Generator.Kind = EPwSynthGeneratorKind::Unspecified;

    FPwAudioBuffer Rendered;
    FPwRenderReport Report;
    FString Code;
    FString Error;
    const bool bReturned = PwRenderRecipe(Recipe, Rendered, Report, Code, Error);

    CheckAbortedRender(*this, TEXT("An Unspecified generator kind"), bReturned, Rendered, Report,
        Code, Error, ErrorCodes::ERR_UNKNOWN_GENERATOR, TEXT("layers[0].generator"));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSynthRenderUnknownEffectTest,
    "PinWright.audio.synth.render.UnknownEffectAbortsTheRender",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSynthRenderUnknownEffectTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthRenderTestHelpers;

    FPwSynthRecipe Recipe;
    FString ParseError;
    if (!BuildRecipe(RecipeJson(0, 100.0, { OscLayerJson(TEXT("sine"), 440.0, 0.0) }),
            Recipe, ParseError))
    {
        AddError(FString::Printf(TEXT("fixture did not parse: %s"), *ParseError));
        return false;
    }

    // An unimplemented effect must not pass the dry signal through: a silently skipped effect is
    // indistinguishable from one that had no audible result.
    FPwSynthFx UnknownFx;
    UnknownFx.Kind = EPwSynthFxKind::Unspecified;
    Recipe.Layers[0].Fx.Add(UnknownFx);

    FPwAudioBuffer Rendered;
    FPwRenderReport Report;
    FString Code;
    FString Error;
    const bool bReturned = PwRenderRecipe(Recipe, Rendered, Report, Code, Error);

    CheckAbortedRender(*this, TEXT("An unknown layer effect"), bReturned, Rendered, Report,
        Code, Error, ErrorCodes::ERR_UNKNOWN_EFFECT, TEXT("layers[0].fx[0]"));

    // Same rule on the master chain, where the span is stereo rather than mono.
    Recipe.Layers[0].Fx.Reset();
    Recipe.Master.Fx.Add(UnknownFx);

    FPwAudioBuffer MasterRendered;
    FPwRenderReport MasterReport;
    FString MasterCode;
    FString MasterError;
    const bool bMasterReturned =
        PwRenderRecipe(Recipe, MasterRendered, MasterReport, MasterCode, MasterError);

    CheckAbortedRender(*this, TEXT("An unknown master effect"), bMasterReturned, MasterRendered,
        MasterReport, MasterCode, MasterError, ErrorCodes::ERR_UNKNOWN_EFFECT, TEXT("master.fx[0]"));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSynthRenderGeneratorFailureTest,
    "PinWright.audio.synth.render.GeneratorFailureAbortsTheRender",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSynthRenderGeneratorFailureTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthRenderTestHelpers;

    FPwSynthRecipe Recipe;
    FString ParseError;
    if (!BuildRecipe(RecipeJson(0, 100.0, { OscLayerJson(TEXT("sine"), 440.0, 0.0) }),
            Recipe, ParseError))
    {
        AddError(FString::Printf(TEXT("fixture did not parse: %s"), *ParseError));
        return false;
    }

    // A recognised generator kind whose parameter bag is empty. `waveform` and `frequencyHz` are
    // required with no documented default (rpc-design.md §3), so the generator itself must
    // reject this rather than invent a 440 Hz sine. The exact code is the generator chunk's to
    // choose, so this asserts the propagation contract rather than the spelling: the render
    // aborts, some code is set, the message is prefixed with the offending field path, and
    // nothing partial survives.
    Recipe.Layers[0].Generator.Params.Values.Empty();

    FPwAudioBuffer Rendered;
    FPwRenderReport Report;
    FString Code;
    FString Error;
    const bool bReturned = PwRenderRecipe(Recipe, Rendered, Report, Code, Error);

    TestFalse(TEXT("A generator that rejects its own parameters aborts the render"), bReturned);
    TestFalse(TEXT("The failure carries an error code"), Code.IsEmpty());
    TestTrue(TEXT("The message names the failing layer's generator: ") + Error,
        Error.Contains(TEXT("layers[0].generator")));
    TestEqual(TEXT("The output buffer is empty"), Rendered.NumFrames(), 0);
    TestFalse(TEXT("The report is unmeasured"), Report.bMeasured);
    TestEqual(TEXT("No partial layer rows survive"), Report.Layers.Num(), 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSynthRenderMasterOnlyEffectTest,
    "PinWright.audio.synth.render.MasterOnlyEffectInALayerChainIsRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSynthRenderMasterOnlyEffectTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthRenderTestHelpers;

    FPwSynthRecipe Recipe;
    FString ParseError;
    if (!BuildRecipe(RecipeJson(0, 100.0, { OscLayerJson(TEXT("sine"), 440.0, 0.0) }),
            Recipe, ParseError))
    {
        AddError(FString::Printf(TEXT("fixture did not parse: %s"), *ParseError));
        return false;
    }

    // `width` is mid/side and a layer chain is mono, so running it there would do exactly
    // nothing - the quietest possible failure. The parser rejects it; so must the renderer,
    // which is also reachable with a hand-built recipe.
    FPwSynthFx Width;
    Width.Kind = EPwSynthFxKind::Width;
    Recipe.Layers[0].Fx.Add(Width);

    FPwAudioBuffer Rendered;
    FPwRenderReport Report;
    FString Code;
    FString Error;
    const bool bReturned = PwRenderRecipe(Recipe, Rendered, Report, Code, Error);

    CheckAbortedRender(*this, TEXT("A master-only effect in a layer chain"), bReturned, Rendered,
        Report, Code, Error, ErrorCodes::ERR_INVALID_RECIPE, TEXT("layers[0].fx[0]"));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSynthRenderEmptyRecipeTest,
    "PinWright.audio.synth.render.EmptyRecipeIsAnErrorNotSilence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSynthRenderEmptyRecipeTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthRenderTestHelpers;

    // A zero-length render and a silent one are different outcomes, and a recipe with no layers
    // produced nothing rather than produced quiet - both are errors, not buffers of zeros.
    FPwSynthRecipe NoLayers;
    NoLayers.SampleRate = TestSampleRate;
    NoLayers.DurationMs = 100.0;

    FPwAudioBuffer Rendered;
    FPwRenderReport Report;
    FString Code;
    FString Error;
    TestFalse(TEXT("A recipe with no layers is rejected"),
        PwRenderRecipe(NoLayers, Rendered, Report, Code, Error));
    TestEqual(TEXT("... as AUDIO_EMPTY_BUFFER"), Code, FString(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER));
    TestEqual(TEXT("... leaving no buffer"), Rendered.NumFrames(), 0);
    TestFalse(TEXT("... and an unmeasured report"), Report.bMeasured);

    FPwSynthRecipe ZeroDuration;
    ZeroDuration.SampleRate = TestSampleRate;
    ZeroDuration.DurationMs = 0.0;
    ZeroDuration.Layers.AddDefaulted();

    TestFalse(TEXT("A zero-length render is rejected"),
        PwRenderRecipe(ZeroDuration, Rendered, Report, Code, Error));
    TestEqual(TEXT("... as AUDIO_EMPTY_BUFFER"), Code, FString(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER));
    TestEqual(TEXT("... leaving no buffer"), Rendered.NumFrames(), 0);
    TestFalse(TEXT("... and an unmeasured report"), Report.bMeasured);

    return true;
}
