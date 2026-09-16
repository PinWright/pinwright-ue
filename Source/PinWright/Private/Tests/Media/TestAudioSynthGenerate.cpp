// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for audio.synth.generate / patch / variations / export.
//
// The bias here is rpc-design.md §12: a test that asserts a verb succeeded proves almost
// nothing about this family, because every defect it can ship looks like a success. So the
// assertions that carry weight are the ones that break if the verb starts answering "yes":
//
//   - a malformed recipe must come back with the PARSER's code and its field path, not a
//     generic INVALID_PARAMS with prose, because the field path is what the caller edits;
//   - an unknown candidateId must come back with the registry's DISTINCT code, because
//     "re-render, mind the budget" (CANDIDATE_EVICTED), "generate something first"
//     (NO_CANDIDATES) and "your id is wrong" (CANDIDATE_NOT_FOUND) are three different
//     next moves;
//   - patch must leave the source candidate byte-identical, asserted by re-reading its buffer
//     out of the registry after the patch - the whole point of a patch is that the agent can
//     compare the two takes;
//   - a patch whose result does not parse must create NO candidate, asserted by counting the
//     registry before and after rather than by trusting the response;
//   - variations with count 0 must ERROR, because an empty success is indistinguishable from
//     a sweep whose every render failed;
//   - export to a path outside /Game must leave nothing behind, asserted with
//     StaticFindObject and FPackageName::DoesPackageExist rather than with the response;
//   - target response rows, so a parsed target cannot silently disappear from the result.
//
// Transport response-size behavior is covered by the shared HttpResponseSpill tests, which
// measure one condensed reader-facing copy against the configured threshold.

#include "Misc/AutomationTest.h"

#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwAudioDecode.h"
#include "AudioGen/PwCandidateRegistry.h"
#include "AudioGen/PwSynthRecipe.h"
#include "Compat/EngineVersionCompat.h"
#include "Dispatch/SafePoint.h"
#include "Handlers/ErrorCodes.h"
#include "State/JobRegistry.h"
#include "State/PluginState.h"
#include "Tests/TestAssetTeardown.h"
#include "Tests/TestUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundWave.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"

// Named (not anonymous) namespace: the module builds with bUseUnity = true and the sibling
// audio test TUs already own several anonymous helpers of similar name.
namespace PwSynthGenerateTestHelpers
{
    const TCHAR* const GenerateMethod   = TEXT("audio.synth.generate");
    const TCHAR* const PatchMethod      = TEXT("audio.synth.patch");
    const TCHAR* const VariationsMethod = TEXT("audio.synth.variations");
    const TCHAR* const ExportMethod     = TEXT("audio.synth.export");

    constexpr int32 TestSampleRate = 48000;

    // ---------------------------------------------------------------------------------------
    // Recipe fixtures. Built as JSON text and handed to the handler, so every test exercises
    // the same parse the production path runs.
    // ---------------------------------------------------------------------------------------

    FString OscLayerJson(const TCHAR* Waveform, double FrequencyHz, double Pan, double GainDb = -6.0,
                         bool bWithDecay = false)
    {
        // The final zero point is held through the rest of the layer, so a 600 ms render has
        // whole 10 ms RMS blocks below PwAudioAnalysisLimits::EnvelopeFloorDb after 500 ms.
        const TCHAR* AmpEnvelope = bWithDecay
            ? TEXT(",\"ampEnvelope\":[{\"timeMs\":0.0,\"value\":1.0},"
                   "{\"timeMs\":450.0,\"value\":1.0,\"curve\":\"exp\"},"
                   "{\"timeMs\":500.0,\"value\":0.0}]")
            : TEXT("");
        return FString::Printf(
            TEXT("{\"gainDb\":%f,\"pan\":%f,\"generator\":{\"kind\":\"osc\",")
            TEXT("\"params\":{\"waveform\":\"%s\",\"frequencyHz\":%f}}%s}"),
            GainDb, Pan, Waveform, FrequencyHz, AmpEnvelope);
    }

    FString RecipeJson(int32 Seed, double DurationMs, const TArray<FString>& Layers)
    {
        return FString::Printf(
            TEXT("{\"version\":1,\"seed\":%d,\"sampleRate\":%d,\"durationMs\":%f,")
            TEXT("\"layers\":[%s],\"master\":{\"normalize\":{\"mode\":\"peak\",\"target\":-1.0}}}"),
            Seed, TestSampleRate, DurationMs, *FString::Join(Layers, TEXT(",")));
    }

    TSharedPtr<FJsonObject> ParseJson(const FString& Json)
    {
        TSharedPtr<FJsonObject> Root;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
        FJsonSerializer::Deserialize(Reader, Root);
        return Root;
    }

    /** One short sine layer - the cheapest thing that renders and analyses. */
    TSharedPtr<FJsonObject> SimpleRecipe(int32 Seed = 7, double DurationMs = 120.0)
    {
        return ParseJson(RecipeJson(Seed, DurationMs, {OscLayerJson(TEXT("sine"), 440.0, 0.0)}));
    }

    TSharedPtr<FJsonObject> TargetedRecipe()
    {
        // 5765 frames at 48 kHz measure as 120.104166... ms. The narrow fractional range below
        // catches an integer-coerced or rounded published measurement while remaining inclusive.
        TSharedPtr<FJsonObject> Root = SimpleRecipe(/*Seed=*/17, /*DurationMs=*/120.104167);
        TSharedPtr<FJsonObject> Targets = MakeShared<FJsonObject>();

        TSharedPtr<FJsonObject> Duration = MakeShared<FJsonObject>();
        Duration->SetNumberField(TEXT("min"), 120.10415);
        Duration->SetNumberField(TEXT("max"), 120.10418);
        Targets->SetObjectField(TEXT("durationMs"), Duration);

        TSharedPtr<FJsonObject> Centroid = MakeShared<FJsonObject>();
        Centroid->SetNumberField(TEXT("min"), 10000.0);
        Targets->SetObjectField(TEXT("centroidHz"), Centroid);

        TSharedPtr<FJsonObject> Decay = MakeShared<FJsonObject>();
        Decay->SetNumberField(TEXT("max"), 10.0);
        Targets->SetObjectField(TEXT("decayMs"), Decay);

        Root->SetObjectField(TEXT("targets"), Targets);
        return Root;
    }

    /**
     * The worst case the summary form has to survive: the layer cap, both channels occupied,
     * and long enough that every analysis family measures (LUFS needs ~485 ms at 48 kHz, so a
     * shorter fixture would silently shrink the payload the gate is supposed to bound).
     */
    TSharedPtr<FJsonObject> WorstCaseRecipe()
    {
        TArray<FString> Layers;
        for (int32 Index = 0; Index < PwSynthLimits::MaxLayers; ++Index)
        {
            const double Frequency = 110.0 * FMath::Pow(1.5, static_cast<double>(Index));
            const double Pan = -1.0 + (2.0 * Index) / (PwSynthLimits::MaxLayers - 1);
            Layers.Add(OscLayerJson(Index % 2 == 0 ? TEXT("saw") : TEXT("sine"), Frequency, Pan, -14.0,
                /*bWithDecay=*/true));
        }

        TSharedPtr<FJsonObject> Root = ParseJson(RecipeJson(3, 600.0, Layers));
        TSharedPtr<FJsonObject> Targets = MakeShared<FJsonObject>();
        const auto AddTarget = [&Targets](const TCHAR* Metric, double Min, double Max)
        {
            TSharedPtr<FJsonObject> Range = MakeShared<FJsonObject>();
            Range->SetNumberField(TEXT("min"), Min);
            Range->SetNumberField(TEXT("max"), Max);
            Targets->SetObjectField(Metric, Range);
        };

        // Keep every legal metric in the fixture, with both bounds, so response-shape coverage
        // includes the complete target table as well as the eight per-layer rows. The envelope
        // above makes decayMs finite under the analysis implementation's relative floor.
        AddTarget(TEXT("peakDb"), -100.0, 100.0);
        AddTarget(TEXT("rmsDb"), -100.0, 100.0);
        AddTarget(TEXT("lufs"), -100.0, 100.0);
        AddTarget(TEXT("crestDb"), -100.0, 100.0);
        AddTarget(TEXT("durationMs"), 0.0, 60000.0);
        AddTarget(TEXT("centroidHz"), 0.0, 24000.0);
        AddTarget(TEXT("rolloffHz"), 0.0, 24000.0);
        AddTarget(TEXT("flatness"), 0.0, 1.0);
        AddTarget(TEXT("zeroCrossingRate"), 0.0, 48000.0);
        AddTarget(TEXT("attackMs"), 0.0, 60000.0);
        AddTarget(TEXT("decayMs"), 0.0, 60000.0);
        AddTarget(TEXT("dcOffset"), -1.0, 1.0);
        AddTarget(TEXT("clippedSamples"), 0.0, 60000.0);
        AddTarget(TEXT("stereoCorrelation"), -1.0, 1.0);
        Root->SetObjectField(TEXT("targets"), Targets);
        return Root;
    }

    // ---------------------------------------------------------------------------------------
    // Response plumbing
    // ---------------------------------------------------------------------------------------

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

