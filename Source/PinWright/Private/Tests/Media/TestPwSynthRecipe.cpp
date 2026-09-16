// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the audio.synth recipe contract (AudioGen/PwSynthRecipe.h) and the
// audio.synth.describe_schema verb.
//
// Weighted toward the FAILURE direction on purpose (rpc-design §12). A recipe
// parser that succeeds is easy; the contract that matters is that an
// unrecognised kind, a missing required parameter or a cap violation ERRORS and
// leaves the caller's recipe untouched, instead of quietly becoming a default
// oscillator, an invented 440 Hz or a truncated chain. Every negative test below
// asserts both halves: the error code / field path, AND that the out-parameter
// still holds the sentinel it was given. Revert any of those rejections to a
// silent fallback and these break.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Tests/TestUtils.h"

#include "AudioGen/PwSynthRecipe.h"
#include "Handlers/ErrorCodes.h"
#include "Utils/HttpResponseSpill.h"

namespace
{
    TSharedPtr<FJsonObject> PwSynthTestParseJson(const FString& Text)
    {
        TSharedPtr<FJsonObject> Object;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
        FJsonSerializer::Deserialize(Reader, Object);
        return Object;
    }

    FString PwSynthTestToJson(const TSharedPtr<FJsonObject>& Object)
    {
        FString Text;
        if (!Object.IsValid())
        {
            return Text;
        }
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
        FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
        Writer->Close();
        return Text;
    }

    // A recipe that is deliberately NOT the one describe_schema publishes, so a
    // sentinel left in an out-parameter is unmistakable.
    FPwSynthRecipe PwSynthTestSentinel()
    {
        FPwSynthRecipe Sentinel;
        Sentinel.Seed = 987654;
        Sentinel.SampleRate = 22050;
        Sentinel.DurationMs = 4321.0;
        FPwSynthLayer Layer;
        Layer.GainDb = -42.0;
        Layer.Generator.Kind = EPwSynthGeneratorKind::Sample;
        Sentinel.Layers.Add(Layer);
        return Sentinel;
    }

    // Asserts the out-parameter still holds exactly what PwSynthTestSentinel put
    // there. This is the "leaves Out untouched" half of rpc-design §1: a partially
    // parsed recipe must never be observable.
    void PwSynthTestExpectUntouched(FAutomationTestBase& Test, const TCHAR* Label, const FPwSynthRecipe& Out)
    {
        Test.TestEqual(FString::Printf(TEXT("%s: Out.Seed untouched"), Label), Out.Seed, 987654);
        Test.TestEqual(FString::Printf(TEXT("%s: Out.SampleRate untouched"), Label), Out.SampleRate, 22050);
        Test.TestEqual(FString::Printf(TEXT("%s: Out.DurationMs untouched"), Label), Out.DurationMs, 4321.0);
        Test.TestEqual(FString::Printf(TEXT("%s: Out.Layers untouched"), Label), Out.Layers.Num(), 1);
        if (Out.Layers.Num() == 1)
        {
            Test.TestEqual(FString::Printf(TEXT("%s: Out layer gain untouched"), Label), Out.Layers[0].GainDb, -42.0);
            Test.TestTrue(FString::Printf(TEXT("%s: Out generator kind untouched"), Label),
                Out.Layers[0].Generator.Kind == EPwSynthGeneratorKind::Sample);
        }
    }

    // Parses Json, asserts it FAILED with the given code and field path, and that
    // the out-parameter is still the sentinel.
    void PwSynthTestExpectFailure(FAutomationTestBase& Test, const TCHAR* Label, const FString& Json,
        const TCHAR* ExpectedCode, const TCHAR* ExpectedField)
    {
        TSharedPtr<FJsonObject> Object = PwSynthTestParseJson(Json);
        Test.TestTrue(FString::Printf(TEXT("%s: fixture is valid JSON"), Label), Object.IsValid());
        if (!Object.IsValid())
        {
            return;
        }

        FPwSynthRecipe Out = PwSynthTestSentinel();
        FPwSynthRecipeError Error;
        const bool bParsed = ParseSynthRecipe(Object, Out, Error);

        Test.TestFalse(FString::Printf(TEXT("%s: parse reports failure"), Label), bParsed);
        Test.TestEqual(FString::Printf(TEXT("%s: error code"), Label), Error.Code, FString(ExpectedCode));
        Test.TestEqual(FString::Printf(TEXT("%s: error field path"), Label), Error.Field, FString(ExpectedField));
        Test.TestFalse(FString::Printf(TEXT("%s: error carries a message"), Label), Error.Message.IsEmpty());
        Test.TestTrue(FString::Printf(TEXT("%s: ToString names the field"), Label),
            Error.ToString().Contains(ExpectedField));
        PwSynthTestExpectUntouched(Test, Label, Out);
    }

    // Smallest recipe the schema accepts: one osc layer, both required params.
    const TCHAR* const PwSynthTestMinimalRecipe = TEXT(R"JSON(
{"durationMs":100,"layers":[{"generator":{"kind":"osc","params":{"waveform":"sine","frequencyHz":440}}}]}
)JSON");

    FString PwSynthTestMinimalLayer()
    {
        return TEXT(R"({"generator":{"kind":"osc","params":{"waveform":"sine","frequencyHz":440}}})");
    }

