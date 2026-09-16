// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for audio.music.describe_schema / render_stems / export_stems / build_interactive.
//
// The bias is rpc-design.md §12: a test asserting that one of these verbs succeeded proves almost
// nothing, because every defect this family can ship LOOKS like a success. The assertions that
// carry weight are the ones that break if a verb starts answering "yes":
//
//   - a malformed score must ERROR carrying the PARSER'S OWN field path, because that path is the
//     entire value of the error to the caller who has to patch it;
//   - an empty track list must ERROR, because a zero-stem success is exactly the shape a bad
//     selector takes and reads identically to a clean run (§3, §7);
//   - an inexact tempo must report exact:false with a NON-ZERO SIGNED residual AND a
//     nearestExactBpm that really is exact when re-rendered - the last of those is the assertion
//     that breaks if anyone rounds the recovery information away;
//   - export_stems on an unknown candidate must keep the REGISTRY's distinct code, because
//     CANDIDATE_EVICTED / NO_CANDIDATES / CANDIDATE_NOT_FOUND are three different next moves and
//     flattening them is what makes an agent "fix" an id it got right;
//   - a re-run of export_stems must converge on the same asset rather than creating a sibling (§8);
//   - build_interactive with an unresolvable stem path must ERROR AND LEAVE NO ASSET, asserted
//     against the object table rather than against the response;
//   - both jobs must state cancellable:false and name JOB_CANCEL_UNSUPPORTED, because claiming a
//     cancellation that cannot happen is worse than not having one (§9);
//   - the tick-unsafe registrations are asserted through the shared table, never re-implemented;
//   - the response-size gates hold, so growth trips CI instead of a user's response.
//
// SIZE GATES. The wrapped MCP ToolResult carries the payload twice - escaped in content[0].text and
// verbatim in structuredContent - so the real ceiling for a bare result is about 4,250 characters
// (board ticket E-spill-threshold-measured-post-wrap). Measurement uses the PRETTY writer, which is
// what JsonRpc::Serialize and HttpResponseSpill actually take; a condensed measurement understates
// the budget by roughly 20%.
//
// JOB VERBS UNDER TEST. FHandlerContext::StartJob invokes its bind delegate synchronously and the
// test context has no subsystem, so the captured response is the STARTED payload (ticket id,
// cancellable:false) and the terminal result lands in FJobRegistry - the same split
// TestAudioSynthGenerate.cpp asserts against for audio.synth.variations.

#include "Misc/AutomationTest.h"

#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwAudioDecode.h"
#include "AudioGen/PwCandidateRegistry.h"
#include "AudioGen/PwMusicGraph.h"
#include "AudioGen/PwMusicScore.h"
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
#include "Sound/SoundWave.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

// Named (not anonymous) namespace: the module builds with bUseUnity = true and the sibling audio
// test TUs already own several anonymous helpers of similar name.
namespace PwMusicHandlerTests
{
    const TCHAR* const DescribeSchemaMethod   = TEXT("audio.music.describe_schema");
    const TCHAR* const RenderStemsMethod      = TEXT("audio.music.render_stems");
    const TCHAR* const ExportStemsMethod      = TEXT("audio.music.export_stems");
    const TCHAR* const BuildInteractiveMethod = TEXT("audio.music.build_interactive");

    const TCHAR* const TestAssetFolder = TEXT("/Game/PinWrightTests");

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

    double NumberField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, double Default = -1.0)
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

    const TArray<TSharedPtr<FJsonValue>>* ArrayField(const TSharedPtr<FJsonObject>& Object,
                                                     const TCHAR* Key)
    {
        const TArray<TSharedPtr<FJsonValue>>* Found = nullptr;
        if (Object.IsValid() && Object->TryGetArrayField(Key, Found))
        {
            return Found;
        }
        return nullptr;
    }

    bool ReadJobTicket(const FTestResponseCapture& Capture, FJobTicket& Out)
    {
        const FString TicketId = StringField(Capture.Result, TEXT("ticket_id"));
        return !TicketId.IsEmpty() && FPluginState::Get().GetJobRegistry().Get(TicketId, Out);
    }

    TSharedPtr<FJsonObject> ParseJson(const FString& Json)
    {
        TSharedPtr<FJsonObject> Root;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
        FJsonSerializer::Deserialize(Reader, Root);
        return Root;
    }

    FString UniqueAssetName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // ---------------------------------------------------------------------------------------
    // Score fixtures. Built as JSON text and handed to the handler, so every test exercises the
    // same parse the production path runs.
    // ---------------------------------------------------------------------------------------

    /** The cheapest instrument that renders: one sine oscillator, no envelope, no effects. */
    FString MinimalInstrument()
    {
        return TEXT(R"({"generator":{"kind":"osc","params":{"waveform":"sine","frequencyHz":440}}})");
    }

    /** One drone track. Deliberately short: every test here renders for real. */
    FString ScoreJson(double Bpm, int32 Bars, int32 SampleRate, const FString& TracksBody)
    {
        return FString::Printf(
            TEXT(R"({"bpm":%.17g,"bars":%d,"sampleRate":%d,)")
            TEXT(R"("timeSignature":{"numerator":4,"denominator":4},)")
            TEXT(R"("key":{"root":"a","scale":"natural_minor_aeolian"},"tracks":[%s]})"),
            Bpm, Bars, SampleRate, *TracksBody);
    }

    FString DroneTrackJson(const FString& Name)
    {
        return FString::Printf(
            TEXT(R"({"name":"%s","role":"drone","octave":2,"instrument":%s,)")
            TEXT(R"("generation":{"mode":"drone","params":{"degrees":[0]}}})"),
            *Name, *MinimalInstrument());
    }

    /** Smallest score the schema accepts: one bar, one drone track, one note. */
    TSharedPtr<FJsonObject> MinimalScore(double Bpm = 240.0, int32 SampleRate = 48000)
    {
        return ParseJson(ScoreJson(Bpm, /*Bars=*/1, SampleRate, DroneTrackJson(TEXT("bed"))));
    }

    /**
     * The worst case the per-stem table has to survive: the track cap, so the response carries
     * PwMusicLimits::MaxTracks rows. One bar at 240 bpm is 1 s per stem, which keeps the registry
     * cost of the fixture around 6 MB.
     */
    TSharedPtr<FJsonObject> MaxTrackScore()
    {
        TArray<FString> Tracks;
        for (int32 Index = 0; Index < PwMusicLimits::MaxTracks; ++Index)
        {
            Tracks.Add(DroneTrackJson(FString::Printf(TEXT("track_%02d"), Index)));
        }
        return ParseJson(ScoreJson(240.0, /*Bars=*/1, 48000, FString::Join(Tracks, TEXT(","))));
    }

    /** Runs render_stems and returns the terminal job result, or null. */
    TSharedPtr<FJsonObject> RenderStems(const TSharedPtr<FJsonObject>& Score,
                                        FTestResponseCapture& Capture, FJobTicket& OutTicket)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("score"), Score);
        if (!InvokeHandlerWithCapture(RenderStemsMethod, Payload, Capture) || !Capture.bSuccess)
        {
            return nullptr;
        }
        if (!ReadJobTicket(Capture, OutTicket))
        {
            return nullptr;
        }
        return OutTicket.Result;
    }

    /** Candidate ids from a render_stems result, in track order. */
    TArray<FString> StemIds(const TSharedPtr<FJsonObject>& Result)
    {
        TArray<FString> Ids;
        if (const TArray<TSharedPtr<FJsonValue>>* Rows = ArrayField(Result, TEXT("stems")))
        {
            for (const TSharedPtr<FJsonValue>& Row : *Rows)
            {
                const TSharedPtr<FJsonObject>* RowObject = nullptr;
                if (Row.IsValid() && Row->TryGetObject(RowObject) && RowObject)
                {
                    Ids.Add(StringField(*RowObject, TEXT("id")));
                }
            }
        }
        return Ids;
    }

    /** Frees a fixture's candidates so a later test in the same run is not evicted by it. */
    void DiscardCandidates(const TArray<FString>& CandidateIds)
    {
        FPwCandidateRegistry& Registry = FPluginState::Get().GetCandidateRegistry();
        for (const FString& CandidateId : CandidateIds)
        {
            Registry.Discard(CandidateId);
        }
    }

    bool AssetExistsInMemory(const FString& ObjectPath)
    {
        return StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath) != nullptr;
    }
}