    double DoubleField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, double Default = 0.0)
    {
        double Value = 0.0;
        return (Object.IsValid() && Object->TryGetNumberField(Key, Value)) ? Value : Default;
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

    /** Runs generate and returns the new candidate id, or empty on any failure. */
    FString Generate(const TSharedPtr<FJsonObject>& Recipe, bool bAnalyze,
                     FTestResponseCapture& Capture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("recipe"), Recipe);
        Payload->SetBoolField(TEXT("analyze"), bAnalyze);
        if (!InvokeHandlerWithCapture(GenerateMethod, Payload, Capture) || !Capture.bSuccess)
        {
            return FString();
        }
        return StringField(Capture.Result, TEXT("candidateId"));
    }

    FPwCandidateRegistry& Registry()
    {
        return FPluginState::Get().GetCandidateRegistry();
    }

    int32 ResidentCount()
    {
        return Registry().Usage().NumCandidates;
    }

    /** Deep copy of a candidate's audio, so a later re-read can be compared against it. */
    bool SnapshotBuffer(const FString& CandidateId, FPwAudioBuffer& Out)
    {
        const FPwCandidateLookupResult Hit = Registry().Get(CandidateId);
        if (!Hit.IsHit())
        {
            return false;
        }
        Out = Hit.Candidate->Buffer;
        return true;
    }

    bool BuffersAreByteIdentical(const FPwAudioBuffer& A, const FPwAudioBuffer& B)
    {
        if (A.SampleRate != B.SampleRate || A.Left.Num() != B.Left.Num() ||
            A.Right.Num() != B.Right.Num())
        {
            return false;
        }
        return (A.Left.Num() == 0 ||
                FMemory::Memcmp(A.Left.GetData(), B.Left.GetData(), A.Left.Num() * sizeof(float)) == 0)
            && (A.Right.Num() == 0 ||
                FMemory::Memcmp(A.Right.GetData(), B.Right.GetData(), A.Right.Num() * sizeof(float)) == 0);
    }

    FString CanonicalRecipeOf(const FString& CandidateId)
    {
        const FPwCandidateLookupResult Hit = Registry().Get(CandidateId);
        if (!Hit.IsHit())
        {
            return FString();
        }
        FString Text;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
        const TSharedPtr<FJsonObject> Json = SerializeSynthRecipe(Hit.Candidate->Recipe);
        if (Json.IsValid())
        {
            FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);
        }
        return Text;
    }

    TSharedPtr<FJsonObject> MakeOp(const TCHAR* Op, const TCHAR* Path)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("op"), Op);
        Entry->SetStringField(TEXT("path"), Path);
        return Entry;
    }

    TSharedPtr<FJsonObject> MakeNumberOp(const TCHAR* Op, const TCHAR* Path, double Value)
    {
        TSharedPtr<FJsonObject> Entry = MakeOp(Op, Path);
        Entry->SetNumberField(TEXT("value"), Value);
        return Entry;
    }

    /** Terminal state of the ticket a job verb returned. */
    bool ReadJobTicket(const FTestResponseCapture& Capture, FJobTicket& Out)
    {
        const FString TicketId = StringField(Capture.Result, TEXT("ticket_id"));
        return !TicketId.IsEmpty() && FPluginState::Get().GetJobRegistry().Get(TicketId, Out);
    }

    FString UniqueAssetName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    const TCHAR* const TestAssetFolder = TEXT("/Game/PinWrightTests");
}

// =================================================================================================
// A. generate - the happy path, and every number in it read back off the registry rather than
//    off the response, so a verb that returned an id resolving nowhere would fail here.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioSynthGenerateRendersCandidateTest,
    "PinWright.audio.synth.generate.RendersCandidate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioSynthGenerateRendersCandidateTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthGenerateTestHelpers;

    FTestResponseCapture Capture;
    const FString CandidateId = Generate(SimpleRecipe(/*Seed=*/11), /*bAnalyze=*/true, Capture);

    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    TestTrue(TEXT("generate succeeded"), Capture.bSuccess);
    TestFalse(TEXT("a candidate id was returned"), CandidateId.IsEmpty());

    // The id must resolve: a returned identifier that resolves nowhere is the exact defect
    // rpc-design.md §1 lists.
    const FPwCandidateLookupResult Hit = Registry().Get(CandidateId);
    TestTrue(TEXT("the returned id resolves in the candidate registry"), Hit.IsHit());
    if (Hit.IsHit())
    {
        TestEqual(TEXT("the candidate carries the rendered frames"),
            Hit.Candidate->Buffer.NumFrames(), NumberField(Capture.Result, TEXT("frames")));
    }

    const TSharedPtr<FJsonObject> Render = ObjectField(Capture.Result, TEXT("render"));
    TestTrue(TEXT("a render report is present"), Render.IsValid());
    TestTrue(TEXT("the render report is measured"), BoolField(Render, TEXT("measured")));

    // One layer in, one measured row out - and the row's framesMixed is FPwAudioBuffer::MixInto's
    // own return, so a layer that contributed nothing could not report a full mix.
    const TArray<TSharedPtr<FJsonValue>>* Layers = nullptr;
    TestTrue(TEXT("the render report carries per-layer rows"),
        Render.IsValid() && Render->TryGetArrayField(TEXT("layers"), Layers) && Layers);
    if (Layers)
    {
        TestEqual(TEXT("one layer in, one row out"), Layers->Num(), 1);
        if (Layers->Num() == 1)
        {
            const TSharedPtr<FJsonObject> Row = (*Layers)[0]->AsObject();
            TestTrue(TEXT("the layer mixed a non-zero number of frames"),
                NumberField(Row, TEXT("framesMixed"), 0) > 0);
            TestTrue(TEXT("the layer keeps the legacy peak field"),
                Row.IsValid() && Row->HasField(TEXT("peakLinear")));
            TestTrue(TEXT("the layer publishes a clearly named pre-gain peak"),
                Row.IsValid() && Row->HasField(TEXT("peakLinearPreGain")));
            TestTrue(TEXT("the layer publishes a clearly named post-gain peak"),
                Row.IsValid() && Row->HasField(TEXT("peakLinearPostGain")));
        }
    }

    // Normalization was requested by the fixture, so it must report itself measured with a
    // real input level rather than the default 0.0.
    const TSharedPtr<FJsonObject> Normalize = ObjectField(Render, TEXT("normalize"));
    TestTrue(TEXT("the normalize block reports itself measured"),
        BoolField(Normalize, TEXT("measured")));
    TestTrue(TEXT("a measured normalization publishes its input level"),
        Normalize.IsValid() && Normalize->HasField(TEXT("inputDb")));

    TestTrue(TEXT("the summary analysis is present"),
        ObjectField(Capture.Result, TEXT("analysis")).IsValid());
    // No image was requested, so none may be claimed.
    TestFalse(TEXT("no images are reported when none were requested"),
        Capture.Result.IsValid() && Capture.Result->HasField(TEXT("images")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioSynthGenerateTargetsAreScoredTest,
    "PinWright.audio.synth.generate.TargetsAreScoredAgainstAnalysis",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioSynthGenerateTargetsAreScoredTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthGenerateTestHelpers;

    FTestResponseCapture Capture;
    const FString CandidateId = Generate(TargetedRecipe(), /*bAnalyze=*/true, Capture);
    TestTrue(TEXT("targeted generate succeeded"), !CandidateId.IsEmpty());

    const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
    TestTrue(TEXT("the response carries one row per recipe target"),
        Capture.Result.IsValid() && Capture.Result->TryGetArrayField(TEXT("targets"), Rows) && Rows);
    TestEqual(TEXT("three target rows are reported"), Rows ? Rows->Num() : 0, 3);
    TestEqual(TEXT("targetsMet counts only the met row"),
        NumberField(Capture.Result, TEXT("targetsMet")), 1);

    bool bSawMet = false;
    bool bSawMissed = false;
    bool bSawUnscored = false;
    if (Rows)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Rows)
        {
            const TSharedPtr<FJsonObject> Row = Value->AsObject();
            const FString Metric = StringField(Row, TEXT("metric"));
            TestTrue(FString::Printf(TEXT("%s target carries a flat range"), *Metric),
                Row.IsValid() && (Row->HasField(TEXT("min")) || Row->HasField(TEXT("max"))));
            TestFalse(FString::Printf(TEXT("%s target does not nest its range"), *Metric),
                Row.IsValid() && Row->HasField(TEXT("target")));
            TestTrue(FString::Printf(TEXT("%s target carries an explicit tolerance"), *Metric),
                Row.IsValid() && Row->HasField(TEXT("tolerance")));
            TestTrue(FString::Printf(TEXT("%s target carries a status"), *Metric),
                Row.IsValid() && Row->HasField(TEXT("status")));

            const double Tolerance = DoubleField(Row, TEXT("tolerance"), -1.0);
            TestEqual(FString::Printf(TEXT("%s target uses zero additional tolerance"), *Metric),
                Tolerance, 0.0);
            if (Row.IsValid() && Row->HasField(TEXT("measured")))
            {
                double Measured = 0.0;
                const bool bMeasuredFinite = Row->TryGetNumberField(TEXT("measured"), Measured) &&
                    FMath::IsFinite(Measured);
                TestTrue(FString::Printf(TEXT("%s target publishes a finite measured value"), *Metric),
                    bMeasuredFinite);

                if (bMeasuredFinite)
                {
                    bool bExpectedMet = true;
                    if (Row->HasField(TEXT("min")))
                    {
                        double Min = 0.0;
                        const bool bReadable = Row->TryGetNumberField(TEXT("min"), Min);
                        TestTrue(FString::Printf(TEXT("%s target minimum is numeric"), *Metric), bReadable);
                        bExpectedMet = bExpectedMet && bReadable && Measured >= Min - Tolerance;
                    }
                    if (Row->HasField(TEXT("max")))
                    {
                        double Max = 0.0;
                        const bool bReadable = Row->TryGetNumberField(TEXT("max"), Max);
                        TestTrue(FString::Printf(TEXT("%s target maximum is numeric"), *Metric), bReadable);
                        bExpectedMet = bExpectedMet && bReadable && Measured <= Max + Tolerance;
                    }

                    TestTrue(FString::Printf(TEXT("%s measured row carries met"), *Metric),
                        Row->HasField(TEXT("met")));
                    TestTrue(FString::Printf(TEXT("%s published met matches its measured range"), *Metric),
                        BoolField(Row, TEXT("met")) == bExpectedMet);
                    TestEqual(FString::Printf(TEXT("%s status matches its measured range"), *Metric),
                        StringField(Row, TEXT("status")),
                        bExpectedMet ? FString(TEXT("met")) : FString(TEXT("missed")));
                }
            }

            if (Metric == TEXT("durationMs"))
            {
                bSawMet = true;
                TestTrue(TEXT("durationMs carries its fractional flat minimum"),
                    FMath::IsNearlyEqual(DoubleField(Row, TEXT("min")), 120.10415, 1e-9));
                TestTrue(TEXT("durationMs carries its fractional flat maximum"),
                    FMath::IsNearlyEqual(DoubleField(Row, TEXT("max")), 120.10418, 1e-9));
                TestTrue(TEXT("durationMs is measured"), Row->HasField(TEXT("measured")));
                TestTrue(TEXT("durationMs target is met"), BoolField(Row, TEXT("met")));
                TestEqual(TEXT("durationMs status is met"), StringField(Row, TEXT("status")),
                    FString(TEXT("met")));
            }
            else if (Metric == TEXT("centroidHz"))
            {
                bSawMissed = true;
                TestEqual(TEXT("centroidHz carries its flat minimum"), DoubleField(Row, TEXT("min")), 10000.0);
                TestTrue(TEXT("centroidHz is measured"), Row->HasField(TEXT("measured")));
                TestFalse(TEXT("centroidHz target is missed"), BoolField(Row, TEXT("met")));
                TestEqual(TEXT("centroidHz status is missed"), StringField(Row, TEXT("status")),
                    FString(TEXT("missed")));
            }
            else if (Metric == TEXT("decayMs"))
            {
                bSawUnscored = true;
                TestEqual(TEXT("decayMs carries its flat maximum"), DoubleField(Row, TEXT("max")), 10.0);
                TestFalse(TEXT("undefined decayMs omits a met result"), Row->HasField(TEXT("met")));
                TestFalse(TEXT("unscored decayMs fabricates a measured value"),
                    Row->HasField(TEXT("measured")));
                TestEqual(TEXT("undefined decayMs is explicitly unscored"),
                    StringField(Row, TEXT("status")), FString(TEXT("unscored")));
            }
        }
    }

    TestTrue(TEXT("the duration target row is present"), bSawMet);
    TestTrue(TEXT("the centroid target row is present"), bSawMissed);
    TestTrue(TEXT("the undefined decay target row is present"), bSawUnscored);
    return true;
}