    FString PwSynthTestRepeatedLayers(int32 Count)
    {
        TArray<FString> Layers;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Layers.Add(PwSynthTestMinimalLayer());
        }
        return FString::Printf(TEXT("{\"durationMs\":100,\"layers\":[%s]}"), *FString::Join(Layers, TEXT(",")));
    }

    FString PwSynthTestRepeatedFx(int32 Count)
    {
        TArray<FString> Fx;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Fx.Add(TEXT(R"({"kind":"gain","params":{"gainDb":0}})"));
        }
        return FString::Join(Fx, TEXT(","));
    }

    // The full-feature fixture used by the round-trip test: two layers, both
    // envelope kinds, modulation, per-layer and master effect chains,
    // normalization, fades and metric targets.
    const TCHAR* const PwSynthTestRichRecipe = TEXT(R"JSON(
{
  "durationMs": 900,
  "seed": 7,
  "sampleRate": 44100,
  "layers": [
    {
      "gainDb": -3,
      "pan": -0.2,
      "generator": { "kind": "osc", "params": { "waveform": "saw", "frequencyHz": 880, "unison": 3 } },
      "ampEnvelope": [
        { "timeMs": 0, "value": 0, "curve": "linear" },
        { "timeMs": 8, "value": 1, "curve": "exp" },
        { "timeMs": 600, "value": 0 }
      ],
      "pitchEnvelope": [
        { "timeMs": 0, "semitones": 12 },
        { "timeMs": 600, "semitones": -24 }
      ],
      "modulation": { "fm": { "depth": 2.5, "rateHz": 55 } },
      "fx": [
        { "kind": "filter", "params": { "type": "lowpass", "cutoffHz": 6000, "resonance": 3 } },
        { "kind": "distort", "params": { "type": "soft", "drive": 6 } }
      ]
    },
    {
      "startMs": 25,
      "gainDb": -9,
      "pan": 0.3,
      "generator": { "kind": "modal", "params": {
        "modeFreqsHz": [220, 611, 1290],
        "modeDecaysMs": [900, 400, 150],
        "modeGainsDb": [0, -6, -14],
        "exciter": "strike" } },
      "ampEnvelope": [ { "timeMs": 0, "value": 1, "curve": "exp" }, { "timeMs": 120, "value": 0 } ]
    }
  ],
  "master": {
    "fx": [ { "kind": "reverb", "params": { "decayMs": 700, "mix": 0.18 } },
            { "kind": "width", "params": { "width": 1.4 } } ],
    "normalize": { "mode": "lufs", "target": -16.5 },
    "fadeInMs": 2,
    "fadeOutMs": 15
  },
  "targets": { "peakDb": { "min": -3, "max": -0.5 }, "durationMs": { "max": 1000 }, "lufs": { "min": -18 } }
}
)JSON");
}

// =========================================================================
// A. Round-trip fidelity. Serialize(Parse(x)) must survive a second
//    parse/serialize byte-identically, because wave 3's patch verb applies
//    JSON Patch to serialized output and re-parses it - a lossy serialize
//    breaks the iteration loop outright.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSynthRecipeRoundTripTest,
    "PinWright.audio.synth.recipe.RoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSynthRecipeRoundTripTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Authored = PwSynthTestParseJson(PwSynthTestRichRecipe);
    TestTrue(TEXT("fixture is valid JSON"), Authored.IsValid());
    if (!Authored.IsValid())
    {
        return false;
    }

    FPwSynthRecipe First;
    FPwSynthRecipeError Error;
    const bool bFirst = ParseSynthRecipe(Authored, First, Error);
    TestTrue(FString::Printf(TEXT("rich fixture parses (%s)"), *Error.ToString()), bFirst);
    if (!bFirst)
    {
        return false;
    }

    // Authored values survive the parse verbatim.
    TestEqual(TEXT("durationMs"), First.DurationMs, 900.0);
    TestEqual(TEXT("seed"), First.Seed, 7);
    TestEqual(TEXT("sampleRate"), First.SampleRate, 44100);
    TestEqual(TEXT("version defaults to the current revision"), First.Version, PwSynthLimits::RecipeVersion);
    TestEqual(TEXT("layer count"), First.Layers.Num(), 2);
    if (First.Layers.Num() != 2)
    {
        return false;
    }

    const FPwSynthLayer& L0 = First.Layers[0];
    TestTrue(TEXT("layer 0 generator is osc"), L0.Generator.Kind == EPwSynthGeneratorKind::Osc);
    TestEqual(TEXT("layer 0 waveform"), L0.Generator.Params.GetString(FName(TEXT("waveform"))), FString(TEXT("saw")));
    TestEqual(TEXT("layer 0 frequencyHz"), L0.Generator.Params.GetNumber(FName(TEXT("frequencyHz"))), 880.0);
    TestEqual(TEXT("layer 0 unison"), L0.Generator.Params.GetInt(FName(TEXT("unison"))), 3);
    // A documented default is materialized rather than left absent, which is what
    // makes serialize lossless.
    TestEqual(TEXT("layer 0 pulseWidth materialized from its default"),
        L0.Generator.Params.GetNumber(FName(TEXT("pulseWidth"))), 0.5);
    TestEqual(TEXT("layer 0 startMs defaults to 0"), L0.StartMs, 0.0);
    TestEqual(TEXT("layer 0 gainDb"), L0.GainDb, -3.0);
    TestEqual(TEXT("layer 0 pan"), L0.Pan, -0.2);
    TestEqual(TEXT("layer 0 amp envelope points"), L0.AmpEnvelope.Num(), 3);
    if (L0.AmpEnvelope.Num() == 3)
    {
        TestTrue(TEXT("amp point 1 curve is exp"), L0.AmpEnvelope[1].Curve == EPwSynthCurve::Exp);
        TestTrue(TEXT("amp point 2 curve defaults to linear"), L0.AmpEnvelope[2].Curve == EPwSynthCurve::Linear);
        TestEqual(TEXT("amp point 2 timeMs"), L0.AmpEnvelope[2].TimeMs, 600.0);
    }
    TestEqual(TEXT("layer 0 pitch envelope points"), L0.PitchEnvelope.Num(), 2);
    TestTrue(TEXT("layer 0 modulation routes fm"), L0.Modulation.Routing == EPwSynthModulationRouting::Fm);
    TestEqual(TEXT("layer 0 modulation depth"), L0.Modulation.Depth, 2.5);
    TestTrue(TEXT("layer 0 modulation source defaults to sine"), L0.Modulation.Source == EPwSynthModSource::Sine);
    TestEqual(TEXT("layer 0 fx count"), L0.Fx.Num(), 2);
    if (L0.Fx.Num() == 2)
    {
        TestTrue(TEXT("layer 0 fx[0] is filter"), L0.Fx[0].Kind == EPwSynthFxKind::Filter);
        TestEqual(TEXT("layer 0 fx[0] cutoffHz"), L0.Fx[0].Params.GetNumber(FName(TEXT("cutoffHz"))), 6000.0);
        TestTrue(TEXT("layer 0 fx[1] is distort"), L0.Fx[1].Kind == EPwSynthFxKind::Distort);
        TestEqual(TEXT("layer 0 fx[1] mix defaults to fully wet"),
            L0.Fx[1].Params.GetNumber(FName(TEXT("mix"))), 1.0);
    }

    const FPwSynthLayer& L1 = First.Layers[1];
    TestTrue(TEXT("layer 1 generator is modal"), L1.Generator.Kind == EPwSynthGeneratorKind::Modal);
    TestEqual(TEXT("layer 1 startMs"), L1.StartMs, 25.0);
    const TArray<double>* Freqs = L1.Generator.Params.GetNumbers(FName(TEXT("modeFreqsHz")));
    TestNotNull(TEXT("layer 1 modeFreqsHz present"), Freqs);
    if (Freqs)
    {
        TestEqual(TEXT("layer 1 mode count"), Freqs->Num(), 3);
        if (Freqs->Num() == 3)
        {
            TestEqual(TEXT("layer 1 mode[1] frequency"), (*Freqs)[1], 611.0);
        }
    }

    TestEqual(TEXT("master fx count"), First.Master.Fx.Num(), 2);
    TestTrue(TEXT("master normalize mode is lufs"),
        First.Master.Normalize.Mode == EPwSynthNormalizeMode::Lufs);
    TestEqual(TEXT("master normalize target"), First.Master.Normalize.Target, -16.5);
    TestEqual(TEXT("master fadeInMs"), First.Master.FadeInMs, 2.0);
    TestEqual(TEXT("master fadeOutMs"), First.Master.FadeOutMs, 15.0);
    TestEqual(TEXT("target count"), First.Targets.Num(), 3);

    // The hard requirement: serialize -> parse -> serialize is byte-stable.
    const FString FirstJson = PwSynthTestToJson(SerializeSynthRecipe(First));
    TestFalse(TEXT("serialize produced output"), FirstJson.IsEmpty());

    TSharedPtr<FJsonObject> Reparsed = PwSynthTestParseJson(FirstJson);
    TestTrue(TEXT("serialized output is valid JSON"), Reparsed.IsValid());
    if (!Reparsed.IsValid())
    {
        return false;
    }

    FPwSynthRecipe Second;
    FPwSynthRecipeError SecondError;
    const bool bSecond = ParseSynthRecipe(Reparsed, Second, SecondError);
    TestTrue(FString::Printf(TEXT("serialized output re-parses (%s)"), *SecondError.ToString()), bSecond);
    if (!bSecond)
    {
        return false;
    }

    const FString SecondJson = PwSynthTestToJson(SerializeSynthRecipe(Second));
    TestEqual(TEXT("Serialize(Parse(Serialize(Parse(x)))) is byte-identical"), SecondJson, FirstJson);

    // A spot-check that the second pass kept the authored values, not just its
    // own shape: a serializer that dropped a field would still be self-consistent.
    TestEqual(TEXT("re-parsed durationMs"), Second.DurationMs, 900.0);
    TestEqual(TEXT("re-parsed layer count"), Second.Layers.Num(), 2);
    TestEqual(TEXT("re-parsed target count"), Second.Targets.Num(), 3);
    if (Second.Layers.Num() == 2)
    {
        TestEqual(TEXT("re-parsed layer 0 frequencyHz"),
            Second.Layers[0].Generator.Params.GetNumber(FName(TEXT("frequencyHz"))), 880.0);
        TestTrue(TEXT("re-parsed layer 0 modulation routing"),
            Second.Layers[0].Modulation.Routing == EPwSynthModulationRouting::Fm);
        TestEqual(TEXT("re-parsed layer 1 exciter"),
            Second.Layers[1].Generator.Params.GetString(FName(TEXT("exciter"))), FString(TEXT("strike")));
    }

    return true;
}

