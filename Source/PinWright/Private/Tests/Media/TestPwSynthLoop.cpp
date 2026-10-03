// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the seamless-loop render (master.loopCrossfadeMs, PwRenderRecipe's overhang fold)
// and the analysis scalar that measures a loop's wrap (technical.loopSeamRatio).
//
// Offline DSP only: parse -> PwRenderRecipe -> PwAnalyzeBuffer. No audio device, no world.
//
// The failure direction is a sine whose period does not divide durationMs: rendered as a
// one-shot its last sample sits a quarter cycle away from its first, so the wrap is a
// full-scale step (loopSeamRatio ~100). Folded, the first sample IS the continuation of the
// last, so the wrap is an ordinary step of the sine (ratio <= sqrt 2). Reverting the fold puts
// the looped render back at ~100.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AudioGen/PwAudioAnalysis.h"
#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwSynthDsp.h"
#include "AudioGen/PwSynthRecipe.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Named (not anonymous) namespace: the module builds with bUseUnity = true.
namespace PwSynthLoopTestHelpers
{
    bool ParseJson(const FString& Json, FPwSynthRecipe& Out, FPwSynthRecipeError& OutError)
    {
        TSharedPtr<FJsonObject> Root;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
        if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
        {
            OutError.Code = TEXT("NOT_JSON");
            return false;
        }
        return ParseSynthRecipe(Root, Out, OutError);
    }

    /** Parse + render + analyze; false (with an AddError) when any stage fails. */
    bool RenderAndAnalyze(FAutomationTestBase& Test, const FString& Json, FPwAudioBuffer& OutBuffer,
        FPwAudioAnalysis& OutAnalysis)
    {
        FPwSynthRecipe Recipe;
        FPwSynthRecipeError ParseError;
        if (!ParseJson(Json, Recipe, ParseError))
        {
            Test.AddError(FString::Printf(TEXT("fixture did not parse: %s"), *ParseError.ToString()));
            return false;
        }
        FPwRenderReport Report;
        FString Code, Error;
        if (!PwRenderRecipe(Recipe, OutBuffer, Report, Code, Error))
        {
            Test.AddError(FString::Printf(TEXT("render failed: [%s] %s"), *Code, *Error));
            return false;
        }
        if (!PwAnalyzeBuffer(OutBuffer, OutAnalysis, Code, Error))
        {
            Test.AddError(FString::Printf(TEXT("analysis failed: [%s] %s"), *Code, *Error));
            return false;
        }
        return true;
    }

    // 101.25 Hz over 1000 ms is 101.25 cycles: the one-shot ends a quarter cycle off, and the
    // folded segments are in quadrature (r ~ 0). 100 Hz is whole cycles: the overhang repeats
    // the head exactly (r = 1), where a fixed equal-power law would bump the middle by 3 dB.
    FString SineDroneJson(double FrequencyHz, double LoopCrossfadeMs)
    {
        return FString::Printf(
            TEXT("{\"seed\":1,\"sampleRate\":48000,\"durationMs\":1000,\"layers\":[{\"gainDb\":-6,")
            TEXT("\"generator\":{\"kind\":\"osc\",\"params\":{\"waveform\":\"sine\",\"frequencyHz\":%g}}}],")
            TEXT("\"master\":{\"loopCrossfadeMs\":%g}}"), FrequencyHz, LoopCrossfadeMs);
    }

    double RmsDb(const TArray<float>& Channel, int32 Start, int32 Count)
    {
        double Sum = 0.0;
        for (int32 Index = Start; Index < Start + Count; ++Index)
        {
            Sum += static_cast<double>(Channel[Index]) * Channel[Index];
        }
        return 10.0 * FMath::LogX(10.0, FMath::Max(Sum / Count, 1e-20));
    }