// =================================================================================================
// B. generate - §12/§3: a malformed recipe errors with the PARSER's code and its field path.
//    The field path is what the caller edits, so a generic message here is a real regression.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioSynthGenerateMalformedRecipeTest,
    "PinWright.audio.synth.generate.MalformedRecipeReportsFieldPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioSynthGenerateMalformedRecipeTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthGenerateTestHelpers;

    const int32 Before = ResidentCount();

    // "supersaw" is not a generator kind. The parser has a dedicated code for that, one step
    // ahead of UNKNOWN_EFFECT, because an unrecognised generator renders silence.
    const FString Json = RecipeJson(1, 100.0,
        {TEXT("{\"generator\":{\"kind\":\"supersaw\",\"params\":{}}}")});

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("recipe"), ParseJson(Json));

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler found"), InvokeHandlerWithCapture(GenerateMethod, Payload, Capture));
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    TestFalse(TEXT("a malformed recipe is an error, not a silent fallback"), Capture.bSuccess);
    TestEqual(TEXT("the parser's own code is forwarded unmodified"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_UNKNOWN_GENERATOR));

    // The field path travels as STRUCTURE, because prose can be dropped when structuredContent
    // is set (rpc-design.md §7).
    const FString Field = StringField(Capture.Result, TEXT("field"));
    TestTrue(TEXT("the error payload names the offending layer"), Field.Contains(TEXT("layers[0]")));
    TestTrue(TEXT("the error payload names the offending key"), Field.Contains(TEXT("kind")));

    TestEqual(TEXT("nothing was registered for a recipe that never parsed"),
        ResidentCount(), Before);
    return true;
}

// =================================================================================================
// C. generate - a missing recipe and an unknown plot view are both errors (§3).
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioSynthGenerateRejectsBadArgumentsTest,
    "PinWright.audio.synth.generate.RejectsBadArguments",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioSynthGenerateRejectsBadArgumentsTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthGenerateTestHelpers;

    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        TestTrue(TEXT("handler found"), InvokeHandlerWithCapture(GenerateMethod, Payload, Capture));
        TestFalse(TEXT("a missing recipe is an error"), Capture.bSuccess);
    }

    {
        // An unrecognised view must not be dropped: the caller would wait for an image that was
        // never going to arrive.
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("recipe"), SimpleRecipe());
        Payload->SetStringField(TEXT("plots"), TEXT("mel"));
        TestTrue(TEXT("handler found"), InvokeHandlerWithCapture(GenerateMethod, Payload, Capture));
        TestFalse(TEXT("an unknown plot view is an error"), Capture.bSuccess);
        TestEqual(TEXT("the code is INVALID_ARGUMENT"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
        TestTrue(TEXT("the message names the closed set"),
            Capture.Message.Contains(TEXT("waveform")) &&
            Capture.Message.Contains(TEXT("spectrogram")));
    }
    return true;
}

// =================================================================================================
// D. generate - §8: an identical recipe converges onto the candidate it already produced
//    instead of accumulating a second copy. This is what makes a client retry after the
//    transport's response-only timeout safe.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioSynthGenerateIsIdempotentTest,
    "PinWright.audio.synth.generate.IdenticalRecipeConverges",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioSynthGenerateIsIdempotentTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthGenerateTestHelpers;

    // Seed and duration are unique to this test, so the first call in a suite run genuinely
    // renders something new rather than converging onto a sibling test's candidate.
    const TSharedPtr<FJsonObject> Recipe = SimpleRecipe(/*Seed=*/4242, /*DurationMs=*/90.0);

    FTestResponseCapture First;
    const FString FirstId = Generate(Recipe, /*bAnalyze=*/false, First);
    TestFalse(TEXT("the first render produced a candidate"), FirstId.IsEmpty());
    TestFalse(TEXT("the first render reports reused:false"),
        BoolField(First.Result, TEXT("reused"), true));

    const int32 AfterFirst = ResidentCount();

    FTestResponseCapture Second;
    const FString SecondId = Generate(Recipe, /*bAnalyze=*/false, Second);
    TestEqual(TEXT("a retry converges onto the same candidate"), SecondId, FirstId);
    TestTrue(TEXT("the retry reports reused:true, measured from the registry lookup"),
        BoolField(Second.Result, TEXT("reused")));
    TestEqual(TEXT("the retry added no second candidate"), ResidentCount(), AfterFirst);
    return true;
}

// =================================================================================================
// E. generate - response-shape coverage. The worst case requests the layer cap, every analysis
//    family and all three images; all 14 target rows must remain structurally visible.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioSynthGenerateReportsAllTargetsTest,
    "PinWright.audio.synth.generate.ResponseReportsAllTargets",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioSynthGenerateReportsAllTargetsTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthGenerateTestHelpers;

    TArray<TSharedPtr<FJsonValue>> Views;
    Views.Add(MakeShared<FJsonValueString>(TEXT("waveform")));
    Views.Add(MakeShared<FJsonValueString>(TEXT("spectrogram")));
    Views.Add(MakeShared<FJsonValueString>(TEXT("constantq")));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("recipe"), WorstCaseRecipe());
    Payload->SetBoolField(TEXT("analyze"), true);
    Payload->SetArrayField(TEXT("plots"), Views);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler found"), InvokeHandlerWithCapture(GenerateMethod, Payload, Capture));
    TestTrue(TEXT("the worst-case recipe rendered"), Capture.bSuccess);

    const TArray<TSharedPtr<FJsonValue>>* TargetRows = nullptr;
    TestTrue(TEXT("the worst-case response carries its target rows"),
        Capture.Result.IsValid() && Capture.Result->TryGetArrayField(TEXT("targets"), TargetRows) &&
            TargetRows);
    TestEqual(TEXT("all 14 legal targets are reported"), TargetRows ? TargetRows->Num() : 0, 14);

    // ITU-R BS.1770 loudness needs Audio::FLKFSAnalyzer, which the engine ships only from UE 5.8.
    // Before that PwComputeLoudness refuses outright rather than passing off a peak or an RMS
    // number as LUFS (PwAudioFeatures.cpp), so the lufs row is still reported with both bounds but
    // carries no measurement and an explicit unscored status. Every other row stays scoreable.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
    constexpr bool bLoudnessIsScoreable = true;