// =================================================================================================
// A. describe_schema - an unknown section and an unknown mode must be REJECTED, not silently
//    degraded into the default. A section that quietly became "overview" would hand the caller a
//    document that answers a question they did not ask.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioMusicDescribeSchemaRejectsUnknownsTest,
    "PinWright.audio.music.describe_schema.UnknownSectionAndModeAreRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioMusicDescribeSchemaRejectsUnknownsTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicHandlerTests;

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("section"), TEXT("everything"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler found"),
            InvokeHandlerWithCapture(DescribeSchemaMethod, Payload, Capture));
        TestFalse(TEXT("an unknown section is refused rather than defaulted"), Capture.bSuccess);
        TestEqual(TEXT("the refusal uses INVALID_PARAMS"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_PARAMS));
        // §7: the way out is named, not merely implied.
        TestTrue(TEXT("the message names the accepted sections"),
            Capture.Message.Contains(TEXT("overview")) && Capture.Message.Contains(TEXT("modes")));
    }

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("section"), TEXT("modes"));
        Payload->SetStringField(TEXT("mode"), TEXT("arpeggio"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler found"),
            InvokeHandlerWithCapture(DescribeSchemaMethod, Payload, Capture));
        TestFalse(TEXT("an unknown generation mode is refused"), Capture.bSuccess);
        TestEqual(TEXT("the refusal uses INVALID_PARAMS"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestTrue(TEXT("the message names the accepted modes"),
            Capture.Message.Contains(TEXT("evolving_pad")));
    }

    return true;
}

// =================================================================================================
// B. describe_schema - the cold-start section must fit the wrapped ceiling, and the worked example
//    must be validated by the REAL parser rather than asserted. An example that stopped validating
//    would otherwise ship as documentation.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioMusicDescribeSchemaFitsCeilingTest,
    "PinWright.audio.music.describe_schema.SectionsFitCeilingAndExampleValidates",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioMusicDescribeSchemaFitsCeilingTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicHandlerTests;

    const TArray<FString> Sections = {
        TEXT("overview"), TEXT("rules"), TEXT("example"), TEXT("scales"), TEXT("roles"), TEXT("modes")
    };

    for (const FString& Section : Sections)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("section"), Section);

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler found"),
            InvokeHandlerWithCapture(DescribeSchemaMethod, Payload, Capture));
        TestTrue(FString::Printf(TEXT("section '%s' succeeded"), *Section), Capture.bSuccess);

        const int32 Chars = MeasureResponseChars(Capture.Result);
        AddInfo(FString::Printf(TEXT("describe_schema '%s': %d chars (ceiling %d)"),
            *Section, Chars, WrappedResponseCeiling));
        TestTrue(FString::Printf(TEXT("section '%s' (%d chars) fits the wrapped ceiling (%d)"),
            *Section, Chars, WrappedResponseCeiling), Chars <= WrappedResponseCeiling);
    }

    // The largest single-mode drill-down must fit too - that is the one a caller opens while
    // writing a track, and a spill there would cost a file read on the cold-start path.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("section"), TEXT("modes"));
        Payload->SetStringField(TEXT("mode"), TEXT("sparse_events"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler found"),
            InvokeHandlerWithCapture(DescribeSchemaMethod, Payload, Capture));
        TestTrue(TEXT("the single-mode drill-down succeeded"), Capture.bSuccess);

        const int32 Chars = MeasureResponseChars(Capture.Result);
        AddInfo(FString::Printf(TEXT("describe_schema modes/sparse_events: %d chars (ceiling %d)"),
            Chars, WrappedResponseCeiling));
        TestTrue(FString::Printf(TEXT("the sparse_events table (%d chars) fits the ceiling (%d)"),
            Chars, WrappedResponseCeiling), Chars <= WrappedResponseCeiling);
    }

    // §1: `exampleValidated` must be a measurement, and the only way it can be one here is for the
    // built-in example to survive the production parser.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("section"), TEXT("example"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler found"),
            InvokeHandlerWithCapture(DescribeSchemaMethod, Payload, Capture));
        TestTrue(TEXT("the example section succeeded"), Capture.bSuccess);
        TestTrue(FString::Printf(TEXT("the built-in example validates (error='%s')"),
            *StringField(Capture.Result, TEXT("exampleError"))),
            BoolField(Capture.Result, TEXT("exampleValidated")));
        const TSharedPtr<FJsonObject> Example = ObjectField(Capture.Result, TEXT("example"));
        TestTrue(TEXT("the example is published as a parsed object"), Example.IsValid());

        // `exampleValidated` on its own is the handler grading its own homework: AddExample
        // computes the flag and publishes the document, so a flag hardcoded true - or computed
        // over a different document than the one that shipped - round-trips perfectly. Re-parse
        // the PUBLISHED example here, which is the only thing a caller ever receives.
        if (Example.IsValid())
        {
            FPwMusicScore Parsed;
            FString ParseError;
            // Parsed first, then reported: the message has to carry the error the call just
            // produced, and argument evaluation order would not guarantee that inline.
            const bool bExampleParses = ParseMusicScore(Example, Parsed, ParseError);
            TestTrue(FString::Printf(
                TEXT("the example AS PUBLISHED survives the score parser (error='%s')"),
                *ParseError), bExampleParses);
        }
    }

    return true;
}