// =========================================================================
// B. An unrecognised generator kind ERRORS. It must never degrade into a
//    default oscillator - that is the single failure that would make a caller
//    chase a sound it never asked for.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSynthRecipeUnknownGeneratorTest,
    "PinWright.audio.synth.recipe.UnknownGeneratorKind",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSynthRecipeUnknownGeneratorTest::RunTest(const FString& Parameters)
{
    PwSynthTestExpectFailure(*this, TEXT("unknown generator kind"),
        TEXT(R"({"durationMs":100,"layers":[{"generator":{"kind":"supersaw","params":{"frequencyHz":440}}}]})"),
        ErrorCodes::ERR_UNKNOWN_GENERATOR, TEXT("layers[0].generator.kind"));

    // A kind that is merely absent is a shape problem, not an unknown kind, and
    // still must not fall back to a default generator.
    PwSynthTestExpectFailure(*this, TEXT("missing generator kind"),
        TEXT(R"({"durationMs":100,"layers":[{"generator":{"params":{}}}]})"),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("layers[0].generator.kind"));

    // No generator block at all.
    PwSynthTestExpectFailure(*this, TEXT("missing generator"),
        TEXT(R"({"durationMs":100,"layers":[{"gainDb":-3}]})"),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("layers[0].generator"));

    // The rejection message must list the real vocabulary so the caller can fix
    // it in one turn rather than guessing.
    TSharedPtr<FJsonObject> Object = PwSynthTestParseJson(
        TEXT(R"({"durationMs":100,"layers":[{"generator":{"kind":"supersaw"}}]})"));
    FPwSynthRecipe Out;
    FPwSynthRecipeError Error;
    TestFalse(TEXT("supersaw rejected"), ParseSynthRecipe(Object, Out, Error));
    for (uint8 Index = 1; Index < static_cast<uint8>(EPwSynthGeneratorKind::Count); ++Index)
    {
        const FString Name = PwSynthGeneratorKindToString(static_cast<EPwSynthGeneratorKind>(Index));
        TestTrue(FString::Printf(TEXT("rejection names the '%s' generator"), *Name),
            Error.Message.Contains(Name));
    }

    return true;
}