#else
    constexpr bool bLoudnessIsScoreable = false;
#endif
    const int32 ScoreableTargets = bLoudnessIsScoreable ? 14 : 13;

    int32 MeasuredTargets = 0;
    int32 MetTargets = 0;
    if (TargetRows)
    {
        for (const TSharedPtr<FJsonValue>& Value : *TargetRows)
        {
            const TSharedPtr<FJsonObject> Row = Value->AsObject();
            // Name the metric in every message: an unnamed row failure says a target went
            // unscored without saying which one, which is the whole diagnosis.
            const FString Metric = StringField(Row, TEXT("metric"));
            TestTrue(*FString::Printf(
                    TEXT("worst-case target '%s' carries both flat bounds"), *Metric),
                Row.IsValid() && Row->HasField(TEXT("min")) && Row->HasField(TEXT("max")));
            const bool bMeasured = Row.IsValid() && Row->HasField(TEXT("measured"));
            const bool bMet = Row.IsValid() && Row->HasField(TEXT("met")) &&
                BoolField(Row, TEXT("met"));
            if (!bLoudnessIsScoreable && Metric == TEXT("lufs"))
            {
                TestFalse(TEXT("the refused lufs target fabricates no measured value"), bMeasured);
                TestFalse(TEXT("the refused lufs target claims no met result"),
                    Row.IsValid() && Row->HasField(TEXT("met")));
                TestEqual(TEXT("the refused lufs target is explicitly unscored"),
                    StringField(Row, TEXT("status")), FString(TEXT("unscored")));
            }
            else
            {
                TestTrue(*FString::Printf(TEXT("worst-case target '%s' is measured"), *Metric),
                    bMeasured);
                TestTrue(*FString::Printf(TEXT("worst-case target '%s' is met"), *Metric), bMet);
                TestEqual(*FString::Printf(
                        TEXT("worst-case target '%s' reports met status"), *Metric),
                    StringField(Row, TEXT("status")), FString(TEXT("met")));
            }
            MeasuredTargets += bMeasured ? 1 : 0;
            MetTargets += bMet ? 1 : 0;
        }
    }
    TestEqual(TEXT("every scoreable worst-case target is measured"),
        MeasuredTargets, ScoreableTargets);
    TestEqual(TEXT("every scoreable worst-case target is met"), MetTargets, ScoreableTargets);
    TestEqual(TEXT("targetsMet reports every scoreable worst-case target"),
        NumberField(Capture.Result, TEXT("targetsMet")), ScoreableTargets);

    // The handler reports every requested view; transport-level overflow is covered by the
    // shared condensed-reader spill tests rather than a duplicate local size budget.
    const TArray<TSharedPtr<FJsonValue>>* Images = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* ImageFailures = nullptr;
    const bool bHasImages = Capture.Result.IsValid() &&
        Capture.Result->TryGetArrayField(TEXT("images"), Images) && Images;
    const bool bHasFailures = Capture.Result.IsValid() &&
        Capture.Result->TryGetArrayField(TEXT("imageFailures"), ImageFailures) && ImageFailures;
    const int32 ViewsReported = (bHasImages ? Images->Num() : 0)
        + (bHasFailures ? ImageFailures->Num() : 0);
    TestEqual(TEXT("every requested plot view is reported, produced or failed"),
        ViewsReported, Views.Num());
    // A failed view must say why - a row with no reason is indistinguishable from a view that
    // was never attempted.
    if (bHasFailures)
    {
        for (const TSharedPtr<FJsonValue>& Value : *ImageFailures)
        {
            const TSharedPtr<FJsonObject> Failure = Value->AsObject();
            TestFalse(TEXT("each failed view names itself"),
                StringField(Failure, TEXT("view")).IsEmpty());
            TestFalse(TEXT("each failed view carries a reason"),
                StringField(Failure, TEXT("error")).IsEmpty());
        }
    }

    // Images travel as paths and sizes, never as data. A response that started embedding pixels
    // would blow the gate above, but assert the shape too so the reason is legible.
    if (bHasImages)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Images)
        {
            const TSharedPtr<FJsonObject> Image = Value->AsObject();
            TestTrue(TEXT("each image carries a path"),
                !StringField(Image, TEXT("path")).IsEmpty());
            TestEqual(TEXT("each image declares its mime type"),
                StringField(Image, TEXT("mimeType")), FString(TEXT("image/png")));
            // sizeBytes is read off the file system, not off the encoder - a zero here is a
            // write that reported success and produced nothing.
            TestTrue(TEXT("each image reports a non-zero measured size"),
                NumberField(Image, TEXT("sizeBytes"), 0) > 0);
        }
    }

    return true;
}

// =================================================================================================
// F. patch - the source candidate must be byte-identical afterwards. Compared against a
//    snapshot taken before the patch, so a verb that started rendering over the original would
//    fail here rather than in production.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioSynthPatchLeavesSourceUntouchedTest,
    "PinWright.audio.synth.patch.LeavesSourceUntouched",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioSynthPatchLeavesSourceUntouchedTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthGenerateTestHelpers;

    FTestResponseCapture GenerateCapture;
    const FString SourceId = Generate(SimpleRecipe(/*Seed=*/31, /*DurationMs=*/110.0),
                                      /*bAnalyze=*/false, GenerateCapture);
    if (SourceId.IsEmpty())
    {
        AddError(TEXT("could not create the source candidate this test patches"));
        return false;
    }

    FPwAudioBuffer BeforeBuffer;
    TestTrue(TEXT("the source candidate's audio was snapshot"),
        SnapshotBuffer(SourceId, BeforeBuffer));
    const FString BeforeRecipe = CanonicalRecipeOf(SourceId);

    TArray<TSharedPtr<FJsonValue>> Ops;
    Ops.Add(MakeShared<FJsonValueObject>(
        MakeNumberOp(TEXT("replace"), TEXT("/layers/0/gainDb"), -18.0)));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("candidateId"), SourceId);
    Payload->SetArrayField(TEXT("patch"), Ops);
    Payload->SetBoolField(TEXT("analyze"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler found"), InvokeHandlerWithCapture(PatchMethod, Payload, Capture));
    TestTrue(TEXT("the patch rendered"), Capture.bSuccess);

    const FString NewId = StringField(Capture.Result, TEXT("candidateId"));
    TestEqual(TEXT("the response reports the source id"),
        StringField(Capture.Result, TEXT("sourceCandidateId")), SourceId);
    TestFalse(TEXT("a new candidate id was returned"), NewId.IsEmpty());
    TestTrue(TEXT("the patched candidate is a DIFFERENT candidate"), NewId != SourceId);
    TestTrue(TEXT("the patch reports itself as a real change"),
        BoolField(Capture.Result, TEXT("changed")));
    TestTrue(TEXT("the source is reported still resident, measured by a fresh lookup"),
        BoolField(Capture.Result, TEXT("sourceStillResident")));

    // The assertion this test exists for.
    FPwAudioBuffer AfterBuffer;
    TestTrue(TEXT("the source candidate is still resolvable"),
        SnapshotBuffer(SourceId, AfterBuffer));
    TestTrue(TEXT("the source candidate's audio is byte-identical after the patch"),
        BuffersAreByteIdentical(BeforeBuffer, AfterBuffer));
    TestEqual(TEXT("the source candidate's recipe is unchanged after the patch"),
        CanonicalRecipeOf(SourceId), BeforeRecipe);

    // And the patched candidate really is a different render, not a copy of the source.
    FPwAudioBuffer PatchedBuffer;
    if (SnapshotBuffer(NewId, PatchedBuffer))
    {
        TestFalse(TEXT("a -18 dB gain patch produced different audio"),
            BuffersAreByteIdentical(BeforeBuffer, PatchedBuffer));
    }
    return true;
}

// =================================================================================================
// G. patch - a patch whose result does not parse errors with the parser's field path and
//    registers NOTHING. Counted off the registry, not read off the response.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioSynthPatchInvalidResultCreatesNothingTest,
    "PinWright.audio.synth.patch.InvalidResultCreatesNoCandidate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioSynthPatchInvalidResultCreatesNothingTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthGenerateTestHelpers;

    FTestResponseCapture GenerateCapture;
    const FString SourceId = Generate(SimpleRecipe(/*Seed=*/55, /*DurationMs=*/100.0),
                                      /*bAnalyze=*/false, GenerateCapture);
    if (SourceId.IsEmpty())
    {
        AddError(TEXT("could not create the source candidate this test patches"));
        return false;
    }

    const int32 Before = ResidentCount();

    // -5 ms is below PwSynthLimits::MinDurationMs, so the patched document is well-formed JSON
    // that is not a legal recipe - the case that separates "the patch applied" from "the result
    // is renderable".
    TArray<TSharedPtr<FJsonValue>> Ops;
    Ops.Add(MakeShared<FJsonValueObject>(
        MakeNumberOp(TEXT("replace"), TEXT("/durationMs"), -5.0)));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("candidateId"), SourceId);
    Payload->SetArrayField(TEXT("patch"), Ops);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler found"), InvokeHandlerWithCapture(PatchMethod, Payload, Capture));
    TestFalse(TEXT("an unrenderable patch result is an error"), Capture.bSuccess);
    TestEqual(TEXT("the parser's out-of-range code is forwarded"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_PARAMS));
    TestEqual(TEXT("the error payload names the offending field"),
        StringField(Capture.Result, TEXT("field")), FString(TEXT("durationMs")));
    TestEqual(TEXT("the error payload still names the source candidate"),
        StringField(Capture.Result, TEXT("sourceCandidateId")), SourceId);
    TestFalse(TEXT("the response states that no candidate was created"),
        BoolField(Capture.Result, TEXT("candidateCreated"), true));

    TestEqual(TEXT("no candidate was registered"), ResidentCount(), Before);
    return true;
}