// =================================================================================================
// C. render_stems - a malformed score must ERROR carrying the PARSER'S OWN field path. The path is
//    the entire value of the error to the caller who has to patch it, so a verb that flattened it
//    into "invalid score" would be strictly less useful than the library it wraps.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioMusicRenderStemsMalformedScoreTest,
    "PinWright.audio.music.render_stems.MalformedScoreKeepsParserFieldPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioMusicRenderStemsMalformedScoreTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicHandlerTests;

    // An unrecognised scale. It must not degrade into major, and the error must name key.scale.
    const FString BadScale = ScoreJson(120.0, 1, 48000, DroneTrackJson(TEXT("bed")))
        .Replace(TEXT("natural_minor_aeolian"), TEXT("hungarian_minor"));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("score"), ParseJson(BadScale));

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler found"), InvokeHandlerWithCapture(RenderStemsMethod, Payload, Capture));
    TestFalse(TEXT("a malformed score is refused"), Capture.bSuccess);
    TestEqual(TEXT("the parser's own code survives the handler"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_PARAMS));
    TestEqual(TEXT("the structured payload carries the exact field to patch"),
        StringField(Capture.Result, TEXT("field")), FString(TEXT("key.scale")));
    TestTrue(TEXT("the message names the field too"),
        Capture.Message.Contains(TEXT("key.scale")));
    TestTrue(TEXT("the message names the offending token"),
        Capture.Message.Contains(TEXT("hungarian_minor")));
    // Validation happens BEFORE the ticket, so a caller error is never a failed job (§9).
    TestTrue(TEXT("no job ticket was issued for a caller error"),
        StringField(Capture.Result, TEXT("ticket_id")).IsEmpty());

    // An instrument failure is re-rooted onto the score's own path rather than the SFX parser's
    // "layers[0]...", which a score document has no way to patch.
    {
        const FString BadGenerator = ScoreJson(120.0, 1, 48000, DroneTrackJson(TEXT("bed")))
            .Replace(TEXT(R"("kind":"osc")"), TEXT(R"("kind":"supersaw")"));

        TSharedPtr<FJsonObject> GeneratorPayload = MakeShared<FJsonObject>();
        GeneratorPayload->SetObjectField(TEXT("score"), ParseJson(BadGenerator));

        FTestResponseCapture GeneratorCapture;
        TestTrue(TEXT("handler found"),
            InvokeHandlerWithCapture(RenderStemsMethod, GeneratorPayload, GeneratorCapture));
        TestFalse(TEXT("an unknown instrument generator is refused"), GeneratorCapture.bSuccess);
        TestEqual(TEXT("the generator code is forwarded unchanged"),
            GeneratorCapture.ErrorCode, FString(ErrorCodes::ERR_UNKNOWN_GENERATOR));
        TestEqual(TEXT("the field path is the score's, not the recipe's"),
            StringField(GeneratorCapture.Result, TEXT("field")),
            FString(TEXT("tracks[0].instrument.generator.kind")));
    }

    return true;
}

// =================================================================================================
// D. render_stems - an empty track list must ERROR. Zero is not a small number: a zero-stem success
//    reads identically to a clean run and is exactly the shape a bad selector takes (§3, §7).
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioMusicRenderStemsEmptyTracksTest,
    "PinWright.audio.music.render_stems.EmptyTrackListIsAnError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioMusicRenderStemsEmptyTracksTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicHandlerTests;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("score"), ParseJson(ScoreJson(120.0, 1, 48000, FString())));

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler found"), InvokeHandlerWithCapture(RenderStemsMethod, Payload, Capture));
    TestFalse(TEXT("a trackless score is refused rather than rendering nothing"), Capture.bSuccess);
    TestEqual(TEXT("the refusal is a shape error"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_RECIPE));
    TestTrue(TEXT("the error names the tracks field"), Capture.Message.Contains(TEXT("tracks")));
    TestTrue(TEXT("no job ticket was issued"),
        StringField(Capture.Result, TEXT("ticket_id")).IsEmpty());

    // A missing `score` argument is a different failure and must not be answered with a default.
    {
        TSharedPtr<FJsonObject> Empty = MakeShared<FJsonObject>();
        FTestResponseCapture MissingCapture;
        TestTrue(TEXT("handler found"),
            InvokeHandlerWithCapture(RenderStemsMethod, Empty, MissingCapture));
        TestFalse(TEXT("a missing score is refused"), MissingCapture.bSuccess);
    }

    return true;
}