    /** Worst |10 ms RMS - body RMS| over the first 200 ms (the crossfade region), dB. */
    double WorstCrossfadeDeviationDb(const FPwAudioBuffer& Looped)
    {
        const double BodyDb = RmsDb(Looped.Left, 24000, 4800);
        double Worst = 0.0;
        for (int32 Start = 0; Start + 480 <= 9600; Start += 480)
        {
            Worst = FMath::Max(Worst, FMath::Abs(RmsDb(Looped.Left, Start, 480) - BodyDb));
        }
        return Worst;
    }

    // Verbatim the wind bed in docs/wiki-src/audio.synth.cookbook.md "Ambience and loops". Keep
    // them identical: this test is what makes the page's "it loops" claim true.
    const TCHAR* const CookbookWindBed = TEXT(R"json({
  "durationMs": 12000,
  "seed": 21,
  "layers": [
    {
      "gainDb": -3, "pan": -0.2,
      "generator": { "kind": "noise", "params": { "color": "brown", "lowCutHz": 40, "highCutHz": 900 } },
      "ampEnvelope": [
        { "timeMs": 0, "value": 0.55, "curve": "scurve" }, { "timeMs": 2500, "value": 0.9, "curve": "scurve" },
        { "timeMs": 5000, "value": 0.45, "curve": "scurve" }, { "timeMs": 8000, "value": 1.0, "curve": "scurve" },
        { "timeMs": 10500, "value": 0.6, "curve": "scurve" }, { "timeMs": 12000, "value": 0.55 }
      ]
    },
    {
      "gainDb": -10, "pan": 0.25,
      "generator": { "kind": "noise", "params": { "color": "pink", "lowCutHz": 300, "highCutHz": 2500 } },
      "ampEnvelope": [
        { "timeMs": 0, "value": 0.5, "curve": "scurve" }, { "timeMs": 2900, "value": 0.85, "curve": "scurve" },
        { "timeMs": 5400, "value": 0.4, "curve": "scurve" }, { "timeMs": 8400, "value": 0.95, "curve": "scurve" },
        { "timeMs": 10900, "value": 0.55, "curve": "scurve" }, { "timeMs": 12000, "value": 0.5 }
      ]
    },
    {
      "gainDb": -24,
      "generator": { "kind": "noise", "params": { "color": "white", "lowCutHz": 2500, "highCutHz": 7000 } },
      "modulation": { "am": { "depth": 0.5, "rateHz": 0.25 } }
    }
  ],
  "master": {
    "normalize": { "mode": "lufs", "target": -20 },
    "loopCrossfadeMs": 500
  }
})json");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSynthLoopSeamTest,
    "PinWright.audio.synth.loop.FoldMakesTheWrapAnOrdinaryStep",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSynthLoopSeamTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthLoopTestHelpers;

    FPwAudioBuffer OneShot, Looped;
    FPwAudioAnalysis OneShotAnalysis, LoopedAnalysis;
    FPwAudioBuffer Drone;
    FPwAudioAnalysis DroneAnalysis;
    if (!RenderAndAnalyze(*this, SineDroneJson(101.25, 0.0), OneShot, OneShotAnalysis) ||
        !RenderAndAnalyze(*this, SineDroneJson(101.25, 200.0), Looped, LoopedAnalysis) ||
        !RenderAndAnalyze(*this, SineDroneJson(100.0, 200.0), Drone, DroneAnalysis))
    {
        return false;
    }

    TestEqual(TEXT("the loop is exactly durationMs long, not durationMs + crossfade"),
        Looped.NumFrames(), 48000);

    // Precondition: the fixture really has a broken wrap when rendered as a one-shot.
    const TOptional<double> OneShotRatio = OneShotAnalysis.Technical.LoopSeamRatio;
    TestTrue(TEXT("the one-shot reports a seam ratio"), OneShotRatio.IsSet());
    if (OneShotRatio.IsSet())
    {
        TestTrue(FString::Printf(TEXT("the one-shot wraps with a step far outside the sine's own (%.2f)"),
            OneShotRatio.GetValue()), OneShotRatio.GetValue() > 50.0);
    }

    const TOptional<double> LoopedRatio = LoopedAnalysis.Technical.LoopSeamRatio;
    TestTrue(TEXT("the loop reports a seam ratio"), LoopedRatio.IsSet());
    if (LoopedRatio.IsSet())
    {
        // A sine's largest adjacent step is sqrt(2) x its RMS step.
        TestTrue(FString::Printf(TEXT("the folded wrap is an ordinary sine step (%.2f)"),
            LoopedRatio.GetValue()), LoopedRatio.GetValue() <= 1.5);
    }

    // Not seam-free by silence: both edges carry signal (a fade would make them 0).
    TestTrue(TEXT("the loop's first sample is not faded to zero"),
        LoopedAnalysis.Technical.StartDiscontinuity > 0.05);

    // The fold neither bumps nor dips the level, on either crossfade law.
    const double QuadratureDb = WorstCrossfadeDeviationDb(Looped);
    TestTrue(FString::Printf(TEXT("r ~ 0: the crossfade region keeps the body's level (worst %.2f dB)"),
        QuadratureDb), QuadratureDb < 0.5);
    const double DroneDb = WorstCrossfadeDeviationDb(Drone);
    TestTrue(FString::Printf(TEXT("r = 1 (whole-cycle drone): no equal-power bump (worst %.2f dB)"),
        DroneDb), DroneDb < 0.5);
    const TOptional<double> DroneRatio = DroneAnalysis.Technical.LoopSeamRatio;
    TestTrue(TEXT("the whole-cycle drone wraps as an ordinary step"),
        DroneRatio.IsSet() && DroneRatio.GetValue() <= 1.5);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSynthLoopCookbookWindTest,
    "PinWright.audio.synth.loop.CookbookWindBedIsSeamless",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSynthLoopCookbookWindTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthLoopTestHelpers;

    FPwAudioBuffer Buffer;
    FPwAudioAnalysis Analysis;
    if (!RenderAndAnalyze(*this, CookbookWindBed, Buffer, Analysis))
    {
        return false;
    }

    const int32 Frames = Buffer.NumFrames();
    TestEqual(TEXT("12 s at 48 kHz"), Frames, 12 * 48000);

    const TOptional<double> Ratio = Analysis.Technical.LoopSeamRatio;
    TestTrue(TEXT("the bed reports a seam ratio"), Ratio.IsSet());
    if (Ratio.IsSet())
    {
        TestTrue(FString::Printf(TEXT("the wrap is a step the bed already contains (loopSeamRatio %.2f)"),
            Ratio.GetValue()), Ratio.GetValue() < 3.0);
    }
    TestTrue(TEXT("the edges are not faded to silence (that would duck once per loop)"),
        Analysis.Technical.StartDiscontinuity > 0.0 && Analysis.Technical.EndDiscontinuity > 0.0);

    // Level continuity across the wrap: 1 s before the end against 1 s after the start (brown
    // noise needs a long window before its RMS estimate settles).
    constexpr int32 Window = 48000;
    const double TailDb = RmsDb(Buffer.Left, Frames - Window, Window);
    const double HeadDb = RmsDb(Buffer.Left, 0, Window);
    TestTrue(FString::Printf(TEXT("no level step across the wrap (tail %.2f dB, head %.2f dB)"),
        TailDb, HeadDb), FMath::Abs(TailDb - HeadDb) < 3.0);

    // Control: the same bed hard-cut (loopCrossfadeMs 0). One seed's wrap step is a single
    // random draw, so the claim is checked across eight seeds rather than pinned to one.
    double MaxHardCutRatio = 0.0;
    int32 HardCutClicks = 0;
    FString Measured;
    for (int32 Seed = 21; Seed < 29; ++Seed)
    {
        FString HardCut = FString(CookbookWindBed)
            .Replace(TEXT("\"loopCrossfadeMs\": 500"), TEXT("\"loopCrossfadeMs\": 0"))
            .Replace(TEXT("\"seed\": 21"), *FString::Printf(TEXT("\"seed\": %d"), Seed));
        FPwAudioBuffer HardCutBuffer;
        FPwAudioAnalysis HardCutAnalysis;
        if (!RenderAndAnalyze(*this, HardCut, HardCutBuffer, HardCutAnalysis)) return false;
        const double HardCutRatio = HardCutAnalysis.Technical.LoopSeamRatio.Get(0.0);
        MaxHardCutRatio = FMath::Max(MaxHardCutRatio, HardCutRatio);
        HardCutClicks += HardCutRatio > 3.0 ? 1 : 0;
        Measured += FString::Printf(TEXT(" %.1f"), HardCutRatio);
    }
    TestTrue(FString::Printf(TEXT("hard-cut wraps exceed the gate on most seeds (%d of 8:%s)"),
        HardCutClicks, *Measured), HardCutClicks >= 5);
    TestTrue(FString::Printf(TEXT("a hard-cut wrap reaches the tens (max %.1f)"), MaxHardCutRatio),
        MaxHardCutRatio > 10.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSynthLoopParserTest,
    "PinWright.audio.synth.loop.ParserGuardsAndRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSynthLoopParserTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthLoopTestHelpers;

    const FString Layer = TEXT("[{\"generator\":{\"kind\":\"noise\",\"params\":{\"color\":\"pink\"}}}]");
    const auto Recipe = [&Layer](const TCHAR* Master)
    {
        return FString::Printf(TEXT("{\"durationMs\":1000,\"layers\":%s,\"master\":%s}"), *Layer, Master);
    };

    FPwSynthRecipe Parsed;
    FPwSynthRecipeError Error;

    TestFalse(TEXT("a loop with a fade is rejected"),
        ParseJson(Recipe(TEXT("{\"loopCrossfadeMs\":100,\"fadeOutMs\":20}")), Parsed, Error));
    TestEqual(TEXT("... at the loop field"), Error.Field, FString(TEXT("master.loopCrossfadeMs")));

    Error.Reset();
    TestFalse(TEXT("a crossfade over half the duration is rejected"),
        ParseJson(Recipe(TEXT("{\"loopCrossfadeMs\":600}")), Parsed, Error));
    TestEqual(TEXT("... at the loop field"), Error.Field, FString(TEXT("master.loopCrossfadeMs")));

    Error.Reset();
    TestFalse(TEXT("a sub-millisecond crossfade (which would round to no fold) is rejected"),
        ParseJson(Recipe(TEXT("{\"loopCrossfadeMs\":0.01}")), Parsed, Error));

    Error.Reset();
    if (!ParseJson(Recipe(TEXT("{\"loopCrossfadeMs\":250}")), Parsed, Error))
    {
        AddError(FString::Printf(TEXT("a legal loop did not parse: %s"), *Error.ToString()));
        return false;
    }
    TestEqual(TEXT("parsed"), Parsed.Master.LoopCrossfadeMs, 250.0);

    const TSharedPtr<FJsonObject> Serialized = SerializeSynthRecipe(Parsed);
    const TSharedPtr<FJsonObject>* Master = nullptr;
    TestTrue(TEXT("serialized master block"), Serialized.IsValid() && Serialized->TryGetObjectField(TEXT("master"), Master));
    double RoundTripped = -1.0;
    TestTrue(TEXT("loopCrossfadeMs is serialized"),
        Master && (*Master)->TryGetNumberField(TEXT("loopCrossfadeMs"), RoundTripped));
    TestEqual(TEXT("and round-trips"), RoundTripped, 250.0);

    FPwSynthRecipe OneShot;
    Error.Reset();
    TestTrue(TEXT("an omitted field parses"), ParseJson(Recipe(TEXT("{}")), OneShot, Error));
    TestEqual(TEXT("and defaults to one-shot"), OneShot.Master.LoopCrossfadeMs, 0.0);
    return true;
}