// =================================================================================================
// H. patch - the two patch-document faults have different codes because they have different
//    remedies: fix the patch (INVALID_PARAMS) vs re-read the recipe (VERIFICATION_FAILED).
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioSynthPatchFaultsAreDistinctTest,
    "PinWright.audio.synth.patch.PatchFaultsAreDistinct",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioSynthPatchFaultsAreDistinctTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthGenerateTestHelpers;

    FTestResponseCapture GenerateCapture;
    const FString SourceId = Generate(SimpleRecipe(/*Seed=*/91, /*DurationMs=*/100.0),
                                      /*bAnalyze=*/false, GenerateCapture);
    if (SourceId.IsEmpty())
    {
        AddError(TEXT("could not create the source candidate this test patches"));
        return false;
    }

    const auto RunPatch = [&SourceId](const TArray<TSharedPtr<FJsonValue>>& Ops,
                                      FTestResponseCapture& Capture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("candidateId"), SourceId);
        Payload->SetArrayField(TEXT("patch"), Ops);
        Payload->SetBoolField(TEXT("analyze"), false);
        return InvokeHandlerWithCapture(PatchMethod, Payload, Capture);
    };

    {
        // An empty patch is a selector that matched nothing, not a re-render.
        FTestResponseCapture Capture;
        RunPatch(TArray<TSharedPtr<FJsonValue>>(), Capture);
        TestFalse(TEXT("an empty patch array is an error"), Capture.bSuccess);
        TestEqual(TEXT("an empty patch reports INVALID_PARAMS"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_PARAMS));
    }

    {
        // An operation name outside RFC-6902 is rejected, never treated as a no-op.
        TArray<TSharedPtr<FJsonValue>> Ops;
        Ops.Add(MakeShared<FJsonValueObject>(
            MakeNumberOp(TEXT("increment"), TEXT("/layers/0/gainDb"), 1.0)));
        FTestResponseCapture Capture;
        RunPatch(Ops, Capture);
        TestFalse(TEXT("an unknown patch op is an error"), Capture.bSuccess);
        TestEqual(TEXT("an unknown op reports INVALID_PARAMS"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestEqual(TEXT("the failing operation is named by index"),
            NumberField(Capture.Result, TEXT("opIndex")), 0);
    }

    {
        // A pointer into nothing must fail rather than silently create the path.
        TArray<TSharedPtr<FJsonValue>> Ops;
        Ops.Add(MakeShared<FJsonValueObject>(
            MakeNumberOp(TEXT("replace"), TEXT("/layers/9/gainDb"), -3.0)));
        FTestResponseCapture Capture;
        RunPatch(Ops, Capture);
        TestFalse(TEXT("a pointer past the end of an array is an error"), Capture.bSuccess);
        TestEqual(TEXT("a bad pointer reports INVALID_PARAMS"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_PARAMS));
    }

    {
        // A failed `test` op is NOT a malformed patch: the document is not what the caller
        // asserted, and the remedy is to re-read the recipe.
        TArray<TSharedPtr<FJsonValue>> Ops;
        Ops.Add(MakeShared<FJsonValueObject>(
            MakeNumberOp(TEXT("test"), TEXT("/layers/0/gainDb"), 999.0)));
        FTestResponseCapture Capture;
        RunPatch(Ops, Capture);
        TestFalse(TEXT("a failed test op is an error"), Capture.bSuccess);
        TestEqual(TEXT("a failed test op reports VERIFICATION_FAILED, not INVALID_PARAMS"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_VERIFICATION_FAILED));
    }

    {
        // A `test` op that HOLDS must not block the rest of the patch.
        TArray<TSharedPtr<FJsonValue>> Ops;
        Ops.Add(MakeShared<FJsonValueObject>(
            MakeNumberOp(TEXT("test"), TEXT("/sampleRate"), TestSampleRate)));
        Ops.Add(MakeShared<FJsonValueObject>(
            MakeNumberOp(TEXT("replace"), TEXT("/layers/0/pan"), 0.5)));
        FTestResponseCapture Capture;
        RunPatch(Ops, Capture);
        TestTrue(TEXT("a satisfied test op lets the patch through"), Capture.bSuccess);
        TestTrue(TEXT("the surviving edit is reported as a change"),
            BoolField(Capture.Result, TEXT("changed")));
    }
    return true;
}

// =================================================================================================
// I. patch / export - an unknown candidate id must come back with the REGISTRY's distinct code.
//    NO_CANDIDATES and CANDIDATE_NOT_FOUND point at different next moves, so a verb that
//    collapsed them would send an agent down the wrong path.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioSynthCandidateMissCodesTest,
    "PinWright.audio.synth.patch.UnknownCandidateReportsRegistryCode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioSynthCandidateMissCodesTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthGenerateTestHelpers;

    const auto PatchUnknown = [](const FString& Id, FTestResponseCapture& Capture)
    {
        TArray<TSharedPtr<FJsonValue>> Ops;
        Ops.Add(MakeShared<FJsonValueObject>(
            MakeNumberOp(TEXT("replace"), TEXT("/layers/0/gainDb"), -3.0)));
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("candidateId"), Id);
        Payload->SetArrayField(TEXT("patch"), Ops);
        InvokeHandlerWithCapture(PatchMethod, Payload, Capture);
    };

    // 1. Nothing has been generated: "generate something first", not "your id is wrong".
    //    Candidates are session-scoped scratch, so clearing them here corrupts nothing - each
    //    sibling test creates the candidates it needs inside its own body.
    Registry().DiscardAll();
    {
        FTestResponseCapture Capture;
        PatchUnknown(TEXT("c999_dead"), Capture);
        TestFalse(TEXT("an unknown id on an empty registry is an error"), Capture.bSuccess);
        TestEqual(TEXT("an empty registry reports NO_CANDIDATES"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_NO_CANDIDATES));
    }

    // 2. The registry holds something, so an unknown id is a wrong id.
    FTestResponseCapture GenerateCapture;
    const FString RealId = Generate(SimpleRecipe(/*Seed=*/77, /*DurationMs=*/80.0),
                                    /*bAnalyze=*/false, GenerateCapture);
    if (RealId.IsEmpty())
    {
        AddError(TEXT("could not create the candidate this test needs"));
        return false;
    }
    {
        FTestResponseCapture Capture;
        PatchUnknown(TEXT("c999_dead"), Capture);
        TestFalse(TEXT("an unknown id is an error"), Capture.bSuccess);
        TestEqual(TEXT("a non-empty registry reports CANDIDATE_NOT_FOUND for an unknown id"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_CANDIDATE_NOT_FOUND));
        TestEqual(TEXT("the miss payload names the status"),
            StringField(Capture.Result, TEXT("status")),
            FString(FPwCandidateRegistry::LexLookupStatus(EPwCandidateLookup::NeverExisted)));
    }

    // 3. A candidate the caller discarded is CANDIDATE_NOT_FOUND too - same remedy - but the
    //    payload separates it from a wrong id, which is the whole reason removals are recorded.
    Registry().Discard(RealId);
    {
        FTestResponseCapture Capture;
        PatchUnknown(RealId, Capture);
        TestFalse(TEXT("a discarded id is an error"), Capture.bSuccess);
        TestEqual(TEXT("a discarded candidate reports CANDIDATE_NOT_FOUND"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_CANDIDATE_NOT_FOUND));
        TestEqual(TEXT("the miss payload distinguishes discarded from never-existed"),
            StringField(Capture.Result, TEXT("status")),
            FString(FPwCandidateRegistry::LexLookupStatus(EPwCandidateLookup::Discarded)));
    }

    // 4. The third code is the registry's single writer's job, and it is what the handler
    //    forwards; assert the mapping itself so a change to it fails here rather than only in
    //    a session that happened to hit the byte budget.
    TestEqual(TEXT("an evicted candidate maps to its own code"),
        FString(FPwCandidateRegistry::MissErrorCode(EPwCandidateLookup::Evicted)),
        FString(ErrorCodes::ERR_CANDIDATE_EVICTED));
    return true;
}