// =================================================================================================
// E. render_stems - the honest case. An inexact tempo must report exact:false, a NON-ZERO SIGNED
//    residual, and a nearestExactBpm that really is exact when re-rendered.
//
//    That last assertion is the one that matters most: the recovery information is a ratio, so any
//    rounding of it on the way out silently makes the caller's "fix" come back inexact, and every
//    other field would still look right.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioMusicRenderStemsInexactLoopTest,
    "PinWright.audio.music.render_stems.InexactLoopIsReportedWithUsableRecovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioMusicRenderStemsInexactLoopTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicHandlerTests;

    // One bar of 4/4 at 130 bpm and 44.1 kHz is 81415.38 frames - not a whole number, and the
    // canonical inexact case TestPwMusicScore.cpp pins at the library level.
    FTestResponseCapture Capture;
    FJobTicket Ticket;
    const TSharedPtr<FJsonObject> Result =
        RenderStems(MinimalScore(/*Bpm=*/130.0, /*SampleRate=*/44100), Capture, Ticket);
    if (!Result.IsValid())
    {
        AddError(FString::Printf(TEXT("render_stems produced no job result (status='%s', error='%s')"),
            *Ticket.Status, *Ticket.Error));
        return false;
    }
    TestEqual(FString::Printf(TEXT("the job completed (error='%s')"), *Ticket.Error),
        Ticket.Status, FString(TEXT("completed")));

    const TSharedPtr<FJsonObject> Loop = ObjectField(Result, TEXT("loop"));
    TestTrue(TEXT("a loop report is published"), Loop.IsValid());
    TestFalse(TEXT("130 bpm at 44.1 kHz over one bar is NOT sample-exact"),
        BoolField(Loop, TEXT("exact"), true));
    TestEqual(TEXT("the nearest whole frame count is reported"),
        static_cast<int32>(NumberField(Loop, TEXT("frames"))), 81415);

    // Signed, and non-zero: a one-sided or absolute residual would score "long by a third of a
    // frame" and "short by a third" alike (§6).
    const double Residual = NumberField(Loop, TEXT("residualFrames"), 0.0);
    TestTrue(TEXT("the residual is reported rather than swallowed"), FMath::Abs(Residual) > 0.3);
    TestTrue(TEXT("the residual is signed toward the unrounded value"), Residual > 0.0);

    // §7: recovery information that cannot be used is a dead end with extra steps.
    const double NearestExactBpm = NumberField(Loop, TEXT("nearestExactBpm"), 0.0);
    TestTrue(TEXT("a nearby exact tempo is offered, and it is not the one that was asked for"),
        NearestExactBpm > 130.0 && NearestExactBpm < 130.01);

    DiscardCandidates(StemIds(Result));

    // Re-render at the offered tempo. This breaks the moment anyone rounds nearestExactBpm on the
    // way out, which is precisely the failure mode the field exists to avoid.
    FTestResponseCapture FixedCapture;
    FJobTicket FixedTicket;
    const TSharedPtr<FJsonObject> Fixed =
        RenderStems(MinimalScore(NearestExactBpm, /*SampleRate=*/44100), FixedCapture, FixedTicket);
    if (!Fixed.IsValid())
    {
        AddError(TEXT("the re-render at nearestExactBpm produced no job result"));
        return false;
    }
    const TSharedPtr<FJsonObject> FixedLoop = ObjectField(Fixed, TEXT("loop"));
    TestTrue(TEXT("the offered tempo really is exact"), BoolField(FixedLoop, TEXT("exact")));
    TestEqual(TEXT("the offered tempo keeps the frame count"),
        static_cast<int32>(NumberField(FixedLoop, TEXT("frames"))), 81415);

    DiscardCandidates(StemIds(Fixed));
    return true;
}

// =================================================================================================
// F. render_stems - the ticket must not claim a cancellation it cannot deliver, and the full-width
//    table (PwMusicLimits::MaxTracks rows) must fit the wrapped ceiling.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioMusicRenderStemsTicketAndSizeTest,
    "PinWright.audio.music.render_stems.TicketIsHonestAndTableFitsCeiling",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioMusicRenderStemsTicketAndSizeTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicHandlerTests;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("score"), MaxTrackScore());

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler found"), InvokeHandlerWithCapture(RenderStemsMethod, Payload, Capture));
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

    const TArray<TSharedPtr<FJsonValue>>* Rows = ArrayField(Ticket.Result, TEXT("stems"));
    TestTrue(TEXT("the table carries one row per track"),
        Rows != nullptr && Rows->Num() == PwMusicLimits::MaxTracks);
    // §5b: "we looked at every stem" and "a stem was never looked at" must not score alike.
    TestEqual(TEXT("every stem reports itself measured"),
        static_cast<int32>(NumberField(Ticket.Result, TEXT("measuredStems"))),
        PwMusicLimits::MaxTracks);

    // Nothing may return audio inline, in either direction.
    TestFalse(TEXT("no sample array travels in the response"),
        Ticket.Result.IsValid() && (Ticket.Result->HasField(TEXT("samples")) ||
                                    Ticket.Result->HasField(TEXT("pcm"))));

    const int32 Chars = MeasureResponseChars(Ticket.Result);
    AddInfo(FString::Printf(TEXT("render_stems x%d: %d chars (ceiling %d)"),
        PwMusicLimits::MaxTracks, Chars, WrappedResponseCeiling));
    TestTrue(FString::Printf(TEXT("the %d-stem table (%d chars) fits the wrapped ceiling (%d)"),
        PwMusicLimits::MaxTracks, Chars, WrappedResponseCeiling), Chars <= WrappedResponseCeiling);

    DiscardCandidates(StemIds(Ticket.Result));
    return true;
}