// =========================================================================
// C. An unrecognised effect kind ERRORS, and the field path names the exact
//    chain slot so the caller patches one path instead of re-sending the recipe.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSynthRecipeUnknownEffectTest,
    "PinWright.audio.synth.recipe.UnknownEffectKind",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSynthRecipeUnknownEffectTest::RunTest(const FString& Parameters)
{
    PwSynthTestExpectFailure(*this, TEXT("unknown layer effect"),
        FString::Printf(TEXT("{\"durationMs\":100,\"layers\":[{%s,\"fx\":[{\"kind\":\"bitrot\"}]}]}"),
            TEXT("\"generator\":{\"kind\":\"noise\",\"params\":{\"color\":\"pink\"}}")),
        ErrorCodes::ERR_UNKNOWN_EFFECT, TEXT("layers[0].fx[0].kind"));

    PwSynthTestExpectFailure(*this, TEXT("unknown master effect"),
        FString::Printf(TEXT("{\"durationMs\":100,\"layers\":[%s],\"master\":{\"fx\":[{\"kind\":\"bitrot\"}]}}"),
            *PwSynthTestMinimalLayer()),
        ErrorCodes::ERR_UNKNOWN_EFFECT, TEXT("master.fx[0].kind"));

    // A stereo-bus effect inside a mono layer would be a silent no-op, so it is
    // refused - and the refusal names the remedy (rpc-design §7).
    TSharedPtr<FJsonObject> Object = PwSynthTestParseJson(FString::Printf(
        TEXT("{\"durationMs\":100,\"layers\":[{%s,\"fx\":[{\"kind\":\"width\",\"params\":{\"width\":1.5}}]}]}"),
        TEXT("\"generator\":{\"kind\":\"noise\",\"params\":{\"color\":\"pink\"}}")));
    FPwSynthRecipe Out = PwSynthTestSentinel();
    FPwSynthRecipeError Error;
    TestFalse(TEXT("master-only effect in a layer is refused"), ParseSynthRecipe(Object, Out, Error));
    TestEqual(TEXT("master-only refusal code"), Error.Code, FString(ErrorCodes::ERR_INVALID_RECIPE));
    TestEqual(TEXT("master-only refusal field"), Error.Field, FString(TEXT("layers[0].fx[0].kind")));
    TestTrue(TEXT("master-only refusal names master.fx as the remedy"),
        Error.Message.Contains(TEXT("master.fx")));
    PwSynthTestExpectUntouched(*this, TEXT("master-only effect in a layer"), Out);

    return true;
}

// =========================================================================
// D. A required parameter with no safe default ERRORS rather than defaulting.
//    Explicitly: an osc with no frequency must not become 440 Hz, a filter with
//    no cutoff must not become a wide-open filter, a modal with no modes must
//    not become silence.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSynthRecipeMissingRequiredParamTest,
    "PinWright.audio.synth.recipe.MissingRequiredParam",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSynthRecipeMissingRequiredParamTest::RunTest(const FString& Parameters)
{
    PwSynthTestExpectFailure(*this, TEXT("osc without frequencyHz"),
        TEXT(R"({"durationMs":100,"layers":[{"generator":{"kind":"osc","params":{"waveform":"sine"}}}]})"),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("layers[0].generator.params.frequencyHz"));

    PwSynthTestExpectFailure(*this, TEXT("osc with no params object at all"),
        TEXT(R"({"durationMs":100,"layers":[{"generator":{"kind":"osc"}}]})"),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("layers[0].generator.params.waveform"));

    PwSynthTestExpectFailure(*this, TEXT("filter without cutoffHz"),
        FString::Printf(TEXT("{\"durationMs\":100,\"layers\":[{%s,\"fx\":[{\"kind\":\"filter\",\"params\":{\"type\":\"lowpass\"}}]}]}"),
            TEXT("\"generator\":{\"kind\":\"noise\",\"params\":{\"color\":\"pink\"}}")),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("layers[0].fx[0].params.cutoffHz"));

    PwSynthTestExpectFailure(*this, TEXT("modal without modes"),
        TEXT(R"({"durationMs":100,"layers":[{"generator":{"kind":"modal","params":{"exciter":"strike"}}}]})"),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("layers[0].generator.params.modeFreqsHz"));

    PwSynthTestExpectFailure(*this, TEXT("delay without its required mix"),
        FString::Printf(TEXT("{\"durationMs\":100,\"layers\":[%s],\"master\":{\"fx\":[{\"kind\":\"delay\",\"params\":{\"timeMs\":120}}]}}"),
            *PwSynthTestMinimalLayer()),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("master.fx[0].params.mix"));

    PwSynthTestExpectFailure(*this, TEXT("recipe without durationMs"),
        TEXT(R"({"layers":[{"generator":{"kind":"osc","params":{"waveform":"sine","frequencyHz":440}}}]})"),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("durationMs"));

    PwSynthTestExpectFailure(*this, TEXT("normalize without a target"),
        FString::Printf(TEXT("{\"durationMs\":100,\"layers\":[%s],\"master\":{\"normalize\":{\"mode\":\"peak\"}}}"),
            *PwSynthTestMinimalLayer()),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("master.normalize.target"));

    // The counterfactual for the whole test: nothing above may have produced a
    // usable recipe. Parse a valid osc recipe and confirm the parser does supply
    // documented defaults - so the failures above are about required-ness, not
    // about the parser being unable to default at all.
    TSharedPtr<FJsonObject> Object = PwSynthTestParseJson(PwSynthTestMinimalRecipe);
    FPwSynthRecipe Out;
    FPwSynthRecipeError Error;
    const bool bMinimalParsed = ParseSynthRecipe(Object, Out, Error);
    TestTrue(FString::Printf(TEXT("minimal recipe parses (%s)"), *Error.ToString()), bMinimalParsed);
    if (Out.Layers.Num() == 1)
    {
        TestEqual(TEXT("pulseWidth default applied when the parameter is optional"),
            Out.Layers[0].Generator.Params.GetNumber(FName(TEXT("pulseWidth"))), 0.5);
        TestEqual(TEXT("unison default applied"),
            Out.Layers[0].Generator.Params.GetInt(FName(TEXT("unison"))), 1);
    }
    TestEqual(TEXT("seed default applied"), Out.Seed, PwSynthLimits::DefaultSeed);
    TestEqual(TEXT("sampleRate default applied"), Out.SampleRate, PwSynthLimits::DefaultSampleRate);

    return true;
}