// =================================================================================================
// J. variations - count 0 errors rather than returning an empty success, and a sweep with
//    nothing to vary is refused instead of converging onto one candidate and reporting N.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioSynthVariationsRejectsEmptySweepTest,
    "PinWright.audio.synth.variations.RejectsEmptySweep",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioSynthVariationsRejectsEmptySweepTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthGenerateTestHelpers;

    FTestResponseCapture GenerateCapture;
    const FString SourceId = Generate(SimpleRecipe(/*Seed=*/17, /*DurationMs=*/80.0),
                                      /*bAnalyze=*/false, GenerateCapture);
    if (SourceId.IsEmpty())
    {
        AddError(TEXT("could not create the source candidate this test varies"));
        return false;
    }

    const auto RunVariations = [&SourceId](int32 Count, bool bVarySeed,
                                           FTestResponseCapture& Capture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("candidateId"), SourceId);
        Payload->SetNumberField(TEXT("count"), Count);
        Payload->SetBoolField(TEXT("varySeed"), bVarySeed);
        InvokeHandlerWithCapture(VariationsMethod, Payload, Capture);
    };

    const int32 Before = ResidentCount();

    {
        FTestResponseCapture Capture;
        RunVariations(0, /*bVarySeed=*/true, Capture);
        TestTrue(TEXT("handler responded"), Capture.bWasCalled);
        TestFalse(TEXT("count=0 is an error, not an empty success"), Capture.bSuccess);
        TestEqual(TEXT("count=0 reports INVALID_ARGUMENT"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
        // A rejected sweep must not have issued a job ticket either.
        TestTrue(TEXT("no ticket was issued for a rejected sweep"),
            StringField(Capture.Result, TEXT("ticket_id")).IsEmpty());
    }

    {
        FTestResponseCapture Capture;
        RunVariations(9, /*bVarySeed=*/true, Capture);
        TestFalse(TEXT("a count above the cap is an error"), Capture.bSuccess);
    }

    {
        // Neither mutations nor varySeed: every "variation" would be the base recipe.
        FTestResponseCapture Capture;
        RunVariations(3, /*bVarySeed=*/false, Capture);
        TestFalse(TEXT("a sweep with nothing to vary is an error"), Capture.bSuccess);
        TestEqual(TEXT("it reports INVALID_PARAMS"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_PARAMS));
    }

    {
        // A mutation path that is not a number in the canonical recipe is refused up front,
        // before any render - silently skipping it would produce N identical renders that
        // looked like a working sweep.
        TArray<TSharedPtr<FJsonValue>> Mutations;
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("path"), TEXT("/layers/0/generator/kind"));
        Entry->SetNumberField(TEXT("relative"), 0.2);
        Mutations.Add(MakeShared<FJsonValueObject>(Entry));

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("candidateId"), SourceId);
        Payload->SetNumberField(TEXT("count"), 2);
        Payload->SetArrayField(TEXT("mutations"), Mutations);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(VariationsMethod, Payload, Capture);
        TestFalse(TEXT("a non-numeric mutation target is an error"), Capture.bSuccess);
        TestTrue(TEXT("the message names the offending entry"),
            Capture.Message.Contains(TEXT("mutations[0]")));
    }

    TestEqual(TEXT("no rejected sweep registered a candidate"), ResidentCount(), Before);
    return true;
}

// =================================================================================================
// K. variations - the happy path: a job ticket, a table of ids and metrics, and no ranking.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioSynthVariationsTableReportsRowsTest,
    "PinWright.audio.synth.variations.TableReportsRows",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioSynthVariationsTableReportsRowsTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthGenerateTestHelpers;

    FTestResponseCapture GenerateCapture;
    const FString SourceId = Generate(SimpleRecipe(/*Seed=*/23, /*DurationMs=*/600.0),
                                      /*bAnalyze=*/false, GenerateCapture);
    if (SourceId.IsEmpty())
    {
        AddError(TEXT("could not create the source candidate this test varies"));
        return false;
    }

    TArray<TSharedPtr<FJsonValue>> Mutations;
    TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
    Entry->SetStringField(TEXT("path"), TEXT("/layers/0/generator/params/frequencyHz"));
    Entry->SetNumberField(TEXT("relative"), 0.25);
    Mutations.Add(MakeShared<FJsonValueObject>(Entry));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("candidateId"), SourceId);
    Payload->SetNumberField(TEXT("count"), 8);
    Payload->SetArrayField(TEXT("mutations"), Mutations);
    Payload->SetBoolField(TEXT("varySeed"), true);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler found"), InvokeHandlerWithCapture(VariationsMethod, Payload, Capture));
    TestTrue(TEXT("the sweep was accepted"), Capture.bSuccess);

    // The immediate response is the ticket - the work is long enough that the response must not
    // wait on it (§9).
    TestEqual(TEXT("the immediate response is a job ticket"),
        StringField(Capture.Result, TEXT("status")), FString(TEXT("running")));
    TestFalse(TEXT("the ticket carries an id"),
        StringField(Capture.Result, TEXT("ticket_id")).IsEmpty());
    TestFalse(TEXT("the ticket states it cannot be cancelled"),
        BoolField(Capture.Result, TEXT("cancellable"), true));

    FJobTicket Ticket;
    TestTrue(TEXT("the ticket resolves in the job registry"), ReadJobTicket(Capture, Ticket));
    TestEqual(TEXT("the job reached a terminal completed state"),
        Ticket.Status, FString(TEXT("completed")));

    const TSharedPtr<FJsonObject> Result = Ticket.Result;
    TestTrue(TEXT("the job carries a result"), Result.IsValid());
    TestEqual(TEXT("every requested variation was attempted"),
        NumberField(Result, TEXT("requested")), 8);
    TestTrue(TEXT("at least one variation rendered"),
        NumberField(Result, TEXT("rendered"), 0) > 0);

    const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
    TestTrue(TEXT("a variations table is present"),
        Result.IsValid() && Result->TryGetArrayField(TEXT("variations"), Rows) && Rows);
    if (Rows)
    {
        TestEqual(TEXT("one row per requested variation"), Rows->Num(), 8);
        for (const TSharedPtr<FJsonValue>& Value : *Rows)
        {
            const TSharedPtr<FJsonObject> Row = Value->AsObject();
            if (!BoolField(Row, TEXT("rendered")))
            {
                // A failed row must say WHY, with a code, rather than being silently dropped.
                TestFalse(TEXT("a failed row carries an error code"),
                    StringField(Row, TEXT("errorCode")).IsEmpty());
                continue;
            }
            TestFalse(TEXT("a rendered row carries a candidate id"),
                StringField(Row, TEXT("id")).IsEmpty());
            // The table is evidence, not a verdict: nothing may rank or select.
            TestFalse(TEXT("no row carries a score"), Row->HasField(TEXT("score")));
            TestFalse(TEXT("no row is flagged as the winner"), Row->HasField(TEXT("best")));
        }
    }
    TestFalse(TEXT("the table itself carries no ranking"),
        Result.IsValid() && (Result->HasField(TEXT("best")) || Result->HasField(TEXT("ranked"))));

    return true;
}

// =================================================================================================
// L. export - §10: asset creation plus a package save plus a blocking bulk-data read must be
//    gated through the shared table, never a hand-written gate.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioSynthExportGatedAsTickUnsafeTest,
    "PinWright.audio.synth.export.GatedAsTickUnsafe",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioSynthExportGatedAsTickUnsafeTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthGenerateTestHelpers;

    TestTrue(TEXT("audio.synth.export is in the tick-unsafe method table"),
        PinWrightSafePoint::IsTickUnsafeMethod(FString(ExportMethod)));
    TestTrue(TEXT("the table entry is spelled the same as the registered handler"),
        PinWrightSafePoint::GetTickUnsafeMethods().Contains(FString(ExportMethod)));

    // The render verbs are deliberately NOT in the table: they touch no package and no asset,
    // so listing them would defer work that is safe mid-frame.
    TestFalse(TEXT("audio.synth.generate is not gated"),
        PinWrightSafePoint::IsTickUnsafeMethod(FString(GenerateMethod)));
    return true;
}

// =================================================================================================
// M. export - a path outside /Game leaves nothing behind. Asserted against the object table and
//    the package registry, not against the response.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioSynthExportInvalidPathLeavesNothingTest,
    "PinWright.audio.synth.export.InvalidPathLeavesNothingBehind",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioSynthExportInvalidPathLeavesNothingTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthGenerateTestHelpers;

    FTestResponseCapture GenerateCapture;
    const FString CandidateId = Generate(SimpleRecipe(/*Seed=*/61, /*DurationMs=*/60.0),
                                         /*bAnalyze=*/false, GenerateCapture);
    if (CandidateId.IsEmpty())
    {
        AddError(TEXT("could not create the candidate this test exports"));
        return false;
    }

    const FString AssetName = UniqueAssetName(TEXT("SW_SynthBadPath_"));
    const FString Folder = TEXT("/Engine/PinWrightTests");
    const FString ObjectPath = FString::Printf(TEXT("%s/%s.%s"), *Folder, *AssetName, *AssetName);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("candidateId"), CandidateId);
    Payload->SetStringField(TEXT("name"), AssetName);
    Payload->SetStringField(TEXT("path"), Folder);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler found"), InvokeHandlerWithCapture(ExportMethod, Payload, Capture));
    TestFalse(TEXT("a folder outside /Game is an error"), Capture.bSuccess);
    TestEqual(TEXT("the code is INVALID_PATH"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_PATH));

    // The failure direction that matters: the engine writer relocates a path it dislikes to the
    // Content root with only a log warning, so a rejection that happened too late would leave a
    // stray asset somewhere else.
    TestNull(TEXT("no object was created at the rejected path"),
        StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath));
    TestFalse(TEXT("no package was created at the rejected path"),
        FPackageName::DoesPackageExist(FString::Printf(TEXT("%s/%s"), *Folder, *AssetName)));
    // And the relocation target the writer would have used is empty too.
    TestNull(TEXT("nothing was relocated into the Content root"),
        StaticFindObject(UObject::StaticClass(), nullptr,
            *FString::Printf(TEXT("/Game/%s.%s"), *AssetName, *AssetName)));
    return true;
}