// =================================================================================================
// G. render_stems - §8. The transport's timeout is response-only, so a retry must CONVERGE. The
//    assertion is on the ids, not on a flag: a verb could report reused:true and still have added a
//    second set.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioMusicRenderStemsIsIdempotentTest,
    "PinWright.audio.music.render_stems.RetryConvergesOnTheSameCandidates",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioMusicRenderStemsIsIdempotentTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicHandlerTests;

    // A tempo nothing else in this file uses, so the memo entry cannot be one another test left.
    const double Bpm = 172.0;

    FTestResponseCapture FirstCapture;
    FJobTicket FirstTicket;
    const TSharedPtr<FJsonObject> First = RenderStems(MinimalScore(Bpm), FirstCapture, FirstTicket);
    if (!First.IsValid())
    {
        AddError(TEXT("the first render produced no job result"));
        return false;
    }
    TestFalse(TEXT("the first render is not a reuse"), BoolField(First, TEXT("reused"), true));

    const TArray<FString> FirstIds = StemIds(First);
    TestEqual(TEXT("the first render produced one stem"), FirstIds.Num(), 1);

    const int32 CountAfterFirst =
        FPluginState::Get().GetCandidateRegistry().Usage().NumCandidates;

    FTestResponseCapture SecondCapture;
    FJobTicket SecondTicket;
    const TSharedPtr<FJsonObject> Second = RenderStems(MinimalScore(Bpm), SecondCapture, SecondTicket);
    if (!Second.IsValid())
    {
        AddError(TEXT("the retry produced no job result"));
        return false;
    }

    TestTrue(TEXT("the retry reports itself a reuse"), BoolField(Second, TEXT("reused")));

    const TArray<FString> SecondIds = StemIds(Second);
    TestEqual(TEXT("the retry returned the same number of stems"), SecondIds.Num(), FirstIds.Num());
    for (int32 Index = 0; Index < FMath::Min(SecondIds.Num(), FirstIds.Num()); ++Index)
    {
        TestEqual(FString::Printf(TEXT("stem %d kept its candidate id"), Index),
            SecondIds[Index], FirstIds[Index]);
    }
    // The measurement that a flag cannot fake: the registry did not grow.
    TestEqual(TEXT("the retry added no candidates"),
        FPluginState::Get().GetCandidateRegistry().Usage().NumCandidates, CountAfterFirst);
    // The counts belong to the render that took them, so a reuse must not republish them (§4).
    TestTrue(TEXT("a reuse says why it carries no per-stem counts"),
        !StringField(Second, TEXT("countsNote")).IsEmpty());
    // The loop report is a pure function of the score, so it must still be there.
    TestTrue(TEXT("a reuse still carries the loop report"),
        ObjectField(Second, TEXT("loop")).IsValid());

    // Evicting one member must make the WHOLE set a miss - a partially-evicted set reported as a
    // complete one is the §1 defect this check exists to prevent.
    DiscardCandidates(FirstIds);

    FTestResponseCapture ThirdCapture;
    FJobTicket ThirdTicket;
    const TSharedPtr<FJsonObject> Third = RenderStems(MinimalScore(Bpm), ThirdCapture, ThirdTicket);
    if (!Third.IsValid())
    {
        AddError(TEXT("the post-discard render produced no job result"));
        return false;
    }
    TestFalse(TEXT("a discarded set is re-rendered rather than reported as reused"),
        BoolField(Third, TEXT("reused"), true));

    DiscardCandidates(StemIds(Third));
    return true;
}

// =================================================================================================
// H. export_stems - an empty list must ERROR, and an unknown candidate id must keep the REGISTRY's
//    own distinct code. Flattening CANDIDATE_EVICTED / NO_CANDIDATES / CANDIDATE_NOT_FOUND is what
//    makes an agent "fix" an id it got right.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioMusicExportStemsRejectionsTest,
    "PinWright.audio.music.export_stems.EmptyListAndUnknownCandidateAreRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioMusicExportStemsRejectionsTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicHandlerTests;

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetArrayField(TEXT("stems"), TArray<TSharedPtr<FJsonValue>>());
        Payload->SetStringField(TEXT("path"), TestAssetFolder);

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler found"),
            InvokeHandlerWithCapture(ExportStemsMethod, Payload, Capture));
        TestFalse(TEXT("an empty stem list is refused rather than exporting nothing"),
            Capture.bSuccess);
        TestEqual(TEXT("the refusal is an argument error"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
        TestTrue(TEXT("no job ticket was issued"),
            StringField(Capture.Result, TEXT("ticket_id")).IsEmpty());
    }

    // Render something first, so the registry is non-empty: this is the "wrong id" case rather
    // than the "nothing has been generated yet" one, and the two must not answer alike.
    FTestResponseCapture RenderCapture;
    FJobTicket RenderTicket;
    const TSharedPtr<FJsonObject> Rendered =
        RenderStems(MinimalScore(/*Bpm=*/188.0), RenderCapture, RenderTicket);
    if (!Rendered.IsValid())
    {
        AddError(TEXT("could not render the stem this test needs"));
        return false;
    }

    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("candidateId"), TEXT("c999999_dead"));
        Entry->SetStringField(TEXT("name"), UniqueAssetName(TEXT("SW_MusicMissing_")));

        TArray<TSharedPtr<FJsonValue>> Stems;
        Stems.Add(MakeShared<FJsonValueObject>(Entry));

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetArrayField(TEXT("stems"), Stems);
        Payload->SetStringField(TEXT("path"), TestAssetFolder);
        Payload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler found"),
            InvokeHandlerWithCapture(ExportStemsMethod, Payload, Capture));
        TestFalse(TEXT("an unknown candidate id is refused"), Capture.bSuccess);
        // The registry's own vocabulary, reached through MissErrorCode - not a code this verb
        // invented for itself.
        TestEqual(TEXT("the miss keeps the registry's distinct code"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_CANDIDATE_NOT_FOUND));
        TestTrue(TEXT("the miss carries the registry's recovery payload"),
            Capture.Result.IsValid() && Capture.Result->HasField(TEXT("status")));
        // Resolution happens BEFORE the ticket, so this is a real error code and not a failed job.
        TestTrue(TEXT("no job ticket was issued for a bad id"),
            StringField(Capture.Result, TEXT("ticket_id")).IsEmpty());
    }

    DiscardCandidates(StemIds(Rendered));
    return true;
}