// =========================================================================
// E. Caps are validation errors, not clamps. A clamped recipe would not
//    round-trip: the caller would patch a layer that silently no longer exists.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSynthRecipeCapsAreErrorsTest,
    "PinWright.audio.synth.recipe.CapsAreErrorsNotClamps",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSynthRecipeCapsAreErrorsTest::RunTest(const FString& Parameters)
{
    // Exactly at the cap: accepted, which is what makes the over-cap rejection
    // meaningful rather than an off-by-one.
    {
        TSharedPtr<FJsonObject> Object = PwSynthTestParseJson(
            PwSynthTestRepeatedLayers(PwSynthLimits::MaxLayers));
        FPwSynthRecipe Out;
        FPwSynthRecipeError Error;
        const bool bAtCapParsed = ParseSynthRecipe(Object, Out, Error);
        TestTrue(FString::Printf(TEXT("%d layers is accepted (%s)"),
            PwSynthLimits::MaxLayers, *Error.ToString()), bAtCapParsed);
        TestEqual(TEXT("all layers survived"), Out.Layers.Num(), PwSynthLimits::MaxLayers);
    }

    PwSynthTestExpectFailure(*this, TEXT("layer cap +1"),
        PwSynthTestRepeatedLayers(PwSynthLimits::MaxLayers + 1),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("layers"));

    PwSynthTestExpectFailure(*this, TEXT("layer fx cap +1"),
        FString::Printf(TEXT("{\"durationMs\":100,\"layers\":[{%s,\"fx\":[%s]}]}"),
            TEXT("\"generator\":{\"kind\":\"noise\",\"params\":{\"color\":\"pink\"}}"),
            *PwSynthTestRepeatedFx(PwSynthLimits::MaxLayerFx + 1)),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("layers[0].fx"));

    PwSynthTestExpectFailure(*this, TEXT("master fx cap +1"),
        FString::Printf(TEXT("{\"durationMs\":100,\"layers\":[%s],\"master\":{\"fx\":[%s]}}"),
            *PwSynthTestMinimalLayer(), *PwSynthTestRepeatedFx(PwSynthLimits::MaxMasterFx + 1)),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("master.fx"));

    PwSynthTestExpectFailure(*this, TEXT("durationMs above the ceiling"),
        FString::Printf(TEXT("{\"durationMs\":%g,\"layers\":[%s]}"),
            PwSynthLimits::MaxDurationMs + 1.0, *PwSynthTestMinimalLayer()),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("durationMs"));

    PwSynthTestExpectFailure(*this, TEXT("zero layers"),
        TEXT(R"({"durationMs":100,"layers":[]})"),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("layers"));

    PwSynthTestExpectFailure(*this, TEXT("sampleRate below the floor"),
        FString::Printf(TEXT("{\"durationMs\":100,\"sampleRate\":100,\"layers\":[%s]}"),
            *PwSynthTestMinimalLayer()),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("sampleRate"));

    // A per-parameter range is a rejection too, not a clamp to the nearest bound.
    PwSynthTestExpectFailure(*this, TEXT("pan beyond hard right"),
        FString::Printf(TEXT("{\"durationMs\":100,\"layers\":[{\"pan\":4,%s}]}"),
            TEXT("\"generator\":{\"kind\":\"noise\",\"params\":{\"color\":\"pink\"}}")),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("layers[0].pan"));

    return true;
}

// =========================================================================
// F. The error names the exact field path the caller must patch. A recipe with
//    three layers whose THIRD layer's SECOND effect is bad must report
//    layers[2].fx[1].kind, not "an effect is invalid".
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSynthRecipeFieldPathTest,
    "PinWright.audio.synth.recipe.ErrorNamesFieldPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSynthRecipeFieldPathTest::RunTest(const FString& Parameters)
{
    const FString GoodLayer = PwSynthTestMinimalLayer();
    const FString BadLayer = TEXT(R"({"generator":{"kind":"osc","params":{"waveform":"sine","frequencyHz":440}},)")
        TEXT(R"("fx":[{"kind":"gain","params":{"gainDb":0}},{"kind":"bitrot"}]})");

    PwSynthTestExpectFailure(*this, TEXT("deep effect path"),
        FString::Printf(TEXT("{\"durationMs\":100,\"layers\":[%s,%s,%s]}"),
            *GoodLayer, *GoodLayer, *BadLayer),
        ErrorCodes::ERR_UNKNOWN_EFFECT, TEXT("layers[2].fx[1].kind"));

    PwSynthTestExpectFailure(*this, TEXT("deep envelope point path"),
        FString::Printf(TEXT("{\"durationMs\":100,\"layers\":[%s,{%s,\"ampEnvelope\":[{\"timeMs\":0,\"value\":1},{\"timeMs\":10,\"value\":0,\"curve\":\"wobble\"}]}]}"),
            *GoodLayer, TEXT("\"generator\":{\"kind\":\"noise\",\"params\":{\"color\":\"pink\"}}")),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("layers[1].ampEnvelope[1].curve"));

    PwSynthTestExpectFailure(*this, TEXT("deep mode array element path"),
        TEXT(R"({"durationMs":100,"layers":[{"generator":{"kind":"modal","params":{"modeFreqsHz":[220,"x"],"modeDecaysMs":[10,10],"modeGainsDb":[0,0]}}}]})"),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("layers[0].generator.params.modeFreqsHz[1]"));

    PwSynthTestExpectFailure(*this, TEXT("target metric path"),
        FString::Printf(TEXT("{\"durationMs\":100,\"layers\":[%s],\"targets\":{\"loudness\":{\"max\":-1}}}"), *GoodLayer),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("targets.loudness"));

    return true;
}