// =================================================================================================
// N. export - the §4 verification and §8 idempotence. The verification is a decode-back through
//    a different subsystem than the writer, so the test asserts the comparison it produced
//    rather than that the call returned true.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioSynthExportVerifiesAndConvergesTest,
    "PinWright.audio.synth.export.VerifiesAndIsIdempotent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioSynthExportVerifiesAndConvergesTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthGenerateTestHelpers;

    FTestResponseCapture GenerateCapture;
    const FString CandidateId = Generate(SimpleRecipe(/*Seed=*/13, /*DurationMs=*/120.0),
                                         /*bAnalyze=*/false, GenerateCapture);
    if (CandidateId.IsEmpty())
    {
        AddError(TEXT("could not create the candidate this test exports"));
        return false;
    }

    const FString AssetName = UniqueAssetName(TEXT("SW_SynthExport_"));
    const FString ObjectPath = FString::Printf(TEXT("%s/%s.%s"),
        TestAssetFolder, *AssetName, *AssetName);

    const auto RunExport = [&CandidateId, &AssetName](FTestResponseCapture& Capture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("candidateId"), CandidateId);
        Payload->SetStringField(TEXT("name"), AssetName);
        Payload->SetStringField(TEXT("path"), TestAssetFolder);
        // save:false throughout: the suite must not write .uasset files into the host project.
        Payload->SetBoolField(TEXT("save"), false);
        InvokeHandlerWithCapture(ExportMethod, Payload, Capture);
    };

    FTestResponseCapture First;
    RunExport(First);

    TestTrue(TEXT("the export succeeded"), First.bSuccess);
    const FString AssetPath = StringField(First.Result, TEXT("assetPath"));
    TestEqual(TEXT("the asset landed at the requested path"), AssetPath, ObjectPath);

    // §4: the verification block must be a real comparison, and its verdict must be derived
    // from the decode rather than asserted.
    const TSharedPtr<FJsonObject> Verification = ObjectField(First.Result, TEXT("verification"));
    TestTrue(TEXT("a verification block is present"), Verification.IsValid());
    TestTrue(TEXT("the verification reports itself measured"),
        BoolField(Verification, TEXT("measured")));
    TestTrue(TEXT("the round trip passed"), BoolField(Verification, TEXT("pass")));
    TestTrue(TEXT("the decoded frame count matched"), BoolField(Verification, TEXT("framesMatch")));
    TestTrue(TEXT("the decoded sample rate matched"),
        BoolField(Verification, TEXT("sampleRateMatch")));
    TestTrue(TEXT("the payload header was read for the channel count"),
        BoolField(Verification, TEXT("payloadHeaderRead")));
    // The per-channel deltas must exist even on a pass, so a failure can say by how much.
    TestTrue(TEXT("the left channel comparison is published"),
        ObjectField(Verification, TEXT("left")).IsValid());
    TestTrue(TEXT("the right channel comparison is published"),
        ObjectField(Verification, TEXT("right")).IsValid());

    // The frame count is read off the DECODE, so it must agree with the candidate's own buffer.
    FPwAudioBuffer CandidateBuffer;
    const bool bHaveCandidateBuffer = SnapshotBuffer(CandidateId, CandidateBuffer);
    if (bHaveCandidateBuffer)
    {
        TestEqual(TEXT("the decoded frame count matches the candidate's buffer"),
            NumberField(First.Result, TEXT("frames")), CandidateBuffer.NumFrames());
    }

    // The samples themselves, not a statistic of them. The handler's verification compares a
    // per-channel RMS/PEAK signature (AudioSynthGenerateHandler.cpp:1797-1833) - both
    // magnitude-only and order-invariant - so a write that swapped the two channels, inverted
    // the polarity or reversed the sample order produces an identical signature and reports
    // pass:true. Everything asserted above is downstream of that verdict, so the content of
    // the asset is only held by reading it back here.
    USoundWave* Written = Cast<USoundWave>(
        StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath));
    TestNotNull(TEXT("the exported asset resolves as a USoundWave"), Written);
    if (Written && bHaveCandidateBuffer)
    {
        FPwAudioBuffer Decoded;
        FString DecodeError;
        const bool bDecoded = PwDecodeSoundWave(Written, Decoded, DecodeError);
        TestTrue(FString::Printf(TEXT("the exported asset decodes back (error='%s')"),
            *DecodeError), bDecoded);
        if (bDecoded)
        {
            TestEqual(TEXT("the decoded buffer keeps the candidate's frame count"),
                Decoded.NumFrames(), CandidateBuffer.NumFrames());
            TestEqual(TEXT("the decoded buffer keeps the candidate's sample rate"),
                Decoded.SampleRate, CandidateBuffer.SampleRate);

            const int32 ComparedFrames =
                FMath::Min(Decoded.NumFrames(), CandidateBuffer.NumFrames());
            TestTrue(TEXT("there were samples to compare"), ComparedFrames > 0);

            // The export writes 16-bit PCM (PwAudioExport.cpp: BuildInt16SampleBuffer), whose
            // quantisation step is ~3.1e-5, so the handler's own 1e-3 tolerance leaves three
            // decades of headroom: anything above it is a content difference, not rounding.
            constexpr float SampleTolerance = 1.0e-3f;
            float WorstLeft = 0.0f;
            float WorstRight = 0.0f;
            for (int32 Frame = 0; Frame < ComparedFrames; ++Frame)
            {
                WorstLeft = FMath::Max(WorstLeft,
                    FMath::Abs(Decoded.Left[Frame] - CandidateBuffer.Left[Frame]));
                WorstRight = FMath::Max(WorstRight,
                    FMath::Abs(Decoded.Right[Frame] - CandidateBuffer.Right[Frame]));
            }
            TestTrue(FString::Printf(
                TEXT("every left sample survived the round trip (worst |delta| %g)"),
                WorstLeft), WorstLeft <= SampleTolerance);
            TestTrue(FString::Printf(
                TEXT("every right sample survived the round trip (worst |delta| %g)"),
                WorstRight), WorstRight <= SampleTolerance);
        }
    }

    // §5: save:false must report the mark-dirty triple honestly rather than claiming durability.
    TestFalse(TEXT("an unsaved export does not claim to be saved"),
        BoolField(First.Result, TEXT("saved"), true));

    // §8: a retry converges on the same asset rather than making a second one.
    FTestResponseCapture Second;
    RunExport(Second);
    TestTrue(TEXT("the re-export succeeded"), Second.bSuccess);
    TestEqual(TEXT("the re-export converged on the same asset path"),
        StringField(Second.Result, TEXT("assetPath")), AssetPath);
    TestTrue(TEXT("the re-export found the existing asset"),
        BoolField(Second.Result, TEXT("existing")));
    TestEqual(TEXT("the re-export updated in place rather than creating a second asset"),
        StringField(Second.Result, TEXT("mode")), FString(TEXT("updated_in_place")));
    TestTrue(TEXT("the re-export verified too"),
        BoolField(ObjectField(Second.Result, TEXT("verification")), TEXT("pass")));

    // Reclaim the never-saved asset through GC rather than force-delete, which crashes the
    // suite under -unattended on a never-reloaded asset.
    PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ObjectPath);
    return true;
}