// =================================================================================================
// I. export_stems - the §4 verification and the §8 convergence, plus the size gate. The verification
//    is a decode-back through a different subsystem than the writer, so the assertions are on the
//    comparison it produced rather than on the call returning true.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioMusicExportStemsVerifiesAndConvergesTest,
    "PinWright.audio.music.export_stems.VerifiesAndIsIdempotent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioMusicExportStemsVerifiesAndConvergesTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicHandlerTests;

    FTestResponseCapture RenderCapture;
    FJobTicket RenderTicket;
    const TSharedPtr<FJsonObject> Rendered =
        RenderStems(MinimalScore(/*Bpm=*/196.0), RenderCapture, RenderTicket);
    if (!Rendered.IsValid())
    {
        AddError(TEXT("could not render the stem this test exports"));
        return false;
    }
    const TArray<FString> Ids = StemIds(Rendered);
    if (Ids.Num() != 1 || Ids[0].IsEmpty())
    {
        AddError(TEXT("the render produced no usable candidate id"));
        return false;
    }

    const FString TrackName = UniqueAssetName(TEXT("SW_MusicStem_"));
    const FString ObjectPath = FString::Printf(TEXT("%s/%s.%s"),
        TestAssetFolder, *TrackName, *TrackName);

    const auto RunExport = [&Ids, &TrackName](FTestResponseCapture& Capture, FJobTicket& OutTicket)
        -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("candidateId"), Ids[0]);
        Entry->SetStringField(TEXT("name"), TrackName);

        TArray<TSharedPtr<FJsonValue>> Stems;
        Stems.Add(MakeShared<FJsonValueObject>(Entry));

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetArrayField(TEXT("stems"), Stems);
        Payload->SetStringField(TEXT("path"), TestAssetFolder);
        // save:false throughout: the suite must not write .uasset files into the host project.
        Payload->SetBoolField(TEXT("save"), false);

        if (!InvokeHandlerWithCapture(ExportStemsMethod, Payload, Capture) || !Capture.bSuccess)
        {
            return nullptr;
        }
        return ReadJobTicket(Capture, OutTicket) ? OutTicket.Result : nullptr;
    };

    FTestResponseCapture FirstCapture;
    FJobTicket FirstTicket;
    const TSharedPtr<FJsonObject> First = RunExport(FirstCapture, FirstTicket);
    if (!First.IsValid())
    {
        AddError(FString::Printf(TEXT("the export produced no job result (status='%s', error='%s')"),
            *FirstTicket.Status, *FirstTicket.Error));
        DiscardCandidates(Ids);
        return false;
    }

    TestFalse(TEXT("the export ticket does not claim it can be cancelled"),
        BoolField(FirstCapture.Result, TEXT("cancellable"), true));
    TestTrue(TEXT("the export ticket names the code job_cancel will return"),
        StringField(FirstCapture.Result, TEXT("message")).Contains(TEXT("JOB_CANCEL_UNSUPPORTED")));

    TestEqual(FString::Printf(TEXT("the export job completed (error='%s')"), *FirstTicket.Error),
        FirstTicket.Status, FString(TEXT("completed")));
    TestEqual(TEXT("one asset was exported"),
        static_cast<int32>(NumberField(First, TEXT("exported"))), 1);
    TestEqual(TEXT("nothing failed"), static_cast<int32>(NumberField(First, TEXT("failed"))), 0);
    TestEqual(TEXT("the verification counted the same asset"),
        static_cast<int32>(NumberField(ObjectField(First, TEXT("verification")), TEXT("verified"))), 1);

    const TArray<TSharedPtr<FJsonValue>>* FirstRows = ArrayField(First, TEXT("stems"));
    TestTrue(TEXT("the table carries one row"), FirstRows != nullptr && FirstRows->Num() == 1);
    if (FirstRows && FirstRows->Num() == 1)
    {
        const TSharedPtr<FJsonObject>* Row = nullptr;
        if ((*FirstRows)[0]->TryGetObject(Row) && Row)
        {
            TestTrue(TEXT("the row reports the round trip verified"),
                BoolField(*Row, TEXT("verified")));
            TestEqual(TEXT("the asset landed at the requested path"),
                StringField(*Row, TEXT("assetPath")), ObjectPath);
            // The frame count is read off the DECODE, so it can only be right if the decode ran.
            TestTrue(TEXT("the row publishes the decoded frame count"),
                NumberField(*Row, TEXT("frames"), 0.0) > 0.0);
        }
    }

    // The samples, not a statistic of them. `verified` is computed from a per-channel RMS/PEAK
    // signature (AudioMusicHandler.cpp:1140-1170) - both magnitude-only and order-invariant -
    // so a write that swapped the channels, inverted the polarity or reversed the sample order
    // produces the same signature and reports verified:true. Read the written asset back here
    // and compare it against the stem candidate the export was handed.
    {
        const FPwCandidateLookupResult Stem =
            FPluginState::Get().GetCandidateRegistry().Get(Ids[0]);
        USoundWave* Written = Cast<USoundWave>(
            StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath));
        TestNotNull(TEXT("the exported stem resolves as a USoundWave"), Written);
        TestTrue(TEXT("the stem candidate is still resident to compare against"), Stem.IsHit());
        if (Written && Stem.IsHit())
        {
            const FPwAudioBuffer& Source = Stem.Candidate->Buffer;
            FPwAudioBuffer Decoded;
            FString DecodeError;
            const bool bDecoded = PwDecodeSoundWave(Written, Decoded, DecodeError);
            TestTrue(FString::Printf(TEXT("the exported stem decodes back (error='%s')"),
                *DecodeError), bDecoded);
            if (bDecoded)
            {
                TestEqual(TEXT("the decoded stem keeps the candidate's frame count"),
                    Decoded.NumFrames(), Source.NumFrames());
                TestEqual(TEXT("the decoded stem keeps the candidate's sample rate"),
                    Decoded.SampleRate, Source.SampleRate);

                const int32 ComparedFrames = FMath::Min(Decoded.NumFrames(), Source.NumFrames());
                TestTrue(TEXT("there were samples to compare"), ComparedFrames > 0);

                // 16-bit PCM quantisation is ~3.1e-5, so 1e-3 leaves three decades of headroom:
                // anything above it is a content difference rather than rounding.
                constexpr float SampleTolerance = 1.0e-3f;
                float WorstLeft = 0.0f;
                float WorstRight = 0.0f;
                for (int32 Frame = 0; Frame < ComparedFrames; ++Frame)
                {
                    WorstLeft = FMath::Max(WorstLeft,
                        FMath::Abs(Decoded.Left[Frame] - Source.Left[Frame]));
                    WorstRight = FMath::Max(WorstRight,
                        FMath::Abs(Decoded.Right[Frame] - Source.Right[Frame]));
                }
                TestTrue(FString::Printf(
                    TEXT("every left sample survived the round trip (worst |delta| %g)"),
                    WorstLeft), WorstLeft <= SampleTolerance);
                TestTrue(FString::Printf(
                    TEXT("every right sample survived the round trip (worst |delta| %g)"),
                    WorstRight), WorstRight <= SampleTolerance);
            }
        }
    }

    // §5: save:false must report the triple honestly rather than claiming durability.
    const TSharedPtr<FJsonObject> Save = ObjectField(First, TEXT("save"));
    TestFalse(TEXT("an unsaved export does not claim a save was requested"),
        BoolField(Save, TEXT("saveRequested"), true));
    TestEqual(TEXT("an unsaved export wrote nothing to disk"),
        static_cast<int32>(NumberField(Save, TEXT("saved"))), 0);

    const int32 Chars = MeasureResponseChars(First);
    AddInfo(FString::Printf(TEXT("export_stems x1: %d chars (ceiling %d)"),
        Chars, WrappedResponseCeiling));
    TestTrue(FString::Printf(TEXT("the export result (%d chars) fits the wrapped ceiling (%d)"),
        Chars, WrappedResponseCeiling), Chars <= WrappedResponseCeiling);

    // §8: a retry converges on the same asset rather than making a second one. The measurement a
    // flag cannot fake is updatedInPlace, which comes from AssetCreatePolicy finding the occupant.
    FTestResponseCapture SecondCapture;
    FJobTicket SecondTicket;
    const TSharedPtr<FJsonObject> Second = RunExport(SecondCapture, SecondTicket);
    if (Second.IsValid())
    {
        TestEqual(TEXT("the re-export exported the same one asset"),
            static_cast<int32>(NumberField(Second, TEXT("exported"))), 1);
        TestEqual(TEXT("the re-export updated in place rather than creating a sibling"),
            static_cast<int32>(NumberField(Second, TEXT("updatedInPlace"))), 1);
        TestFalse(TEXT("no _1 sibling was created"),
            AssetExistsInMemory(FString::Printf(TEXT("%s/%s_1.%s_1"),
                TestAssetFolder, *TrackName, *TrackName)));
    }
    else
    {
        AddError(TEXT("the re-export produced no job result"));
    }

    // Reclaim the never-saved asset through GC rather than force-delete, which crashes the suite
    // under -unattended on a never-reloaded asset.
    PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ObjectPath);
    DiscardCandidates(Ids);
    return true;
}