// =========================================================================
// G. Closed vocabularies and unknown keys. A typo must surface as an error at a
//    named path, never as a dropped setting the caller then cannot find.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSynthRecipeStrictVocabularyTest,
    "PinWright.audio.synth.recipe.StrictVocabulary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSynthRecipeStrictVocabularyTest::RunTest(const FString& Parameters)
{
    const FString GoodLayer = PwSynthTestMinimalLayer();

    PwSynthTestExpectFailure(*this, TEXT("unknown normalize mode"),
        FString::Printf(TEXT("{\"durationMs\":100,\"layers\":[%s],\"master\":{\"normalize\":{\"mode\":\"rms\",\"target\":-14}}}"), *GoodLayer),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("master.normalize.mode"));

    PwSynthTestExpectFailure(*this, TEXT("unknown filter type"),
        FString::Printf(TEXT("{\"durationMs\":100,\"layers\":[{%s,\"fx\":[{\"kind\":\"filter\",\"params\":{\"type\":\"moog\",\"cutoffHz\":800}}]}]}"),
            TEXT("\"generator\":{\"kind\":\"noise\",\"params\":{\"color\":\"pink\"}}")),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("layers[0].fx[0].params.type"));

    PwSynthTestExpectFailure(*this, TEXT("unknown modulator source"),
        FString::Printf(TEXT("{\"durationMs\":100,\"layers\":[{%s,\"modulation\":{\"am\":{\"depth\":0.5,\"rateHz\":6,\"source\":\"supersaw\"}}}]}"),
            TEXT("\"generator\":{\"kind\":\"noise\",\"params\":{\"color\":\"pink\"}}")),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("layers[0].modulation.am.source"));

    // The typo case: a misspelled layer key would otherwise be silently dropped
    // and the caller would keep patching a field nothing reads.
    PwSynthTestExpectFailure(*this, TEXT("misspelled layer key"),
        FString::Printf(TEXT("{\"durationMs\":100,\"layers\":[{\"panning\":0.5,%s}]}"),
            TEXT("\"generator\":{\"kind\":\"noise\",\"params\":{\"color\":\"pink\"}}")),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("layers[0].panning"));

    PwSynthTestExpectFailure(*this, TEXT("misspelled generator parameter"),
        TEXT(R"({"durationMs":100,"layers":[{"generator":{"kind":"osc","params":{"waveform":"sine","frequencyHz":440,"freq":220}}}]})"),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("layers[0].generator.params.freq"));

    PwSynthTestExpectFailure(*this, TEXT("misspelled top-level key"),
        FString::Printf(TEXT("{\"durationMs\":100,\"sampleRateHz\":48000,\"layers\":[%s]}"), *GoodLayer),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("sampleRateHz"));

    // Type strictness: a numeric string is not a number, because coercing it
    // would change the type across a patch round-trip.
    PwSynthTestExpectFailure(*this, TEXT("numeric string where a number is required"),
        TEXT(R"({"durationMs":100,"layers":[{"generator":{"kind":"osc","params":{"waveform":"sine","frequencyHz":"880"}}}]})"),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("layers[0].generator.params.frequencyHz"));

    // Structural contradictions that a lenient parser would accept and render wrong.
    PwSynthTestExpectFailure(*this, TEXT("empty modulation block"),
        FString::Printf(TEXT("{\"durationMs\":100,\"layers\":[{%s,\"modulation\":{}}]}"),
            TEXT("\"generator\":{\"kind\":\"noise\",\"params\":{\"color\":\"pink\"}}")),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("layers[0].modulation"));

    PwSynthTestExpectFailure(*this, TEXT("out-of-order envelope points"),
        FString::Printf(TEXT("{\"durationMs\":100,\"layers\":[{%s,\"ampEnvelope\":[{\"timeMs\":50,\"value\":1},{\"timeMs\":10,\"value\":0}]}]}"),
            TEXT("\"generator\":{\"kind\":\"noise\",\"params\":{\"color\":\"pink\"}}")),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("layers[0].ampEnvelope[1].timeMs"));

    PwSynthTestExpectFailure(*this, TEXT("mismatched modal mode array lengths"),
        TEXT(R"({"durationMs":100,"layers":[{"generator":{"kind":"modal","params":{"modeFreqsHz":[220,440],"modeDecaysMs":[10],"modeGainsDb":[0,0]}}}]})"),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("layers[0].generator.params.modeDecaysMs"));

    PwSynthTestExpectFailure(*this, TEXT("target range with neither bound"),
        FString::Printf(TEXT("{\"durationMs\":100,\"layers\":[%s],\"targets\":{\"peakDb\":{}}}"), *GoodLayer),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("targets.peakDb"));

    PwSynthTestExpectFailure(*this, TEXT("unsupported recipe version"),
        FString::Printf(TEXT("{\"version\":99,\"durationMs\":100,\"layers\":[%s]}"), *GoodLayer),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("version"));

    // FJsonObject field lookup is case-insensitive. The closed-key adapter must
    // preserve that established parser behavior while still rejecting typos.
    {
        TSharedPtr<FJsonObject> Object = PwSynthTestParseJson(
            FString(PwSynthTestMinimalRecipe).Replace(TEXT("\"durationMs\""), TEXT("\"DurationMs\"")));
        FPwSynthRecipe Out;
        FPwSynthRecipeError Error;
        TestTrue(FString::Printf(TEXT("mixed-case known key parses (%s)"), *Error.ToString()),
            ParseSynthRecipe(Object, Out, Error));
    }

    // Null input is a rejection, not an empty recipe.
    {
        FPwSynthRecipe Out = PwSynthTestSentinel();
        FPwSynthRecipeError Error;
        TestFalse(TEXT("null recipe object is rejected"),
            ParseSynthRecipe(TSharedPtr<FJsonObject>(), Out, Error));
        TestEqual(TEXT("null recipe error code"), Error.Code, FString(ErrorCodes::ERR_INVALID_RECIPE));
        PwSynthTestExpectUntouched(*this, TEXT("null recipe"), Out);
    }

    // The FString contract overload must carry the same field path, since that is
    // the only error surface some callers see.
    {
        TSharedPtr<FJsonObject> Object = PwSynthTestParseJson(
            TEXT(R"({"durationMs":100,"layers":[{"generator":{"kind":"supersaw"}}]})"));
        FPwSynthRecipe Out;
        FString Message;
        TestFalse(TEXT("string overload reports failure"), ParseSynthRecipe(Object, Out, Message));
        TestTrue(TEXT("string overload names the field path"),
            Message.Contains(TEXT("layers[0].generator.kind")));
    }

    return true;
}