// =================================================================================================
// N2. export - the in-place half of §8. An idempotent rewrite that keeps the ASSET but resets its
//     properties is not idempotence, it is silent data loss: the wave that comes back has the
//     audio that was asked for and no SoundClass, so it escapes every SoundMix and plays at full
//     level at any distance, while the verb answers pass:true (board
//     B-synth-export-wipes-soundclass-attenuation, four encounters).
//
//     Both halves are asserted from the OBJECT, not from the response: the routing and the
//     per-wave properties survive, and the payload-derived fields do NOT - the second export uses
//     a shorter candidate, so a stale Duration is a failure in the opposite direction.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioSynthExportInPlaceKeepsPropertiesTest,
    "PinWright.audio.synth.export.InPlaceRewriteKeepsNonPayloadProperties",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioSynthExportInPlaceKeepsPropertiesTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthGenerateTestHelpers;

    // Two candidates of DIFFERENT length, so the rewrite has to move the payload-derived fields
    // as well as leave the rest alone.
    FTestResponseCapture FirstGenerate;
    const FString LongCandidateId = Generate(SimpleRecipe(/*Seed=*/21, /*DurationMs=*/240.0),
                                             /*bAnalyze=*/false, FirstGenerate);
    FTestResponseCapture SecondGenerate;
    const FString ShortCandidateId = Generate(SimpleRecipe(/*Seed=*/22, /*DurationMs=*/80.0),
                                              /*bAnalyze=*/false, SecondGenerate);
    if (LongCandidateId.IsEmpty() || ShortCandidateId.IsEmpty())
    {
        AddError(TEXT("could not create the two candidates this test exports"));
        return false;
    }

    const FString AssetName = UniqueAssetName(TEXT("SW_SynthInPlace_"));
    const FString ObjectPath = FString::Printf(TEXT("%s/%s.%s"),
        TestAssetFolder, *AssetName, *AssetName);

    const auto RunExport = [&AssetName](const FString& CandidateId, FTestResponseCapture& Capture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("candidateId"), CandidateId);
        Payload->SetStringField(TEXT("name"), AssetName);
        Payload->SetStringField(TEXT("path"), TestAssetFolder);
        // save:false throughout: the suite must not write .uasset files into the host project.
        Payload->SetBoolField(TEXT("save"), false);
        InvokeHandlerWithCapture(ExportMethod, Payload, Capture);
    };

    FTestResponseCapture Created;
    RunExport(LongCandidateId, Created);
    if (!Created.bSuccess)
    {
        AddError(TEXT("the first export failed, so there is nothing to rewrite"));
        return false;
    }

    // A create carries no routing, and the response has to SAY so - the fourth encounter on the
    // board was a caller who could not tell "was wiped" from "was never set" from the answer.
    const TSharedPtr<FJsonObject> CreatedRouting = ObjectField(Created.Result, TEXT("routing"));
    TestTrue(TEXT("a create publishes its routing block"), CreatedRouting.IsValid());
    TestEqual(TEXT("a freshly created wave reports no sound class"),
        StringField(CreatedRouting, TEXT("soundClass")), FString());
    TestEqual(TEXT("a freshly created wave reports no attenuation"),
        StringField(CreatedRouting, TEXT("attenuationSettings")), FString());

    USoundWave* Wave = Cast<USoundWave>(
        StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath));
    if (!Wave)
    {
        AddError(TEXT("the exported asset did not resolve as a USoundWave"));
        return false;
    }

    // The routing this ticket is about, plus two of the "per-wave properties" the verb used to
    // document as reset. Transient outers: nothing here is saved, and the wave keeps both alive
    // through its own UPROPERTYs for as long as it exists.
    USoundClass* SoundClass = NewObject<USoundClass>(GetTransientPackage());
    USoundAttenuation* Attenuation = NewObject<USoundAttenuation>(GetTransientPackage());
    if (!SoundClass || !Attenuation)
    {
        AddError(TEXT("could not build the routing probes this test assigns"));
        return false;
    }

    Wave->SoundClassObject = SoundClass;
    Wave->AttenuationSettings = Attenuation;
    Wave->bLooping = true;
    Wave->Volume = 0.42f;
    Wave->SubtitlePriority = 7.5f;

    const int32 CreatedFrames = NumberField(Created.Result, TEXT("frames"));

    FTestResponseCapture Rewritten;
    RunExport(ShortCandidateId, Rewritten);

    TestTrue(TEXT("the in-place re-export succeeded"), Rewritten.bSuccess);
    TestEqual(TEXT("the re-export rewrote the same asset"),
        StringField(Rewritten.Result, TEXT("assetPath")), ObjectPath);
    TestEqual(TEXT("the re-export took the in-place path"),
        StringField(Rewritten.Result, TEXT("mode")), FString(TEXT("updated_in_place")));

    // The response half: the verdict now covers preservation, so a regression fails the verb
    // instead of returning pass:true beside the loss.
    const TSharedPtr<FJsonObject> Verification = ObjectField(Rewritten.Result, TEXT("verification"));
    TestTrue(TEXT("the rewrite reports every non-payload property preserved"),
        BoolField(Verification, TEXT("propertiesPreserved")));
    TestFalse(TEXT("no property is listed as changed"),
        Verification.IsValid() && Verification->HasField(TEXT("changedProperties")));
    TestTrue(TEXT("the rewrite verified"), BoolField(Verification, TEXT("pass")));

    const TSharedPtr<FJsonObject> RewrittenRouting =
        ObjectField(Rewritten.Result, TEXT("routing"));
    TestEqual(TEXT("the response reports the sound class that survived"),
        StringField(RewrittenRouting, TEXT("soundClass")), SoundClass->GetPathName());
    TestEqual(TEXT("the response reports the attenuation that survived"),
        StringField(RewrittenRouting, TEXT("attenuationSettings")), Attenuation->GetPathName());

    // The object half, which is what the response is only a report of. Re-resolved by path rather
    // than reused from above, so a rewrite that replaced the object is caught here.
    USoundWave* After = Cast<USoundWave>(
        StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath));
    TestNotNull(TEXT("the asset still resolves after the rewrite"), After);
    if (After)
    {
        TestTrue(TEXT("SoundClassObject survived the rewrite"),
            After->SoundClassObject == SoundClass);
        TestTrue(TEXT("AttenuationSettings survived the rewrite"),
            After->AttenuationSettings == Attenuation);
        TestTrue(TEXT("bLooping survived the rewrite"), After->bLooping != 0);
        TestEqual(TEXT("Volume survived the rewrite"), After->Volume, 0.42f);
        TestEqual(TEXT("SubtitlePriority survived the rewrite"), After->SubtitlePriority, 7.5f);

        // The other direction: the payload-derived fields must NOT survive, or the asset would
        // claim a length it no longer has.
        const int32 RewrittenFrames = NumberField(Rewritten.Result, TEXT("frames"));
        TestTrue(TEXT("the two candidates really differ in length"),
            CreatedFrames > 0 && RewrittenFrames > 0 && RewrittenFrames != CreatedFrames);

        FPwAudioBuffer ShortBuffer;
        if (SnapshotBuffer(ShortCandidateId, ShortBuffer))
        {
            TestEqual(TEXT("the decoded frame count followed the second candidate"),
                RewrittenFrames, ShortBuffer.NumFrames());

            const float ExpectedDuration = static_cast<float>(ShortBuffer.NumFrames()) /
                                           static_cast<float>(ShortBuffer.SampleRate);
            // One frame of slack: Duration is a float division of the same two integers.
            TestTrue(FString::Printf(
                TEXT("Duration was refreshed to the new payload (%f vs %f)"),
                After->Duration, ExpectedDuration),
                FMath::IsNearlyEqual(After->Duration, ExpectedDuration, 1.0e-4f));
        }
    }

    // Reclaim the never-saved asset through GC rather than force-delete, which crashes the
    // suite under -unattended on a never-reloaded asset.
    PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ObjectPath);
    return true;
}

// =================================================================================================
// O. export - an unknown candidate id reaches the same registry codes here as it does in patch,
//    because both doors call MissErrorCode rather than inventing their own answer.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioSynthExportUnknownCandidateTest,
    "PinWright.audio.synth.export.UnknownCandidateReportsRegistryCode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioSynthExportUnknownCandidateTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthGenerateTestHelpers;

    // Make sure the registry is non-empty, so this is the "wrong id" case rather than the
    // "nothing generated yet" one.
    FTestResponseCapture GenerateCapture;
    const FString RealId = Generate(SimpleRecipe(/*Seed=*/29, /*DurationMs=*/60.0),
                                    /*bAnalyze=*/false, GenerateCapture);
    if (RealId.IsEmpty())
    {
        AddError(TEXT("could not create the candidate this test needs"));
        return false;
    }

    const FString AssetName = UniqueAssetName(TEXT("SW_SynthMissing_"));
    const FString ObjectPath = FString::Printf(TEXT("%s/%s.%s"),
        TestAssetFolder, *AssetName, *AssetName);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("candidateId"), TEXT("c999_dead"));
    Payload->SetStringField(TEXT("name"), AssetName);
    Payload->SetStringField(TEXT("path"), TestAssetFolder);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler found"), InvokeHandlerWithCapture(ExportMethod, Payload, Capture));
    TestFalse(TEXT("an unknown candidate id is an error"), Capture.bSuccess);
    TestEqual(TEXT("the registry's code is forwarded"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_CANDIDATE_NOT_FOUND));

    // Candidate resolution happens before anything is created, so a rejected export cannot have
    // left a package behind.
    TestNull(TEXT("no asset was created for a rejected export"),
        StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath));
    return true;
}

// =================================================================================================
// P. export - a missing required argument is an error, never a defaulted path or name (§3).
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioSynthExportRequiresArgumentsTest,
    "PinWright.audio.synth.export.RequiresArguments",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioSynthExportRequiresArgumentsTest::RunTest(const FString& Parameters)
{
    using namespace PwSynthGenerateTestHelpers;

    const auto ExpectRejection = [this](const TSharedPtr<FJsonObject>& Payload,
                                        const TCHAR* What)
    {
        FTestResponseCapture Capture;
        TestTrue(TEXT("handler found"),
            InvokeHandlerWithCapture(PwSynthGenerateTestHelpers::ExportMethod, Payload, Capture));
        TestTrue(TEXT("handler responded"), Capture.bWasCalled);
        TestFalse(FString::Printf(TEXT("%s is rejected rather than defaulted"), What),
            Capture.bSuccess);
    };

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        ExpectRejection(Payload, TEXT("an empty payload"));
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("candidateId"), TEXT("c1_0000"));
        Payload->SetStringField(TEXT("path"), PwSynthGenerateTestHelpers::TestAssetFolder);
        ExpectRejection(Payload, TEXT("a missing asset name"));
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("candidateId"), TEXT("c1_0000"));
        Payload->SetStringField(TEXT("name"), TEXT("SW_NoFolder"));
        ExpectRejection(Payload, TEXT("a missing folder"));
    }
    return true;
}