// =================================================================================================
// J. build_interactive - an unresolvable stem path must ERROR AND LEAVE NO ASSET. Asserted against
//    the object table, not against the response: a verb that created the MetaSound first and then
//    discovered the bad path would still answer with an error while leaving a half-built asset.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioMusicBuildInteractiveBadStemLeavesNothingTest,
    "PinWright.audio.music.build_interactive.UnresolvableStemLeavesNothingBehind",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioMusicBuildInteractiveBadStemLeavesNothingTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicHandlerTests;

    const FString AssetName = UniqueAssetName(TEXT("MS_MusicBad_"));
    const FString ObjectPath = FString::Printf(TEXT("%s/%s.%s"),
        TestAssetFolder, *AssetName, *AssetName);

    TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
    Entry->SetStringField(TEXT("assetPath"), TEXT("/Game/PinWrightTests/SW_DoesNotExist_ForSure"));
    Entry->SetStringField(TEXT("inputName"), TEXT("Pad"));
    Entry->SetNumberField(TEXT("loopDurationSeconds"), 4.0);

    TArray<TSharedPtr<FJsonValue>> Stems;
    Stems.Add(MakeShared<FJsonValueObject>(Entry));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), TestAssetFolder);
    Payload->SetStringField(TEXT("name"), AssetName);
    Payload->SetArrayField(TEXT("stems"), Stems);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler found"),
        InvokeHandlerWithCapture(BuildInteractiveMethod, Payload, Capture));
    TestFalse(TEXT("an unresolvable stem path is refused"), Capture.bSuccess);
    TestEqual(TEXT("the refusal names the missing asset"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_ASSET_NOT_FOUND));
    TestTrue(TEXT("the message names the path that did not resolve"),
        Capture.Message.Contains(TEXT("SW_DoesNotExist_ForSure")));

    // The assertion that a response cannot fake.
    TestFalse(TEXT("no MetaSound asset was created"), AssetExistsInMemory(ObjectPath));

    // A missing assetPath is a parameter error, not a resolution one: the two failures call for
    // different fixes and must not answer alike.
    {
        TSharedPtr<FJsonObject> NoPath = MakeShared<FJsonObject>();
        NoPath->SetStringField(TEXT("inputName"), TEXT("Pad"));

        TArray<TSharedPtr<FJsonValue>> NoPathStems;
        NoPathStems.Add(MakeShared<FJsonValueObject>(NoPath));

        TSharedPtr<FJsonObject> NoPathPayload = MakeShared<FJsonObject>();
        NoPathPayload->SetStringField(TEXT("path"), TestAssetFolder);
        NoPathPayload->SetStringField(TEXT("name"), UniqueAssetName(TEXT("MS_MusicNoPath_")));
        NoPathPayload->SetArrayField(TEXT("stems"), NoPathStems);

        FTestResponseCapture NoPathCapture;
        TestTrue(TEXT("handler found"),
            InvokeHandlerWithCapture(BuildInteractiveMethod, NoPathPayload, NoPathCapture));
        TestFalse(TEXT("a stem with no assetPath is refused"), NoPathCapture.bSuccess);
        TestEqual(TEXT("the refusal is a parameter error"),
            NoPathCapture.ErrorCode, FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestTrue(TEXT("the message names the field"),
            NoPathCapture.Message.Contains(TEXT("assetPath")));
    }

    // More stems than the engine's mixer node family can take must be refused BEFORE anything is
    // created, and with the engine's number rather than the score's 16-track cap.
    {
        TArray<TSharedPtr<FJsonValue>> WideStems;
        for (int32 Index = 0; Index <= PwMusicGraphMaxStems; ++Index)
        {
            TSharedPtr<FJsonObject> Wide = MakeShared<FJsonObject>();
            Wide->SetStringField(TEXT("assetPath"),
                FString::Printf(TEXT("/Game/PinWrightTests/SW_Wide_%d"), Index));
            WideStems.Add(MakeShared<FJsonValueObject>(Wide));
        }
        TestTrue(TEXT("the fixture is genuinely over the cap"),
            WideStems.Num() > PwMusicGraphMaxStems);

        const FString WideName = UniqueAssetName(TEXT("MS_MusicWide_"));
        TSharedPtr<FJsonObject> WidePayload = MakeShared<FJsonObject>();
        WidePayload->SetStringField(TEXT("path"), TestAssetFolder);
        WidePayload->SetStringField(TEXT("name"), WideName);
        WidePayload->SetArrayField(TEXT("stems"), WideStems);

        FTestResponseCapture WideCapture;
        TestTrue(TEXT("handler found"),
            InvokeHandlerWithCapture(BuildInteractiveMethod, WidePayload, WideCapture));
        TestFalse(TEXT("more stems than the mixer supports is refused"), WideCapture.bSuccess);
        TestEqual(TEXT("the refusal is an argument error"),
            WideCapture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
        TestFalse(TEXT("nothing was created for an over-wide graph"),
            AssetExistsInMemory(FString::Printf(TEXT("%s/%s.%s"),
                TestAssetFolder, *WideName, *WideName)));
    }

    // An empty layer list must not build an empty graph.
    {
        TSharedPtr<FJsonObject> EmptyPayload = MakeShared<FJsonObject>();
        EmptyPayload->SetStringField(TEXT("path"), TestAssetFolder);
        EmptyPayload->SetStringField(TEXT("name"), UniqueAssetName(TEXT("MS_MusicEmpty_")));
        EmptyPayload->SetArrayField(TEXT("stems"), TArray<TSharedPtr<FJsonValue>>());

        FTestResponseCapture EmptyCapture;
        TestTrue(TEXT("handler found"),
            InvokeHandlerWithCapture(BuildInteractiveMethod, EmptyPayload, EmptyCapture));
        TestFalse(TEXT("an empty layer list is refused"), EmptyCapture.bSuccess);
        TestEqual(TEXT("the refusal is an argument error"),
            EmptyCapture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    }

    return true;
}

// =================================================================================================
// K. §10 - the two verbs that create assets and write packages must be gated, and the two that do
//    not must not be. The table is documentation as much as it is a gate, so both halves are
//    asserted through the shared accessor rather than re-implemented here.
// =================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioMusicSafePointRegistrationTest,
    "PinWright.audio.music.SafePoint.AssetWritingVerbsAreGated",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioMusicSafePointRegistrationTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicHandlerTests;

    const TArray<const TCHAR*> Gated = { ExportStemsMethod, BuildInteractiveMethod };
    for (const TCHAR* Method : Gated)
    {
        TestTrue(FString::Printf(TEXT("%s is in the tick-unsafe method table"), Method),
            PinWrightSafePoint::IsTickUnsafeMethod(FString(Method)));
        // Spelled the same as the registered handler: a typo in the table fails silently, which is
        // exactly the failure this second assertion exists to catch.
        TestTrue(FString::Printf(TEXT("the %s table entry matches the registered name"), Method),
            PinWrightSafePoint::GetTickUnsafeMethods().Contains(FString(Method)));
    }

    // Over-deferring is cheap but the table is also documentation: these two touch no package, no
    // asset and no blocking payload read, so listing them would state a hazard that is not there.
    const TArray<const TCHAR*> Ungated = { RenderStemsMethod, DescribeSchemaMethod };
    for (const TCHAR* Method : Ungated)
    {
        TestFalse(FString::Printf(TEXT("%s is not gated - it creates no asset and saves nothing"),
            Method), PinWrightSafePoint::IsTickUnsafeMethod(FString(Method)));
    }

    return true;
}