// =========================================================================
// H. audio.synth.describe_schema: every inline section must stay under the
//    response spill threshold, the kind lists must be complete, the worked
//    example must actually validate, and an unrecognised section or kind must
//    error rather than return an empty slice.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSynthDescribeSchemaTest,
    "PinWright.audio.synth.describe_schema.Sections",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSynthDescribeSchemaTest::RunTest(const FString& Parameters)
{
    const TCHAR* const Method = TEXT("audio.synth.describe_schema");
    const int32 Threshold = HttpResponseSpill::GetDefaultThresholdCharacters();

    auto DescribePage = [&](const FString& Section, const FString& Kind, int32 Page,
        FTestResponseCapture& Capture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        if (!Section.IsEmpty())
        {
            Payload->SetStringField(TEXT("section"), Section);
        }
        if (!Kind.IsEmpty())
        {
            Payload->SetStringField(TEXT("kind"), Kind);
        }
        if (Page > 0)
        {
            Payload->SetNumberField(TEXT("page"), Page);
        }
        return InvokeHandlerWithCapture(Method, Payload, Capture);
    };

    auto Describe = [&](const FString& Section, const FString& Kind, FTestResponseCapture& Capture)
    {
        return DescribePage(Section, Kind, 0, Capture);
    };

    // The default call: no arguments at all.
    {
        FTestResponseCapture Capture;
        TestTrue(TEXT("handler is registered"), Describe(FString(), FString(), Capture));
        TestTrue(FString::Printf(TEXT("default call succeeds (errorCode='%s')"), *Capture.ErrorCode),
            Capture.bSuccess);
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            return false;
        }

        FString SectionName;
        Capture.Result->TryGetStringField(TEXT("section"), SectionName);
        TestEqual(TEXT("default section is overview"), SectionName, FString(TEXT("overview")));

        for (const TCHAR* Field : { TEXT("topology"), TEXT("caps"), TEXT("defaults"),
                                    TEXT("required"), TEXT("strictness"), TEXT("errorCodes"),
                                    TEXT("generatorKinds"), TEXT("effectKinds"), TEXT("example"), TEXT("next") })
        {
            TestTrue(FString::Printf(TEXT("overview carries '%s'"), Field),
                Capture.Result->HasField(Field));
        }

        const TArray<TSharedPtr<FJsonValue>>* Generators = nullptr;
        if (Capture.Result->TryGetArrayField(TEXT("generatorKinds"), Generators))
        {
            TestEqual(TEXT("overview lists all six generator kinds"), Generators->Num(),
                static_cast<int32>(EPwSynthGeneratorKind::Count) - 1);
        }
        const TArray<TSharedPtr<FJsonValue>>* Effects = nullptr;
        if (Capture.Result->TryGetArrayField(TEXT("effectKinds"), Effects))
        {
            TestEqual(TEXT("overview lists all fifteen effect kinds"), Effects->Num(),
                static_cast<int32>(EPwSynthFxKind::Count) - 1);
        }

        // The published example must survive the very parser this schema
        // documents. The handler measures it rather than asserting it, so a
        // literal `true` here would be a lie the test would not catch.
        bool bExampleValidated = false;
        TestTrue(TEXT("overview reports whether the worked example validates"),
            Capture.Result->TryGetBoolField(TEXT("exampleValidated"), bExampleValidated));
        FString ExampleError;
        Capture.Result->TryGetStringField(TEXT("exampleError"), ExampleError);
        TestTrue(FString::Printf(TEXT("the worked example parses (%s)"), *ExampleError), bExampleValidated);

        TestTrue(FString::Printf(TEXT("overview stays inline: %d chars vs threshold %d"),
            PwSynthTestToJson(Capture.Result).Len(), Threshold),
            PwSynthTestToJson(Capture.Result).Len() < Threshold);
    }

    // Every drill-down section must stay inline - that is the whole point of
    // sectioning and paging instead of letting the response spill. For the paged
    // listings, walk every page: each page must be inline AND the pages together
    // must cover every kind, so a paging bug that drops a kind cannot pass by
    // making the response conveniently small.
    auto PageThroughSection = [&](const TCHAR* Section, const TCHAR* ArrayField, int32 ExpectedTotal)
    {
        TSet<FString> SeenKinds;
        int32 Page = 1;
        for (int32 Guard = 0; Guard < 32; ++Guard)
        {
            FTestResponseCapture Capture;
            DescribePage(Section, FString(), Page, Capture);
            TestTrue(FString::Printf(TEXT("section '%s' page %d succeeds (errorCode='%s')"),
                Section, Page, *Capture.ErrorCode), Capture.bSuccess);
            if (!Capture.bSuccess || !Capture.Result.IsValid())
            {
                return;
            }

            const int32 Length = PwSynthTestToJson(Capture.Result).Len();
            TestTrue(FString::Printf(TEXT("section '%s' page %d stays inline: %d chars vs threshold %d"),
                Section, Page, Length, Threshold), Length < Threshold);

            const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
            TestTrue(FString::Printf(TEXT("section '%s' page %d carries '%s'"), Section, Page, ArrayField),
                Capture.Result->TryGetArrayField(ArrayField, Entries));
            if (!Entries)
            {
                return;
            }
            TestTrue(FString::Printf(TEXT("section '%s' page %d is not empty"), Section, Page),
                Entries->Num() > 0);
            for (const TSharedPtr<FJsonValue>& Entry : *Entries)
            {
                const TSharedPtr<FJsonObject>* Obj = nullptr;
                FString KindName;
                if (Entry.IsValid() && Entry->TryGetObject(Obj) && Obj &&
                    (*Obj)->TryGetStringField(TEXT("kind"), KindName))
                {
                    SeenKinds.Add(KindName);
                }
            }

            double NextPage = 0.0;
            if (!Capture.Result->TryGetNumberField(TEXT("nextPage"), NextPage))
            {
                break;
            }
            Page = static_cast<int32>(NextPage);
        }

        TestEqual(FString::Printf(TEXT("section '%s' pages cover every kind"), Section),
            SeenKinds.Num(), ExpectedTotal);
    };

    PageThroughSection(TEXT("generators"), TEXT("generators"),
        static_cast<int32>(EPwSynthGeneratorKind::Count) - 1);
    PageThroughSection(TEXT("effects"), TEXT("effects"),
        static_cast<int32>(EPwSynthFxKind::Count) - 1);

    for (const TCHAR* Section : { TEXT("envelopes"), TEXT("targets") })
    {
        FTestResponseCapture Capture;
        Describe(Section, FString(), Capture);
        TestTrue(FString::Printf(TEXT("section '%s' succeeds (errorCode='%s')"),
            Section, *Capture.ErrorCode), Capture.bSuccess);
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            continue;
        }
        const int32 Length = PwSynthTestToJson(Capture.Result).Len();
        TestTrue(FString::Printf(TEXT("section '%s' stays inline: %d chars vs threshold %d"),
            Section, Length, Threshold), Length < Threshold);
    }

    // A page past the end errors instead of returning an empty listing, which
    // would read identically to "this section has no more kinds".
    {
        FTestResponseCapture Capture;
        DescribePage(TEXT("effects"), FString(), 99, Capture);
        TestFalse(TEXT("out-of-range page is rejected"), Capture.bSuccess);
        TestEqual(TEXT("out-of-range page error code"), Capture.ErrorCode,
            FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestTrue(TEXT("out-of-range page names the page count"),
            Capture.Message.Contains(TEXT("page(s)")));
    }

    // Envelopes and targets carry the forms the recipe grammar depends on.
    {
        FTestResponseCapture Capture;
        Describe(TEXT("envelopes"), FString(), Capture);
        // Assert the call succeeded rather than only guarding on it. This section is a pure
        // vocabulary listing with no environment dependency, so a failure here is a defect —
        // and without this line a verb that stopped answering would skip every field assertion
        // below and leave the test green.
        TestTrue(FString::Printf(TEXT("envelopes section succeeds (errorCode='%s')"),
            *Capture.ErrorCode), Capture.bSuccess && Capture.Result.IsValid());
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            for (const TCHAR* Field : { TEXT("ampEnvelope"), TEXT("curves"), TEXT("pitchEnvelope"),
                                        TEXT("modulation"), TEXT("normalize") })
            {
                TestTrue(FString::Printf(TEXT("envelopes section carries '%s'"), Field),
                    Capture.Result->HasField(Field));
            }
        }
    }
    {
        FTestResponseCapture Capture;
        Describe(TEXT("targets"), FString(), Capture);
        const TSharedPtr<FJsonObject>* Targets = nullptr;
        // Same reason as the envelopes block above, plus one more: the `targets` object itself is
        // part of the contract, so its absence must fail here rather than silently skipping the
        // metric-count assertion that is the whole point of this block.
        TestTrue(FString::Printf(TEXT("targets section succeeds and carries a targets object (errorCode='%s')"),
            *Capture.ErrorCode),
            Capture.bSuccess && Capture.Result.IsValid() &&
                Capture.Result->TryGetObjectField(TEXT("targets"), Targets) && Targets);
        if (Capture.bSuccess && Capture.Result.IsValid() &&
            Capture.Result->TryGetObjectField(TEXT("targets"), Targets) && Targets)
        {
            const TArray<TSharedPtr<FJsonValue>>* Metrics = nullptr;
            TestTrue(TEXT("targets section lists its metrics"),
                (*Targets)->TryGetArrayField(TEXT("metrics"), Metrics));
            if (Metrics)
            {
                TestEqual(TEXT("targets section lists every metric"), Metrics->Num(),
                    static_cast<int32>(EPwSynthMetric::Count));
            }
        }
    }

    // A kind filter narrows the section rather than returning everything.
    {
        FTestResponseCapture Capture;
        Describe(TEXT("generators"), TEXT("granular"), Capture);
        TestTrue(TEXT("kind-filtered generators call succeeds"), Capture.bSuccess);
        const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
        if (Capture.bSuccess && Capture.Result.IsValid() &&
            Capture.Result->TryGetArrayField(TEXT("generators"), Entries) && Entries)
        {
            TestEqual(TEXT("kind filter returns exactly one generator"), Entries->Num(), 1);
        }
    }

    // Failure direction: an unrecognised section or kind errors. An empty slice
    // would read identically to "this kind has no parameters".
    {
        FTestResponseCapture Capture;
        Describe(TEXT("everything"), FString(), Capture);
        TestFalse(TEXT("unknown section is rejected"), Capture.bSuccess);
        TestEqual(TEXT("unknown section error code"), Capture.ErrorCode,
            FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestTrue(TEXT("unknown section names the valid sections"),
            Capture.Message.Contains(TEXT("overview")) && Capture.Message.Contains(TEXT("effects")));
    }
    {
        FTestResponseCapture Capture;
        Describe(TEXT("generators"), TEXT("supersaw"), Capture);
        TestFalse(TEXT("unknown generator kind filter is rejected"), Capture.bSuccess);
        TestEqual(TEXT("unknown generator kind error code"), Capture.ErrorCode,
            FString(ErrorCodes::ERR_UNKNOWN_GENERATOR));
    }
    {
        FTestResponseCapture Capture;
        Describe(TEXT("effects"), TEXT("bitrot"), Capture);
        TestFalse(TEXT("unknown effect kind filter is rejected"), Capture.bSuccess);
        TestEqual(TEXT("unknown effect kind error code"), Capture.ErrorCode,
            FString(ErrorCodes::ERR_UNKNOWN_EFFECT));
    }

    // Every parameter the schema publishes must be documented, or the cold-start
    // path teaches an incomplete grammar.
    for (uint8 Index = 1; Index < static_cast<uint8>(EPwSynthGeneratorKind::Count); ++Index)
    {
        const FPwSynthKindSpec& Kind = PwSynthGeneratorSpec(static_cast<EPwSynthGeneratorKind>(Index));
        TestNotNull(TEXT("generator kind has a params table"), Kind.Params);
        if (!Kind.Params)
        {
            continue;
        }
        for (const FPwSynthParamSpec& Spec : *Kind.Params)
        {
            TestFalse(FString::Printf(TEXT("%s.%s has a meaning"), Kind.Name, Spec.DisplayName),
                Spec.Meaning == nullptr || FString(Spec.Meaning).IsEmpty());
        }
    }
    for (uint8 Index = 1; Index < static_cast<uint8>(EPwSynthFxKind::Count); ++Index)
    {
        const FPwSynthKindSpec& Kind = PwSynthFxSpec(static_cast<EPwSynthFxKind>(Index));
        TestNotNull(TEXT("effect kind has a params table"), Kind.Params);
        if (!Kind.Params)
        {
            continue;
        }
        for (const FPwSynthParamSpec& Spec : *Kind.Params)
        {
            TestFalse(FString::Printf(TEXT("%s.%s has a meaning"), Kind.Name, Spec.DisplayName),
                Spec.Meaning == nullptr || FString(Spec.Meaning).IsEmpty());
        }
    }

    return true;
}
