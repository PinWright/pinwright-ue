// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the music-score contract (AudioGen/PwMusicScore.h).
//
// Weighted toward the FAILURE direction on purpose (rpc-design §12). A score parser that
// succeeds is easy; the contract that matters is that an unrecognised scale, role or
// generation mode ERRORS naming the valid set, that a cap ERRORS instead of clamping, and
// that the caller's score is left exactly as it was. Every negative test below asserts both
// halves - the error code AND the field path AND that the out-parameter still holds its
// sentinel. Revert any of those rejections to a silent fallback and these break.
//
// The loop tests carry the same intent one level down: the honest case is the one where the
// loop does NOT land on a whole frame, and the assertion is that the function says so rather
// than rounding.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "AudioGen/PwMusicScore.h"
#include "Handlers/ErrorCodes.h"

#include "DSP/MidiNoteQuantizer.h"

// Named namespace, not anonymous: Unity merges translation units and an anonymous namespace
// here would still collide with a sibling test file's identically-named helper.
namespace PwMusicScoreTests
{
    TSharedPtr<FJsonObject> ParseJson(const FString& Text)
    {
        TSharedPtr<FJsonObject> Object;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
        FJsonSerializer::Deserialize(Reader, Object);
        return Object;
    }

    FString ToJson(const TSharedPtr<FJsonObject>& Object)
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

    // Deliberately unlike anything any fixture parses to, so a sentinel left in an
    // out-parameter is unmistakable.
    FPwMusicScore Sentinel()
    {
        FPwMusicScore Score;
        Score.Seed = 424242;
        Score.SampleRate = 22050;
        Score.Bpm = 77.0;
        Score.Bars = 3;
        Score.TimeSignature.Numerator = 7;
        Score.TimeSignature.Denominator = 8;
        Score.Key.Root = EPwMusicPitchClass::FSharp;
        Score.Key.Scale = EPwMusicScale::Blues;

        FPwMusicTrack Track;
        Track.Name = TEXT("sentinel");
        Track.Role = EPwMusicTrackRole::Percussion;
        Track.Octave = 7;
        Track.GainDb = -42.0;
        Score.Tracks.Add(Track);
        return Score;
    }

    // The "leaves Out untouched" half of rpc-design §1: a partially parsed score must never
    // be observable.
    void ExpectUntouched(FAutomationTestBase& Test, const TCHAR* Label, const FPwMusicScore& Out)
    {
        Test.TestEqual(FString::Printf(TEXT("%s: Seed untouched"), Label), Out.Seed, 424242);
        Test.TestEqual(FString::Printf(TEXT("%s: SampleRate untouched"), Label), Out.SampleRate, 22050);
        Test.TestEqual(FString::Printf(TEXT("%s: Bpm untouched"), Label), Out.Bpm, 77.0);
        Test.TestEqual(FString::Printf(TEXT("%s: Bars untouched"), Label), Out.Bars, 3);
        Test.TestEqual(FString::Printf(TEXT("%s: numerator untouched"), Label), Out.TimeSignature.Numerator, 7);
        Test.TestTrue(FString::Printf(TEXT("%s: scale untouched"), Label), Out.Key.Scale == EPwMusicScale::Blues);
        Test.TestEqual(FString::Printf(TEXT("%s: track count untouched"), Label), Out.Tracks.Num(), 1);
        if (Out.Tracks.Num() == 1)
        {
            Test.TestEqual(FString::Printf(TEXT("%s: track name untouched"), Label), Out.Tracks[0].Name, FString(TEXT("sentinel")));
            Test.TestEqual(FString::Printf(TEXT("%s: track octave untouched"), Label), Out.Tracks[0].Octave, 7);
        }
    }

    void ExpectFailure(FAutomationTestBase& Test, const TCHAR* Label, const FString& Json,
        const TCHAR* ExpectedCode, const TCHAR* ExpectedField)
    {
        TSharedPtr<FJsonObject> Object = ParseJson(Json);
        Test.TestTrue(FString::Printf(TEXT("%s: fixture is valid JSON"), Label), Object.IsValid());
        if (!Object.IsValid())
        {
            return;
        }

        FPwMusicScore Out = Sentinel();
        FPwMusicScoreError Error;
        const bool bParsed = ParseMusicScore(Object, Out, Error);

        Test.TestFalse(FString::Printf(TEXT("%s: parse reports failure"), Label), bParsed);
        Test.TestEqual(FString::Printf(TEXT("%s: error code"), Label), Error.Code, FString(ExpectedCode));
        Test.TestEqual(FString::Printf(TEXT("%s: error field path"), Label), Error.Field, FString(ExpectedField));
        Test.TestFalse(FString::Printf(TEXT("%s: error carries a message"), Label), Error.Message.IsEmpty());
        Test.TestTrue(FString::Printf(TEXT("%s: ToString names the field"), Label),
            Error.ToString().Contains(ExpectedField));
        ExpectUntouched(Test, Label, Out);
    }

    FString MinimalInstrument()
    {
        return TEXT(R"({"generator":{"kind":"osc","params":{"waveform":"sine","frequencyHz":440}}})");
    }

    // Smallest score the schema accepts.
    FString MinimalScore()
    {
        return FString::Printf(TEXT(R"({"bpm":120,"bars":4,"timeSignature":{"numerator":4,"denominator":4},)")
            TEXT(R"("key":{"root":"a","scale":"natural_minor_aeolian"},)")
            TEXT(R"("tracks":[{"name":"bed","role":"drone","octave":2,"instrument":%s,)")
            TEXT(R"("generation":{"mode":"drone","params":{"degrees":[0,4]}}}]})"),
            *MinimalInstrument());
    }

    // MinimalScore with one track field replaced / added. Keeps the negative fixtures short
    // enough to read the thing under test.
    FString ScoreWithTrack(const FString& TrackBody)
    {
        return FString::Printf(TEXT(R"({"bpm":120,"bars":4,"timeSignature":{"numerator":4,"denominator":4},)")
            TEXT(R"("key":{"root":"a","scale":"natural_minor_aeolian"},"tracks":[%s]})"),
            *TrackBody);
    }

    FString DroneTrack(const TCHAR* Name)
    {
        return FString::Printf(TEXT(R"({"name":"%s","role":"drone","octave":2,"instrument":%s,)")
            TEXT(R"("generation":{"mode":"drone","params":{"degrees":[0]}}})"),
            Name, *MinimalInstrument());
    }

    FString RepeatedTracks(int32 Count)
    {
        TArray<FString> Tracks;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Tracks.Add(DroneTrack(*FString::Printf(TEXT("bed_%d"), Index)));
        }
        return ScoreWithTrack(FString::Join(Tracks, TEXT(",")));
    }

    FString RepeatedNotes(int32 Count)
    {
        TArray<FString> Notes;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Notes.Add(TEXT(R"({"startBeat":0,"durationBeats":1,"degree":0})"));
        }
        return FString::Printf(TEXT(R"({"name":"lead","role":"lead","octave":4,"instrument":%s,)")
            TEXT(R"("generation":{"mode":"explicit"},"notes":[%s]})"),
            *MinimalInstrument(), *FString::Join(Notes, TEXT(",")));
    }

    FString RepeatedSections(int32 Count)
    {
        TArray<FString> Sections;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Sections.Add(FString::Printf(TEXT(R"({"name":"s%d","startBar":%d,"bars":1})"), Index, Index));
        }
        return FString::Printf(TEXT(R"({"bpm":120,"bars":%d,"timeSignature":{"numerator":4,"denominator":4},)")
            TEXT(R"("key":{"root":"a","scale":"natural_minor_aeolian"},"sections":[%s],"tracks":[%s]})"),
            Count, *FString::Join(Sections, TEXT(",")), *DroneTrack(TEXT("bed")));
    }

    // Everything the schema can express: sections, all three note-source shapes, an explicit
    // track, a drone, an evolving pad, a full instrument with both envelopes, modulation and
    // an fx chain.
    const TCHAR* const RichScore = TEXT(R"JSON(
{
  "seed": 11,
  "sampleRate": 44100,
  "bpm": 84,
  "bars": 8,
  "timeSignature": { "numerator": 6, "denominator": 8 },
  "key": { "root": "d", "scale": "phrygian" },
  "sections": [
    { "name": "intro", "startBar": 0, "bars": 4, "intensityFloor": 0.0, "intensityCeil": 0.4 },
    { "name": "swell", "startBar": 4, "bars": 4, "intensityFloor": 0.3, "intensityCeil": 1.0 }
  ],
  "tracks": [
    {
      "name": "sub_drone",
      "role": "drone",
      "octave": 2,
      "gainDb": -9,
      "pan": 0,
      "instrument": {
        "generator": { "kind": "osc", "params": { "waveform": "sine", "frequencyHz": 55 } },
        "ampEnvelope": [
          { "timeMs": 0, "value": 0, "curve": "exp" },
          { "timeMs": 3000, "value": 1 }
        ]
      },
      "generation": { "mode": "drone", "params": { "degrees": [0, 4], "velocity": 0.7 } }
    },
    {
      "name": "pad",
      "role": "pad",
      "octave": 4,
      "gainDb": -6,
      "pan": 0.25,
      "instrument": {
        "generator": { "kind": "osc", "params": { "waveform": "triangle", "frequencyHz": 330, "unison": 3 } },
        "pitchEnvelope": [ { "timeMs": 0, "semitones": -0.15 }, { "timeMs": 4000, "semitones": 0.15 } ],
        "modulation": { "am": { "depth": 0.2, "rateHz": 0.12 } },
        "fx": [ { "kind": "filter", "params": { "type": "lowpass", "cutoffHz": 1400, "resonance": 0.9 } } ]
      },
      "generation": {
        "mode": "evolving_pad",
        "params": { "degrees": [0, 2, 4, 6], "swellBeatsMin": 6, "swellBeatsMax": 12, "overlap": 0.6, "octaveSpread": 1 }
      }
    },
    {
      "name": "glints",
      "role": "texture",
      "octave": 5,
      "instrument": { "generator": { "kind": "noise", "params": { "color": "pink" } } },
      "generation": {
        "mode": "sparse_events",
        "params": { "degrees": [0, 3, 5], "eventsPerBar": 0.75, "durationBeatsMin": 1, "durationBeatsMax": 3 }
      }
    },
    {
      "name": "motif",
      "role": "lead",
      "octave": 4,
      "instrument": { "generator": { "kind": "osc", "params": { "waveform": "saw", "frequencyHz": 440 } } },
      "generation": { "mode": "explicit" },
      "notes": [
        { "startBeat": 0, "durationBeats": 3, "degree": 0, "velocity": 0.8 },
        { "startBeat": 6, "durationBeats": 2, "degree": 4 },
        { "startBeat": 12, "durationBeats": 6, "midi": 74, "velocity": 0.45 }
      ]
    }
  ]
}
)JSON");

    FPwMusicScore ScoreForTiming(double Bpm, int32 Numerator, int32 Denominator, int32 Bars, int32 SampleRate)
    {
        FPwMusicScore Score;
        Score.Bpm = Bpm;
        Score.TimeSignature.Numerator = Numerator;
        Score.TimeSignature.Denominator = Denominator;
        Score.Bars = Bars;
        Score.SampleRate = SampleRate;
        return Score;
    }

    // A dump that is byte-identical for identical note sets and differs the moment any field
    // does. Cheaper to read in a failure report than a per-field comparison loop.
    FString NotesToString(const TArray<FPwMusicNote>& Notes)
    {
        FString Text;
        for (const FPwMusicNote& Note : Notes)
        {
            Text += FString::Printf(TEXT("%.9f|%.9f|%d|%d|%.9f;"),
                Note.StartBeat, Note.DurationBeats,
                Note.Degree.IsSet() ? Note.Degree.GetValue() : -999,
                Note.Midi.IsSet() ? Note.Midi.GetValue() : -999,
                Note.Velocity);
        }
        return Text;
    }

    // A four-track score whose seed the caller chooses, used by the determinism tests.
    FString SeededScore(int32 Seed)
    {
        return FString::Printf(TEXT(R"({"seed":%d,"bpm":90,"bars":8,)")
            TEXT(R"("timeSignature":{"numerator":4,"denominator":4},)")
            TEXT(R"("key":{"root":"c","scale":"major_pentatonic"},"tracks":[)")
            TEXT(R"({"name":"bed","role":"drone","octave":3,"instrument":%s,)")
            TEXT(R"("generation":{"mode":"drone","params":{"degrees":[0,2]}}},)")
            TEXT(R"({"name":"pad","role":"pad","octave":4,"instrument":%s,)")
            TEXT(R"("generation":{"mode":"evolving_pad","params":{"degrees":[0,1,2,3],"swellBeatsMin":3,"swellBeatsMax":7,"octaveSpread":1}}},)")
            TEXT(R"({"name":"glints","role":"texture","octave":5,"instrument":%s,)")
            TEXT(R"("generation":{"mode":"sparse_events","params":{"degrees":[0,2,4],"eventsPerBar":2,"durationBeatsMin":0.5,"durationBeatsMax":2}}})")
            TEXT(R"(]})"),
            Seed, *MinimalInstrument(), *MinimalInstrument(), *MinimalInstrument());
    }
}

// =========================================================================
// A. Round trip. Serialize(Parse(x)) must survive a second pass byte-for-byte, and the
//    authored values must survive both.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwMusicScoreRoundTripTest,
    "PinWright.audio.music.score.RoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwMusicScoreRoundTripTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicScoreTests;

    TSharedPtr<FJsonObject> Authored = ParseJson(RichScore);
    TestTrue(TEXT("fixture is valid JSON"), Authored.IsValid());
    if (!Authored.IsValid())
    {
        return false;
    }

    FPwMusicScore First;
    FPwMusicScoreError Error;
    const bool bFirst = ParseMusicScore(Authored, First, Error);
    TestTrue(FString::Printf(TEXT("rich fixture parses (%s)"), *Error.ToString()), bFirst);
    if (!bFirst)
    {
        return false;
    }

    TestEqual(TEXT("version defaults to the current revision"), First.Version, PwMusicLimits::ScoreVersion);
    TestEqual(TEXT("seed"), First.Seed, 11);
    TestEqual(TEXT("sampleRate"), First.SampleRate, 44100);
    TestEqual(TEXT("bpm"), First.Bpm, 84.0);
    TestEqual(TEXT("bars"), First.Bars, 8);
    TestEqual(TEXT("numerator"), First.TimeSignature.Numerator, 6);
    TestEqual(TEXT("denominator"), First.TimeSignature.Denominator, 8);
    TestTrue(TEXT("key root"), First.Key.Root == EPwMusicPitchClass::D);
    TestTrue(TEXT("key scale"), First.Key.Scale == EPwMusicScale::Phrygian);

    TestEqual(TEXT("section count"), First.Sections.Num(), 2);
    if (First.Sections.Num() == 2)
    {
        TestEqual(TEXT("section 0 name"), First.Sections[0].Name, FString(TEXT("intro")));
        TestEqual(TEXT("section 1 startBar"), First.Sections[1].StartBar, 4);
        TestEqual(TEXT("section 1 intensityCeil"), First.Sections[1].IntensityCeil, 1.0);
    }

    TestEqual(TEXT("track count"), First.Tracks.Num(), 4);
    if (First.Tracks.Num() != 4)
    {
        return false;
    }

    const FPwMusicTrack& Drone = First.Tracks[0];
    TestTrue(TEXT("track 0 role"), Drone.Role == EPwMusicTrackRole::Drone);
    TestTrue(TEXT("track 0 generation mode"), Drone.Generation.Mode == EPwMusicGenerationMode::Drone);
    TestEqual(TEXT("track 0 octave"), Drone.Octave, 2);
    TestEqual(TEXT("track 0 pan"), Drone.Pan, 0.0);
    // The instrument really is a synth layer, parsed by the SFX parser.
    TestTrue(TEXT("track 0 instrument generator is osc"), Drone.Instrument.Generator.Kind == EPwSynthGeneratorKind::Osc);
    TestEqual(TEXT("track 0 instrument amp envelope points"), Drone.Instrument.AmpEnvelope.Num(), 2);
    // A documented default materialized by the shared bag parser, which is what makes
    // serialize lossless on this side too.
    TestEqual(TEXT("track 0 instrument pulseWidth materialized"),
        Drone.Instrument.Generator.Params.GetNumber(FName(TEXT("pulseWidth"))), 0.5);
    TestEqual(TEXT("track 0 drone velocity"), Drone.Generation.Params.GetNumber(FName(TEXT("velocity"))), 0.7);

    const FPwMusicTrack& Pad = First.Tracks[1];
    TestTrue(TEXT("track 1 generation mode"), Pad.Generation.Mode == EPwMusicGenerationMode::EvolvingPad);
    TestEqual(TEXT("track 1 instrument fx count"), Pad.Instrument.Fx.Num(), 1);
    TestTrue(TEXT("track 1 instrument modulation routes am"),
        Pad.Instrument.Modulation.Routing == EPwSynthModulationRouting::Am);
    TestEqual(TEXT("track 1 instrument pitch envelope points"), Pad.Instrument.PitchEnvelope.Num(), 2);
    TestEqual(TEXT("track 1 overlap"), Pad.Generation.Params.GetNumber(FName(TEXT("overlap"))), 0.6);
    // An optional generation parameter the fixture never mentioned.
    TestEqual(TEXT("track 1 velocityMin materialized from its default"),
        Pad.Generation.Params.GetNumber(FName(TEXT("velocityMin"))), 0.35);
    const TArray<double>* PadDegrees = Pad.Generation.Params.GetNumbers(FName(TEXT("degrees")));
    TestNotNull(TEXT("track 1 degrees present"), PadDegrees);
    if (PadDegrees)
    {
        TestEqual(TEXT("track 1 degree count"), PadDegrees->Num(), 4);
    }

    const FPwMusicTrack& Motif = First.Tracks[3];
    TestTrue(TEXT("track 3 generation mode"), Motif.Generation.Mode == EPwMusicGenerationMode::Explicit);
    TestEqual(TEXT("track 3 note count"), Motif.Notes.Num(), 3);
    if (Motif.Notes.Num() == 3)
    {
        TestTrue(TEXT("note 0 carries a degree"), Motif.Notes[0].Degree.IsSet());
        TestFalse(TEXT("note 0 carries no midi"), Motif.Notes[0].Midi.IsSet());
        TestEqual(TEXT("note 1 velocity defaults to unity"), Motif.Notes[1].Velocity, PwMusicLimits::DefaultVelocity);
        TestTrue(TEXT("note 2 carries a midi"), Motif.Notes[2].Midi.IsSet());
        if (Motif.Notes[2].Midi.IsSet())
        {
            TestEqual(TEXT("note 2 midi"), Motif.Notes[2].Midi.GetValue(), 74);
        }
    }

    // The hard requirement.
    const FString FirstJson = ToJson(SerializeMusicScore(First));
    TestFalse(TEXT("serialize produced output"), FirstJson.IsEmpty());

    TSharedPtr<FJsonObject> Reparsed = ParseJson(FirstJson);
    TestTrue(TEXT("serialized output is valid JSON"), Reparsed.IsValid());
    if (!Reparsed.IsValid())
    {
        return false;
    }

    FPwMusicScore Second;
    FPwMusicScoreError SecondError;
    const bool bSecond = ParseMusicScore(Reparsed, Second, SecondError);
    TestTrue(FString::Printf(TEXT("serialized output re-parses (%s)"), *SecondError.ToString()), bSecond);
    if (!bSecond)
    {
        return false;
    }

    const FString SecondJson = ToJson(SerializeMusicScore(Second));
    TestEqual(TEXT("Serialize(Parse(Serialize(Parse(x)))) is byte-identical"), SecondJson, FirstJson);

    // A serializer that dropped a field would still be self-consistent, so spot-check that
    // the second pass kept the AUTHORED values.
    TestEqual(TEXT("re-parsed bpm"), Second.Bpm, 84.0);
    TestEqual(TEXT("re-parsed track count"), Second.Tracks.Num(), 4);
    TestEqual(TEXT("re-parsed section count"), Second.Sections.Num(), 2);
    if (Second.Tracks.Num() == 4)
    {
        TestEqual(TEXT("re-parsed pad overlap"),
            Second.Tracks[1].Generation.Params.GetNumber(FName(TEXT("overlap"))), 0.6);
        TestEqual(TEXT("re-parsed motif note count"), Second.Tracks[3].Notes.Num(), 3);
        TestEqual(TEXT("re-parsed pad instrument fx count"), Second.Tracks[1].Instrument.Fx.Num(), 1);
    }

    // A generative track must not gain a notes[] array on the way out - that array would not
    // re-parse, which is the round-trip half of the rule that rejects it on the way in.
    TSharedPtr<FJsonObject> Serialized = SerializeMusicScore(First);
    const TArray<TSharedPtr<FJsonValue>>* SerializedTracks = nullptr;
    TestTrue(TEXT("serialized score carries tracks"),
        Serialized.IsValid() && Serialized->TryGetArrayField(TEXT("tracks"), SerializedTracks));
    if (SerializedTracks && SerializedTracks->Num() == 4)
    {
        TestFalse(TEXT("generative track emits no notes[]"),
            (*SerializedTracks)[0]->AsObject()->HasField(TEXT("notes")));
        TestTrue(TEXT("explicit track emits notes[]"),
            (*SerializedTracks)[3]->AsObject()->HasField(TEXT("notes")));
        // The three layer fields the score owns never leak back into the instrument.
        TSharedPtr<FJsonObject> Instrument = (*SerializedTracks)[0]->AsObject()->GetObjectField(TEXT("instrument"));
        TestTrue(TEXT("serialized instrument exists"), Instrument.IsValid());
        if (Instrument.IsValid())
        {
            TestFalse(TEXT("instrument emits no startMs"), Instrument->HasField(TEXT("startMs")));
            TestFalse(TEXT("instrument emits no gainDb"), Instrument->HasField(TEXT("gainDb")));
            TestFalse(TEXT("instrument emits no pan"), Instrument->HasField(TEXT("pan")));
            TestTrue(TEXT("instrument emits its generator"), Instrument->HasField(TEXT("generator")));
        }
    }

    return true;
}

// =========================================================================
// B. An unrecognised scale ERRORS. It must never degrade into major - a silently major
//    score is a bug the caller would chase by ear.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwMusicScoreUnknownScaleTest,
    "PinWright.audio.music.score.UnknownScale",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwMusicScoreUnknownScaleTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicScoreTests;

    const FString Json = MinimalScore().Replace(TEXT("natural_minor_aeolian"), TEXT("hungarian_minor"));
    ExpectFailure(*this, TEXT("unknown scale"), Json, ErrorCodes::ERR_INVALID_PARAMS, TEXT("key.scale"));

    // The message must name the valid set, because that is what the caller patches with.
    TSharedPtr<FJsonObject> Object = ParseJson(Json);
    FPwMusicScore Out = Sentinel();
    FPwMusicScoreError Error;
    TestFalse(TEXT("unknown scale fails"), ParseMusicScore(Object, Out, Error));
    TestTrue(TEXT("message names an accepted scale"), Error.Message.Contains(TEXT("natural_minor_aeolian")));
    TestTrue(TEXT("message names the offending token"), Error.Message.Contains(TEXT("hungarian_minor")));

    // A missing scale is a MISSING-field failure, not an unknown one, and still must not
    // fall back.
    ExpectFailure(*this, TEXT("missing scale"),
        MinimalScore().Replace(TEXT(R"("scale":"natural_minor_aeolian")"), TEXT(R"("scale":null)")),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("key.scale"));

    // An unrecognised root is the same class of failure and must not become C.
    ExpectFailure(*this, TEXT("unknown root"),
        MinimalScore().Replace(TEXT(R"("root":"a")"), TEXT(R"("root":"h")")),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("key.root"));

    // Flat spellings are deliberately NOT aliases: canonicalizing them would break the
    // byte-stable round trip.
    ExpectFailure(*this, TEXT("flat spelling"),
        MinimalScore().Replace(TEXT(R"("root":"a")"), TEXT(R"("root":"bb")")),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("key.root"));

    return true;
}

// =========================================================================
// C. An unrecognised track role ERRORS rather than becoming "lead".
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwMusicScoreUnknownRoleTest,
    "PinWright.audio.music.score.UnknownRole",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwMusicScoreUnknownRoleTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicScoreTests;

    ExpectFailure(*this, TEXT("unknown role"),
        MinimalScore().Replace(TEXT(R"("role":"drone")"), TEXT(R"("role":"ambience")")),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("tracks[0].role"));

    ExpectFailure(*this, TEXT("missing role"),
        MinimalScore().Replace(TEXT(R"("role":"drone",)"), TEXT("")),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("tracks[0].role"));

    TSharedPtr<FJsonObject> Object = ParseJson(
        MinimalScore().Replace(TEXT(R"("role":"drone")"), TEXT(R"("role":"ambience")")));
    FPwMusicScore Out = Sentinel();
    FPwMusicScoreError Error;
    TestFalse(TEXT("unknown role fails"), ParseMusicScore(Object, Out, Error));
    TestTrue(TEXT("message names the accepted roles"), Error.Message.Contains(TEXT("percussion")));

    return true;
}

// =========================================================================
// D. An unrecognised generation mode ERRORS rather than becoming "explicit". A track that
//    silently became explicit would fall silent, and the caller would blame the instrument.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwMusicScoreUnknownGenerationModeTest,
    "PinWright.audio.music.score.UnknownGenerationMode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwMusicScoreUnknownGenerationModeTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicScoreTests;

    ExpectFailure(*this, TEXT("unknown generation mode"),
        MinimalScore().Replace(TEXT(R"("mode":"drone")"), TEXT(R"("mode":"perlin")")),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("tracks[0].generation.mode"));

    ExpectFailure(*this, TEXT("missing generation block"),
        ScoreWithTrack(FString::Printf(
            TEXT(R"({"name":"bed","role":"drone","octave":2,"instrument":%s})"), *MinimalInstrument())),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("tracks[0].generation"));

    TSharedPtr<FJsonObject> Object = ParseJson(
        MinimalScore().Replace(TEXT(R"("mode":"drone")"), TEXT(R"("mode":"perlin")")));
    FPwMusicScore Out = Sentinel();
    FPwMusicScoreError Error;
    TestFalse(TEXT("unknown mode fails"), ParseMusicScore(Object, Out, Error));
    TestTrue(TEXT("message names the accepted modes"), Error.Message.Contains(TEXT("evolving_pad")));
    TestTrue(TEXT("message names explicit"), Error.Message.Contains(TEXT("explicit")));

    // A parameter that is not in the mode's table is a shape error, not a silently dropped
    // setting.
    ExpectFailure(*this, TEXT("unknown generation parameter"),
        MinimalScore().Replace(TEXT(R"("degrees":[0,4])"), TEXT(R"("degrees":[0,4],"wobble":1)")),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("tracks[0].generation.params.wobble"));

    // `explicit` takes no parameters at all, and saying otherwise is a mistake worth naming.
    ExpectFailure(*this, TEXT("explicit takes no parameters"),
        ScoreWithTrack(FString::Printf(
            TEXT(R"({"name":"lead","role":"lead","octave":4,"instrument":%s,)")
            TEXT(R"("generation":{"mode":"explicit","params":{"degrees":[0]}},)")
            TEXT(R"("notes":[{"startBeat":0,"durationBeats":1,"degree":0}]})"), *MinimalInstrument())),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("tracks[0].generation.params.degrees"));

    return true;
}

// =========================================================================
// E. A missing required field ERRORS rather than defaulting. Each of these is a value with
//    no reading that is right in the field.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwMusicScoreMissingRequiredFieldTest,
    "PinWright.audio.music.score.MissingRequiredField",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwMusicScoreMissingRequiredFieldTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicScoreTests;

    ExpectFailure(*this, TEXT("missing bpm"),
        MinimalScore().Replace(TEXT(R"("bpm":120,)"), TEXT("")),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("bpm"));

    ExpectFailure(*this, TEXT("missing bars"),
        MinimalScore().Replace(TEXT(R"("bars":4,)"), TEXT("")),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("bars"));

    ExpectFailure(*this, TEXT("missing time signature"),
        MinimalScore().Replace(TEXT(R"("timeSignature":{"numerator":4,"denominator":4},)"), TEXT("")),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("timeSignature"));

    ExpectFailure(*this, TEXT("missing key"),
        MinimalScore().Replace(TEXT(R"("key":{"root":"a","scale":"natural_minor_aeolian"},)"), TEXT("")),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("key"));

    // Register has no safe default: the same score at octave 2 and octave 6 is a different
    // piece of music.
    ExpectFailure(*this, TEXT("missing track octave"),
        MinimalScore().Replace(TEXT(R"("octave":2,)"), TEXT("")),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("tracks[0].octave"));

    ExpectFailure(*this, TEXT("missing track name"),
        MinimalScore().Replace(TEXT(R"("name":"bed",)"), TEXT("")),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("tracks[0].name"));

    ExpectFailure(*this, TEXT("missing instrument"),
        ScoreWithTrack(TEXT(R"({"name":"bed","role":"drone","octave":2,)")
            TEXT(R"("generation":{"mode":"drone","params":{"degrees":[0]}}})")),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("tracks[0].instrument"));

    // A required GENERATION parameter, checked by the shared bag parser.
    ExpectFailure(*this, TEXT("missing drone degrees"),
        MinimalScore().Replace(TEXT(R"("params":{"degrees":[0,4]})"), TEXT(R"("params":{})")),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("tracks[0].generation.params.degrees"));

    // A required INSTRUMENT parameter, checked by the SFX parser and re-rooted onto the
    // instrument path.
    ExpectFailure(*this, TEXT("missing instrument generator parameter"),
        MinimalScore().Replace(TEXT(R"("params":{"waveform":"sine","frequencyHz":440})"),
            TEXT(R"("params":{"waveform":"sine"})")),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("tracks[0].instrument.generator.params.frequencyHz"));

    // An explicit track without notes is not an empty track, it is an unfinished one.
    ExpectFailure(*this, TEXT("explicit track with no notes"),
        ScoreWithTrack(FString::Printf(
            TEXT(R"({"name":"lead","role":"lead","octave":4,"instrument":%s,"generation":{"mode":"explicit"}})"),
            *MinimalInstrument())),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("tracks[0].notes"));

    // A note needs exactly one pitch source. Neither, and both, are errors.
    ExpectFailure(*this, TEXT("note with no pitch"),
        ScoreWithTrack(FString::Printf(
            TEXT(R"({"name":"lead","role":"lead","octave":4,"instrument":%s,"generation":{"mode":"explicit"},)")
            TEXT(R"("notes":[{"startBeat":0,"durationBeats":1}]})"), *MinimalInstrument())),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("tracks[0].notes[0]"));

    ExpectFailure(*this, TEXT("note with two pitches"),
        ScoreWithTrack(FString::Printf(
            TEXT(R"({"name":"lead","role":"lead","octave":4,"instrument":%s,"generation":{"mode":"explicit"},)")
            TEXT(R"("notes":[{"startBeat":0,"durationBeats":1,"degree":0,"midi":60}]})"), *MinimalInstrument())),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("tracks[0].notes[0]"));

    return true;
}

// =========================================================================
// F. Caps are validation errors, not clamps. A silently truncated score does not
//    round-trip, and every dropped track is a stem the caller believes exists.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwMusicScoreCapsAreErrorsTest,
    "PinWright.audio.music.score.CapsAreErrors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwMusicScoreCapsAreErrorsTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicScoreTests;

    // At the cap: accepted.
    {
        TSharedPtr<FJsonObject> Object = ParseJson(RepeatedTracks(PwMusicLimits::MaxTracks));
        FPwMusicScore Out;
        FPwMusicScoreError Error;
        const bool bParsed = ParseMusicScore(Object, Out, Error);
        TestTrue(FString::Printf(TEXT("%d tracks parse (%s)"), PwMusicLimits::MaxTracks, *Error.ToString()), bParsed);
        TestEqual(TEXT("all tracks survive"), Out.Tracks.Num(), PwMusicLimits::MaxTracks);
    }

    // One past the cap: rejected, NOT truncated.
    ExpectFailure(*this, TEXT("track cap"), RepeatedTracks(PwMusicLimits::MaxTracks + 1),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("tracks"));

    ExpectFailure(*this, TEXT("section cap"), RepeatedSections(PwMusicLimits::MaxSections + 1),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("sections"));

    ExpectFailure(*this, TEXT("note cap"),
        ScoreWithTrack(RepeatedNotes(PwMusicLimits::MaxNotesPerTrack + 1)),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("tracks[0].notes"));

    ExpectFailure(*this, TEXT("bar cap"),
        MinimalScore().Replace(TEXT(R"("bars":4)"), *FString::Printf(TEXT(R"("bars":%d)"), PwMusicLimits::MaxBars + 1)),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("bars"));

    ExpectFailure(*this, TEXT("zero bars"),
        MinimalScore().Replace(TEXT(R"("bars":4)"), TEXT(R"("bars":0)")),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("bars"));

    ExpectFailure(*this, TEXT("no tracks"),
        TEXT(R"({"bpm":120,"bars":4,"timeSignature":{"numerator":4,"denominator":4},)")
        TEXT(R"("key":{"root":"a","scale":"natural_minor_aeolian"},"tracks":[]})"),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("tracks"));

    // The generative-note cap is enforced at materialization too, because a hand-built score
    // never passed through the parser.
    {
        TSharedPtr<FJsonObject> Object = ParseJson(
            TEXT(R"({"bpm":240,"bars":256,"timeSignature":{"numerator":16,"denominator":16},)")
            TEXT(R"("key":{"root":"c","scale":"chromatic"},"tracks":[)")
            TEXT(R"({"name":"storm","role":"texture","octave":4,)")
            TEXT(R"("instrument":{"generator":{"kind":"osc","params":{"waveform":"sine","frequencyHz":440}}},)")
            TEXT(R"("generation":{"mode":"sparse_events","params":{"degrees":[0],"eventsPerBar":16,)")
            TEXT(R"("durationBeatsMin":0.125,"durationBeatsMax":0.25}}}]})"));
        FPwMusicScore Score;
        FPwMusicScoreError Error;
        const bool bDenseParsed = ParseMusicScore(Object, Score, Error);
        TestTrue(FString::Printf(TEXT("dense score parses (%s)"), *Error.ToString()), bDenseParsed);
        if (!bDenseParsed)
        {
            return false;
        }

        TArray<FPwMusicNote> Notes;
        FString Code;
        FString Message;
        const bool bGenerated = PwScoreGenerateTrackNotes(Score, 0, Notes, Code, Message);
        TestFalse(TEXT("a runaway rule errors instead of truncating"), bGenerated);
        TestEqual(TEXT("runaway rule error code"), Code, FString(ErrorCodes::ERR_INVALID_RECIPE));
        TestEqual(TEXT("failed generation leaves no notes observable"), Notes.Num(), 0);
    }

    return true;
}

// =========================================================================
// G. The error's field path is the exact JSON path the caller must patch. Every one of
//    these is nested somewhere the caller cannot guess.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwMusicScoreFieldPathTest,
    "PinWright.audio.music.score.FieldPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwMusicScoreFieldPathTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicScoreTests;

    // A note that rings past the loop point - named on durationBeats, the field to shorten.
    ExpectFailure(*this, TEXT("note overhang"),
        ScoreWithTrack(FString::Printf(
            TEXT(R"({"name":"lead","role":"lead","octave":4,"instrument":%s,"generation":{"mode":"explicit"},)")
            TEXT(R"("notes":[{"startBeat":0,"durationBeats":1,"degree":0},)")
            TEXT(R"({"startBeat":8,"durationBeats":1,"degree":0},)")
            TEXT(R"({"startBeat":12,"durationBeats":9,"degree":0}]})"), *MinimalInstrument())),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("tracks[0].notes[2].durationBeats"));

    // Out-of-order notes, named on the offending note's own startBeat.
    ExpectFailure(*this, TEXT("unordered notes"),
        ScoreWithTrack(FString::Printf(
            TEXT(R"({"name":"lead","role":"lead","octave":4,"instrument":%s,"generation":{"mode":"explicit"},)")
            TEXT(R"("notes":[{"startBeat":4,"durationBeats":1,"degree":0},)")
            TEXT(R"({"startBeat":1,"durationBeats":1,"degree":0}]})"), *MinimalInstrument())),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("tracks[0].notes[1].startBeat"));

    // An instrument failure is re-rooted from the SFX parser's "layers[0]..." onto the
    // instrument, because a score document has no layers array to patch.
    ExpectFailure(*this, TEXT("unknown instrument generator"),
        MinimalScore().Replace(TEXT(R"("kind":"osc")"), TEXT(R"("kind":"supersaw")")),
        ErrorCodes::ERR_UNKNOWN_GENERATOR, TEXT("tracks[0].instrument.generator.kind"));

    ExpectFailure(*this, TEXT("unknown instrument effect"),
        MinimalScore().Replace(TEXT(R"("frequencyHz":440}}})"),
            TEXT(R"("frequencyHz":440}},"fx":[{"kind":"shimmer","params":{}}]})")),
        ErrorCodes::ERR_UNKNOWN_EFFECT, TEXT("tracks[0].instrument.fx[0].kind"));

    // The three layer fields the score owns elsewhere.
    ExpectFailure(*this, TEXT("instrument carries pan"),
        MinimalScore().Replace(TEXT(R"("instrument":{"generator")"), TEXT(R"("instrument":{"pan":0.5,"generator")")),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("tracks[0].instrument.pan"));

    ExpectFailure(*this, TEXT("instrument carries startMs"),
        MinimalScore().Replace(TEXT(R"("instrument":{"generator")"), TEXT(R"("instrument":{"startMs":10,"generator")")),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("tracks[0].instrument.startMs"));

    ExpectFailure(*this, TEXT("instrument carries gainDb"),
        MinimalScore().Replace(TEXT(R"("instrument":{"generator")"), TEXT(R"("instrument":{"gainDb":-3,"generator")")),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("tracks[0].instrument.gainDb"));

    // A generative track cannot also carry authored notes: the renderer would have two
    // sources of truth for one stem.
    ExpectFailure(*this, TEXT("generative track with notes"),
        MinimalScore().Replace(TEXT(R"("degrees":[0,4]}}}]})"),
            TEXT(R"("degrees":[0,4]}},"notes":[{"startBeat":0,"durationBeats":1,"degree":0}]}]})")),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("tracks[0].notes"));

    // Sections must tile the score; the gap is named on the section that opens it.
    ExpectFailure(*this, TEXT("section gap"),
        TEXT(R"({"bpm":120,"bars":8,"timeSignature":{"numerator":4,"denominator":4},)")
        TEXT(R"("key":{"root":"a","scale":"natural_minor_aeolian"},)")
        TEXT(R"("sections":[{"name":"a","startBar":0,"bars":2},{"name":"b","startBar":4,"bars":4}],)")
        TEXT(R"("tracks":[{"name":"bed","role":"drone","octave":2,)")
        TEXT(R"("instrument":{"generator":{"kind":"osc","params":{"waveform":"sine","frequencyHz":440}}},)")
        TEXT(R"("generation":{"mode":"drone","params":{"degrees":[0]}}}]})"),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("sections[1].startBar"));

    // ... and must reach the end of the score.
    ExpectFailure(*this, TEXT("sections stop short"),
        TEXT(R"({"bpm":120,"bars":8,"timeSignature":{"numerator":4,"denominator":4},)")
        TEXT(R"("key":{"root":"a","scale":"natural_minor_aeolian"},)")
        TEXT(R"("sections":[{"name":"a","startBar":0,"bars":4}],)")
        TEXT(R"("tracks":[{"name":"bed","role":"drone","octave":2,)")
        TEXT(R"("instrument":{"generator":{"kind":"osc","params":{"waveform":"sine","frequencyHz":440}}},)")
        TEXT(R"("generation":{"mode":"drone","params":{"degrees":[0]}}}]})"),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("sections[0]"));

    // Two stems that differ only in case are one file on Windows.
    ExpectFailure(*this, TEXT("duplicate track name"),
        ScoreWithTrack(FString::Printf(TEXT("%s,%s"), *DroneTrack(TEXT("bed")), *DroneTrack(TEXT("BED")))),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("tracks[1].name"));

    // A name that would escape the stem directory.
    ExpectFailure(*this, TEXT("path-bearing track name"),
        MinimalScore().Replace(TEXT(R"("name":"bed")"), TEXT(R"("name":"..\/..\/bed")")),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("tracks[0].name"));

    // A degree that resolves outside MIDI is caught HERE, with a field path, rather than at
    // render time where all that is left is a silent stem.
    ExpectFailure(*this, TEXT("degree out of MIDI range"),
        ScoreWithTrack(FString::Printf(
            TEXT(R"({"name":"bed","role":"drone","octave":9,"instrument":%s,)")
            TEXT(R"("generation":{"mode":"drone","params":{"degrees":[40]}}})"), *MinimalInstrument())),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("tracks[0].generation.params.degrees[0]"));

    // A denominator Quartz cannot express.
    ExpectFailure(*this, TEXT("unschedulable denominator"),
        MinimalScore().Replace(TEXT(R"("denominator":4)"), TEXT(R"("denominator":1)")),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("timeSignature.denominator"));

    // A typo at score level, named exactly.
    ExpectFailure(*this, TEXT("unknown score field"),
        MinimalScore().Replace(TEXT(R"("bpm":120,)"), TEXT(R"("bpm":120,"tempo":120,)")),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("tempo"));

    // Preserve the case-insensitive key behavior used by FJsonObject accessors.
    {
        TSharedPtr<FJsonObject> Object = ParseJson(
            MinimalScore().Replace(TEXT(R"("bpm":120)"), TEXT(R"("BPM":120)")));
        FPwMusicScore Out;
        FPwMusicScoreError Error;
        TestTrue(FString::Printf(TEXT("mixed-case known key parses (%s)"), *Error.ToString()),
            ParseMusicScore(Object, Out, Error));
    }

    return true;
}

// =========================================================================
// H. Beat <-> time. The convention is that one beat is one DENOMINATOR note, so a bar is
//    `numerator` beats. This test is the executable statement of that choice.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwMusicScoreBeatTimeTest,
    "PinWright.audio.music.score.BeatTimeConversion",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwMusicScoreBeatTimeTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicScoreTests;

    {
        const FPwMusicScore Score = ScoreForTiming(120.0, 4, 4, 4, 48000);
        TestEqual(TEXT("4/4 has four beats to the bar"), PwScoreBeatsPerBar(Score), 4.0);
        TestEqual(TEXT("4 bars of 4/4 is 16 beats"), PwScoreTotalBeats(Score), 16.0);
        TestEqual(TEXT("120 bpm puts a beat at 500 ms"), PwScoreBeatToMs(Score, 1.0), 500.0);
        TestEqual(TEXT("a 4/4 bar at 120 bpm is 2000 ms"), PwScoreBeatToMs(Score, 4.0), 2000.0);
        TestEqual(TEXT("whole-score duration"), PwScoreDurationMs(Score), 8000.0);
        TestEqual(TEXT("ms back to beats"), PwScoreMsToBeat(Score, 2000.0), 4.0);
    }

    {
        // The convention's load-bearing case. Under the MIDI convention (beat == quarter
        // note) a 6/8 bar would be 3 beats; here it is 6, and bpm counts eighth notes.
        const FPwMusicScore Score = ScoreForTiming(180.0, 6, 8, 2, 48000);
        TestEqual(TEXT("6/8 has SIX beats to the bar"), PwScoreBeatsPerBar(Score), 6.0);
        TestEqual(TEXT("180 bpm puts an eighth at 333.333 ms"), PwScoreBeatToMs(Score, 1.0), 1000.0 / 3.0, 1.0e-9);
        TestEqual(TEXT("a 6/8 bar at 180 bpm is 2000 ms"), PwScoreBeatToMs(Score, 6.0), 2000.0, 1.0e-9);
    }

    {
        const FPwMusicScore Score = ScoreForTiming(96.0, 7, 8, 3, 44100);
        TestEqual(TEXT("7/8 has seven beats to the bar"), PwScoreBeatsPerBar(Score), 7.0);
        TestEqual(TEXT("3 bars of 7/8 is 21 beats"), PwScoreTotalBeats(Score), 21.0);
    }

    // Round trip at several tempos and meters. Not asserted as bit-exact - 60000/90 is not
    // representable - but tight enough that a wrong formula cannot hide inside it.
    const double Tempos[] = { 60.0, 90.0, 120.0, 137.5, 200.0 };
    const int32 Numerators[] = { 3, 4, 5, 6, 7 };
    const int32 Denominators[] = { 2, 4, 8, 16, 32 };
    const double Beats[] = { 0.0, 1.0, 3.5, 16.0, 127.25 };
    for (int32 Index = 0; Index < 5; ++Index)
    {
        const FPwMusicScore Score = ScoreForTiming(Tempos[Index], Numerators[Index], Denominators[Index], 4, 48000);
        for (double Beat : Beats)
        {
            const double Ms = PwScoreBeatToMs(Score, Beat);
            const double Back = PwScoreMsToBeat(Score, Ms);
            const double Tolerance = FMath::Max(1.0, FMath::Abs(Beat)) * 1.0e-9;
            TestTrue(FString::Printf(TEXT("beat %g round-trips at %g bpm (got %.12f)"), Beat, Tempos[Index], Back),
                FMath::Abs(Back - Beat) <= Tolerance);
        }
        // The denominator names what a beat IS; it must not also scale how long one lasts.
        TestEqual(FString::Printf(TEXT("beat length at %g bpm ignores the denominator"), Tempos[Index]),
            PwScoreBeatToMs(Score, 1.0), 60000.0 / Tempos[Index], 1.0e-9);
    }

    return true;
}

// =========================================================================
// I. Loop length. The interesting assertion is the FAILURE one: a loop that does not land
//    on a whole frame must say so, with the residual, instead of rounding.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwMusicScoreLoopLengthTest,
    "PinWright.audio.music.score.LoopLength",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwMusicScoreLoopLengthTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicScoreTests;

    // 48 kHz at the common tempos: 8 bars of 4/4 at 120 bpm is exactly 768000 frames.
    {
        const FPwScoreLoop Loop = PwScoreLoopLength(ScoreForTiming(120.0, 4, 4, 8, 48000));
        TestTrue(TEXT("120 bpm / 48k / 8 bars is sample-exact"), Loop.bExact);
        TestEqual(TEXT("frame count"), Loop.Frames, static_cast<int64>(768000));
        TestEqual(TEXT("no residual on an exact loop"), Loop.ResidualFrames, 0.0);
        TestEqual(TEXT("duration"), Loop.DurationMs, 16000.0, 1.0e-9);
        TestEqual(TEXT("an exact loop reports its own tempo as the exact one"), Loop.NearestExactBpm, 120.0, 1.0e-9);
    }

    {
        const FPwScoreLoop Loop = PwScoreLoopLength(ScoreForTiming(90.0, 4, 4, 4, 48000));
        TestTrue(TEXT("90 bpm / 48k / 4 bars is sample-exact"), Loop.bExact);
        TestEqual(TEXT("frame count"), Loop.Frames, static_cast<int64>(512000));
    }

    // The same tempo is exact at one bar count and not at another, which is exactly why this
    // has to be measured per score rather than assumed from the tempo.
    {
        const FPwScoreLoop OneBar = PwScoreLoopLength(ScoreForTiming(140.0, 4, 4, 1, 48000));
        TestFalse(TEXT("140 bpm / 48k / 1 bar is NOT sample-exact"), OneBar.bExact);
        const FPwScoreLoop SevenBars = PwScoreLoopLength(ScoreForTiming(140.0, 4, 4, 7, 48000));
        TestTrue(TEXT("140 bpm / 48k / 7 bars IS sample-exact"), SevenBars.bExact);
        TestEqual(TEXT("seven-bar frame count"), SevenBars.Frames, static_cast<int64>(576000));
    }

    // The honest failure case: 44.1 kHz at an awkward tempo. Reported, never rounded away.
    {
        const FPwScoreLoop Loop = PwScoreLoopLength(ScoreForTiming(130.0, 4, 4, 1, 44100));
        TestFalse(TEXT("130 bpm / 44.1k / 1 bar is NOT sample-exact"), Loop.bExact);
        TestEqual(TEXT("nearest whole frame count"), Loop.Frames, static_cast<int64>(81415));
        TestTrue(TEXT("the residual is reported, not swallowed"), FMath::Abs(Loop.ResidualFrames) > 0.3);
        TestTrue(TEXT("the residual is signed toward the unrounded value"), Loop.ResidualFrames > 0.0);
        TestEqual(TEXT("unrounded frame count"), Loop.ExactFrames, 10584000.0 / 130.0, 1.0e-6);
        // Recovery information: the tempo that WOULD be exact at this bar count and rate.
        TestTrue(TEXT("a nearby exact tempo is offered"), Loop.NearestExactBpm > 130.0 && Loop.NearestExactBpm < 130.01);
        const FPwScoreLoop Fixed = PwScoreLoopLength(
            ScoreForTiming(Loop.NearestExactBpm, 4, 4, 1, 44100));
        TestTrue(TEXT("the offered tempo really is exact"), Fixed.bExact);
        TestEqual(TEXT("the offered tempo keeps the frame count"), Fixed.Frames, Loop.Frames);
    }

    // A fractional tempo takes the floating-point path and must still be honest about it.
    {
        const FPwScoreLoop Loop = PwScoreLoopLength(ScoreForTiming(137.5, 4, 4, 11, 48000));
        // 11 * 4 * 60 * 48000 / 137.5 = 921600 exactly.
        TestTrue(TEXT("a fractional tempo can still be exact"), Loop.bExact);
        TestEqual(TEXT("fractional-tempo frame count"), Loop.Frames, static_cast<int64>(921600));
    }

    // A degenerate score reports nothing rather than a plausible zero-length loop.
    {
        FPwMusicScore Empty;
        const FPwScoreLoop Loop = PwScoreLoopLength(Empty);
        TestFalse(TEXT("a score with no tempo is not exact"), Loop.bExact);
        TestEqual(TEXT("a score with no tempo has no frames"), Loop.Frames, static_cast<int64>(0));
        TestEqual(TEXT("a score with no tempo offers no exact tempo"), Loop.NearestExactBpm, 0.0);
    }

    return true;
}

// =========================================================================
// J. Generation determinism. The same seed must produce byte-identical notes; a different
//    seed must move the stochastic modes and must NOT move the drone, which draws no
//    randomness at all.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwMusicScoreGenerationDeterminismTest,
    "PinWright.audio.music.score.GenerationDeterminism",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwMusicScoreGenerationDeterminismTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicScoreTests;

    auto MaterializeAll = [this](int32 Seed, TArray<FString>& OutDumps) -> bool
    {
        TSharedPtr<FJsonObject> Object = ParseJson(SeededScore(Seed));
        FPwMusicScore Score;
        FPwMusicScoreError Error;
        if (!ParseMusicScore(Object, Score, Error))
        {
            AddError(FString::Printf(TEXT("seeded fixture failed to parse: %s"), *Error.ToString()));
            return false;
        }
        OutDumps.Reset();
        for (int32 Index = 0; Index < Score.Tracks.Num(); ++Index)
        {
            TArray<FPwMusicNote> Notes;
            FString Code;
            FString Message;
            if (!PwScoreGenerateTrackNotes(Score, Index, Notes, Code, Message))
            {
                AddError(FString::Printf(TEXT("track %d failed to materialize: %s (%s)"), Index, *Message, *Code));
                return false;
            }
            OutDumps.Add(NotesToString(Notes));
        }
        return true;
    };

    TArray<FString> SeedSevenA;
    TArray<FString> SeedSevenB;
    TArray<FString> SeedEight;
    if (!MaterializeAll(7, SeedSevenA) || !MaterializeAll(7, SeedSevenB) || !MaterializeAll(8, SeedEight))
    {
        return false;
    }

    TestEqual(TEXT("three tracks materialized"), SeedSevenA.Num(), 3);
    if (SeedSevenA.Num() != 3 || SeedSevenB.Num() != 3 || SeedEight.Num() != 3)
    {
        return false;
    }

    // Every generative mode produces something, or the determinism assertions below would
    // be comparing two empty strings and passing for the wrong reason.
    for (int32 Index = 0; Index < 3; ++Index)
    {
        TestFalse(FString::Printf(TEXT("track %d produced notes"), Index), SeedSevenA[Index].IsEmpty());
    }

    TestEqual(TEXT("drone is byte-identical across runs"), SeedSevenB[0], SeedSevenA[0]);
    TestEqual(TEXT("evolving_pad is byte-identical across runs"), SeedSevenB[1], SeedSevenA[1]);
    TestEqual(TEXT("sparse_events is byte-identical across runs"), SeedSevenB[2], SeedSevenA[2]);

    // A drone consumes no randomness, so the seed cannot move it. That is a property, not a
    // gap: a bed that changed when an unrelated track's seed moved would not be a bed.
    TestEqual(TEXT("drone is seed-invariant by construction"), SeedEight[0], SeedSevenA[0]);
    TestTrue(TEXT("evolving_pad differs on a different seed"), SeedEight[1] != SeedSevenA[1]);
    TestTrue(TEXT("sparse_events differs on a different seed"), SeedEight[2] != SeedSevenA[2]);

    // Order independence: materializing track 2 first must not change what track 2 gets.
    {
        TSharedPtr<FJsonObject> Object = ParseJson(SeededScore(7));
        FPwMusicScore Score;
        FPwMusicScoreError Error;
        TestTrue(TEXT("seeded fixture parses"), ParseMusicScore(Object, Score, Error));

        TArray<FPwMusicNote> Notes;
        FString Code;
        FString Message;
        TestTrue(TEXT("track 2 materializes alone"), PwScoreGenerateTrackNotes(Score, 2, Notes, Code, Message));
        TestEqual(TEXT("track 2 is identical whether or not tracks 0-1 ran first"),
            NotesToString(Notes), SeedSevenA[2]);
    }

    // Structural properties every materialized set must hold.
    {
        TSharedPtr<FJsonObject> Object = ParseJson(SeededScore(7));
        FPwMusicScore Score;
        FPwMusicScoreError Error;
        TestTrue(TEXT("seeded fixture parses again"), ParseMusicScore(Object, Score, Error));
        const double TotalBeats = PwScoreTotalBeats(Score);

        for (int32 Index = 0; Index < Score.Tracks.Num(); ++Index)
        {
            TArray<FPwMusicNote> Notes;
            FString Code;
            FString Message;
            TestTrue(FString::Printf(TEXT("track %d materializes"), Index),
                PwScoreGenerateTrackNotes(Score, Index, Notes, Code, Message));

            double Previous = -1.0;
            bool bOrdered = true;
            bool bInside = true;
            for (const FPwMusicNote& Note : Notes)
            {
                bOrdered = bOrdered && Note.StartBeat >= Previous;
                Previous = Note.StartBeat;
                bInside = bInside && Note.StartBeat >= 0.0 &&
                    (Note.StartBeat + Note.DurationBeats) <= TotalBeats + UE_DOUBLE_KINDA_SMALL_NUMBER &&
                    Note.DurationBeats > 0.0;
            }
            TestTrue(FString::Printf(TEXT("track %d notes are ordered"), Index), bOrdered);
            TestTrue(FString::Printf(TEXT("track %d notes stay inside the loop"), Index), bInside);
            TestTrue(FString::Printf(TEXT("track %d respects the note cap"), Index),
                Notes.Num() <= PwMusicLimits::MaxNotesPerTrack);
        }
    }

    // A track index the score does not have is an error, not an empty success.
    {
        TSharedPtr<FJsonObject> Object = ParseJson(SeededScore(7));
        FPwMusicScore Score;
        FPwMusicScoreError Error;
        TestTrue(TEXT("seeded fixture parses once more"), ParseMusicScore(Object, Score, Error));

        TArray<FPwMusicNote> Notes;
        Notes.Add(FPwMusicNote());
        FString Code;
        FString Message;
        TestFalse(TEXT("an out-of-range track index errors"),
            PwScoreGenerateTrackNotes(Score, 99, Notes, Code, Message));
        TestEqual(TEXT("out-of-range track index error code"), Code, FString(ErrorCodes::ERR_INVALID_RECIPE));
        TestEqual(TEXT("a failed materialization leaves nothing observable"), Notes.Num(), 0);
    }

    return true;
}

// =========================================================================
// K. Section intensity actually does something, and it does it only to generated notes.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwMusicScoreSectionIntensityTest,
    "PinWright.audio.music.score.SectionIntensity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwMusicScoreSectionIntensityTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicScoreTests;

    const FString Json =
        TEXT(R"({"bpm":120,"bars":8,"timeSignature":{"numerator":4,"denominator":4},)")
        TEXT(R"("key":{"root":"c","scale":"major"},)")
        TEXT(R"("sections":[{"name":"quiet","startBar":0,"bars":4,"intensityFloor":0.0,"intensityCeil":0.25},)")
        TEXT(R"({"name":"loud","startBar":4,"bars":4,"intensityFloor":0.9,"intensityCeil":1.0}],)")
        TEXT(R"("tracks":[{"name":"bed","role":"drone","octave":3,)")
        TEXT(R"("instrument":{"generator":{"kind":"osc","params":{"waveform":"sine","frequencyHz":220}}},)")
        TEXT(R"("generation":{"mode":"drone","params":{"degrees":[0],"velocity":1.0}}},)")
        TEXT(R"({"name":"motif","role":"lead","octave":4,)")
        TEXT(R"("instrument":{"generator":{"kind":"osc","params":{"waveform":"saw","frequencyHz":440}}},)")
        TEXT(R"("generation":{"mode":"explicit"},)")
        TEXT(R"("notes":[{"startBeat":0,"durationBeats":1,"degree":0,"velocity":1.0},)")
        TEXT(R"({"startBeat":16,"durationBeats":1,"degree":0,"velocity":1.0}]}]})");

    TSharedPtr<FJsonObject> Object = ParseJson(Json);
    FPwMusicScore Score;
    FPwMusicScoreError Error;
    const bool bParsed = ParseMusicScore(Object, Score, Error);
    TestTrue(FString::Printf(TEXT("intensity fixture parses (%s)"), *Error.ToString()), bParsed);
    if (!bParsed || Score.Tracks.Num() != 2)
    {
        return false;
    }

    TArray<FPwMusicNote> DroneNotes;
    FString Code;
    FString Message;
    TestTrue(TEXT("drone materializes"), PwScoreGenerateTrackNotes(Score, 0, DroneNotes, Code, Message));
    TestEqual(TEXT("one held note per section"), DroneNotes.Num(), 2);
    if (DroneNotes.Num() == 2)
    {
        // velocity 1.0 remapped into each section's band.
        TestEqual(TEXT("quiet section caps the generated velocity"), DroneNotes[0].Velocity, 0.25, 1.0e-9);
        TestEqual(TEXT("loud section lifts the generated velocity"), DroneNotes[1].Velocity, 1.0, 1.0e-9);
        TestEqual(TEXT("section 0 note spans its section"), DroneNotes[0].DurationBeats, 16.0);
        TestEqual(TEXT("section 1 note starts at its section"), DroneNotes[1].StartBeat, 16.0);
    }

    TArray<FPwMusicNote> AuthoredNotes;
    TestTrue(TEXT("explicit track materializes"), PwScoreGenerateTrackNotes(Score, 1, AuthoredNotes, Code, Message));
    TestEqual(TEXT("authored note count"), AuthoredNotes.Num(), 2);
    if (AuthoredNotes.Num() == 2)
    {
        // Authored velocities are NOT remapped. The author already decided, and rescaling
        // them behind their back is the silent repair the schema exists to prevent.
        TestEqual(TEXT("authored velocity in the quiet section is untouched"), AuthoredNotes[0].Velocity, 1.0);
        TestEqual(TEXT("authored velocity in the loud section is untouched"), AuthoredNotes[1].Velocity, 1.0);
    }

    // The section lookup agrees with the tiling.
    const FPwMusicSection* First = PwScoreSectionAtBar(Score, 0);
    const FPwMusicSection* Second = PwScoreSectionAtBar(Score, 7);
    TestNotNull(TEXT("bar 0 resolves to a section"), First);
    TestNotNull(TEXT("bar 7 resolves to a section"), Second);
    if (First && Second)
    {
        TestEqual(TEXT("bar 0 is in 'quiet'"), First->Name, FString(TEXT("quiet")));
        TestEqual(TEXT("bar 7 is in 'loud'"), Second->Name, FString(TEXT("loud")));
    }
    TestNull(TEXT("a bar past the score resolves to nothing"), PwScoreSectionAtBar(Score, 99));

    return true;
}

// =========================================================================
// L. Pitch. Degrees resolve through the ENGINE's scale table, and the vocabulary mirrors
//    the engine's enum, so a score means the same thing here and in a MetaSound graph.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwMusicScorePitchResolutionTest,
    "PinWright.audio.music.score.PitchResolution",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwMusicScorePitchResolutionTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicScoreTests;

    FString Code;
    FString Message;
    int32 Midi = 0;

    // C4 = MIDI 60 is the convention every octave here is stated in.
    TestTrue(TEXT("C major degree 0 at octave 4 resolves"),
        PwScoreDegreeToMidi(EPwMusicScale::Major, EPwMusicPitchClass::C, 4, 0, Midi, Code, Message));
    TestEqual(TEXT("degree 0 is the root"), Midi, 60);

    TestTrue(TEXT("C major degree 4 resolves"),
        PwScoreDegreeToMidi(EPwMusicScale::Major, EPwMusicPitchClass::C, 4, 4, Midi, Code, Message));
    TestEqual(TEXT("the fifth degree of major is 7 semitones up"), Midi, 67);

    // Degrees wrap octaves at the SCALE's degree count, not at 7 and not at 12.
    TestTrue(TEXT("C major degree 7 resolves"),
        PwScoreDegreeToMidi(EPwMusicScale::Major, EPwMusicPitchClass::C, 4, 7, Midi, Code, Message));
    TestEqual(TEXT("degree 7 of a 7-degree scale is the octave"), Midi, 72);

    TestTrue(TEXT("C major-pentatonic degree 5 resolves"),
        PwScoreDegreeToMidi(EPwMusicScale::MajorPentatonic, EPwMusicPitchClass::C, 4, 5, Midi, Code, Message));
    TestEqual(TEXT("degree 5 of a 5-degree scale is the octave"), Midi, 72);

    // Negative degrees walk DOWN, not toward zero.
    TestTrue(TEXT("C major degree -1 resolves"),
        PwScoreDegreeToMidi(EPwMusicScale::Major, EPwMusicPitchClass::C, 4, -1, Midi, Code, Message));
    TestEqual(TEXT("degree -1 is the seventh below"), Midi, 59);

    TestTrue(TEXT("C major degree -7 resolves"),
        PwScoreDegreeToMidi(EPwMusicScale::Major, EPwMusicPitchClass::C, 4, -7, Midi, Code, Message));
    TestEqual(TEXT("degree -7 is the octave below"), Midi, 48);

    // The root's pitch class shifts everything.
    TestTrue(TEXT("A natural minor degree 0 at octave 3 resolves"),
        PwScoreDegreeToMidi(EPwMusicScale::NaturalMinorAeolian, EPwMusicPitchClass::A, 3, 0, Midi, Code, Message));
    TestEqual(TEXT("A3 is MIDI 57"), Midi, 57);

    // Out of range is an ERROR, not a clamp.
    const int32 Before = Midi;
    TestFalse(TEXT("a degree past MIDI 127 errors"),
        PwScoreDegreeToMidi(EPwMusicScale::Major, EPwMusicPitchClass::C, 9, 40, Midi, Code, Message));
    TestEqual(TEXT("out-of-range degree error code"), Code, FString(ErrorCodes::ERR_INVALID_PARAMS));
    TestTrue(TEXT("the message names the resolved note"), Message.Contains(TEXT("MIDI")));
    TestTrue(TEXT("out-of-range resolution did not report a plausible note"),
        Midi == 0 || Midi != Before);

    TestFalse(TEXT("an unspecified scale errors"),
        PwScoreDegreeToMidi(EPwMusicScale::Unspecified, EPwMusicPitchClass::C, 4, 0, Midi, Code, Message));
    TestFalse(TEXT("an unspecified root errors"),
        PwScoreDegreeToMidi(EPwMusicScale::Major, EPwMusicPitchClass::Unspecified, 4, 0, Midi, Code, Message));

    // Frequency comes from the engine's own tuning.
    TestEqual(TEXT("A4 is 440 Hz"), PwScoreMidiToFrequencyHz(69.0), 440.0, 1.0e-3);
    TestEqual(TEXT("A3 is 220 Hz"), PwScoreMidiToFrequencyHz(57.0), 220.0, 1.0e-3);

    // An absolute midi note is NEVER snapped into the key, even when it is out of key.
    {
        FPwMusicScore Score;
        Score.Key.Root = EPwMusicPitchClass::C;
        Score.Key.Scale = EPwMusicScale::Major;
        FPwMusicTrack Track;
        Track.Octave = 4;
        FPwMusicNote Note;
        Note.Midi = 61;     // C#, not in C major
        TestTrue(TEXT("an out-of-key midi note resolves"),
            PwScoreResolveNoteMidi(Score, Track, Note, Midi, Code, Message));
        TestEqual(TEXT("an out-of-key midi note is returned verbatim"), Midi, 61);
    }

    // The vocabulary is a 1:1 mirror of the engine's, which is what makes a score and a
    // MetaSound quantizer node agree about what a scale name means.
    TestEqual(TEXT("scale vocabulary size matches Audio::EMusicalScale::Count"),
        static_cast<int32>(EPwMusicScale::Count) - 1, static_cast<int32>(Audio::EMusicalScale::Count));

    for (uint8 Index = 1; Index < static_cast<uint8>(EPwMusicScale::Count); ++Index)
    {
        const EPwMusicScale Scale = static_cast<EPwMusicScale>(Index);
        const FString Name = PwMusicScaleToString(Scale);
        EPwMusicScale Parsed = EPwMusicScale::Unspecified;
        TestTrue(FString::Printf(TEXT("scale '%s' round-trips through its wire name"), *Name),
            PwMusicScaleFromString(Name, Parsed) && Parsed == Scale);

        int32 DegreeCount = 0;
        TestTrue(FString::Printf(TEXT("scale '%s' has degrees in the engine table"), *Name),
            PwMusicScaleDegreeCount(Scale, DegreeCount) && DegreeCount > 0);
    }

    int32 UnspecifiedCount = 0;
    TestFalse(TEXT("the unspecified scale has no degree set"),
        PwMusicScaleDegreeCount(EPwMusicScale::Unspecified, UnspecifiedCount));

    for (uint8 Index = 1; Index < static_cast<uint8>(EPwMusicPitchClass::Count); ++Index)
    {
        const EPwMusicPitchClass Pitch = static_cast<EPwMusicPitchClass>(Index);
        EPwMusicPitchClass Parsed = EPwMusicPitchClass::Unspecified;
        TestTrue(FString::Printf(TEXT("pitch class '%s' round-trips"), PwMusicPitchClassToString(Pitch)),
            PwMusicPitchClassFromString(PwMusicPitchClassToString(Pitch), Parsed) && Parsed == Pitch);
        TestEqual(FString::Printf(TEXT("pitch class '%s' semitone"), PwMusicPitchClassToString(Pitch)),
            PwMusicPitchClassSemitone(Pitch), static_cast<int32>(Index) - 1);
    }

    for (uint8 Index = 1; Index < static_cast<uint8>(EPwMusicTrackRole::Count); ++Index)
    {
        const EPwMusicTrackRole Role = static_cast<EPwMusicTrackRole>(Index);
        EPwMusicTrackRole Parsed = EPwMusicTrackRole::Unspecified;
        TestTrue(FString::Printf(TEXT("role '%s' round-trips"), PwMusicTrackRoleToString(Role)),
            PwMusicTrackRoleFromString(PwMusicTrackRoleToString(Role), Parsed) && Parsed == Role);
    }

    for (uint8 Index = 1; Index < static_cast<uint8>(EPwMusicGenerationMode::Count); ++Index)
    {
        const EPwMusicGenerationMode Mode = static_cast<EPwMusicGenerationMode>(Index);
        EPwMusicGenerationMode Parsed = EPwMusicGenerationMode::Unspecified;
        TestTrue(FString::Printf(TEXT("mode '%s' round-trips"), PwMusicGenerationModeToString(Mode)),
            PwMusicGenerationModeFromString(PwMusicGenerationModeToString(Mode), Parsed) && Parsed == Mode);
        TestNotNull(FString::Printf(TEXT("mode '%s' publishes a parameter table"), PwMusicGenerationModeToString(Mode)),
            PwMusicGenerationSpec(Mode).Params);
    }

    // An unrecognised token never resolves to anything.
    EPwMusicScale Rejected = EPwMusicScale::Blues;
    TestFalse(TEXT("an unknown scale name does not resolve"), PwMusicScaleFromString(TEXT("hungarian_minor"), Rejected));
    TestTrue(TEXT("a rejected lookup leaves the out-parameter alone"), Rejected == EPwMusicScale::Blues);

    return true;
}

// =========================================================================
// M. Cross-parameter invariants inside a generation block, checked by the SAME validator
//    hook the SFX kinds use.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwMusicScoreGenerationConstraintsTest,
    "PinWright.audio.music.score.GenerationConstraints",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwMusicScoreGenerationConstraintsTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicScoreTests;

    const FString PadTrack =
        TEXT(R"({"name":"pad","role":"pad","octave":4,)")
        TEXT(R"("instrument":{"generator":{"kind":"osc","params":{"waveform":"sine","frequencyHz":440}}},)")
        TEXT(R"("generation":{"mode":"evolving_pad","params":{"degrees":[0,2],"swellBeatsMin":8,"swellBeatsMax":2}}})");
    ExpectFailure(*this, TEXT("inverted swell range"), ScoreWithTrack(PadTrack),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("tracks[0].generation.params.swellBeatsMax"));

    const FString VelocityTrack =
        TEXT(R"({"name":"pad","role":"pad","octave":4,)")
        TEXT(R"("instrument":{"generator":{"kind":"osc","params":{"waveform":"sine","frequencyHz":440}}},)")
        TEXT(R"("generation":{"mode":"evolving_pad","params":{"degrees":[0,2],"swellBeatsMin":2,"swellBeatsMax":8,)")
        TEXT(R"("velocityMin":0.9,"velocityMax":0.2}}})");
    ExpectFailure(*this, TEXT("inverted velocity range"), ScoreWithTrack(VelocityTrack),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("tracks[0].generation.params.velocityMax"));

    const FString FractionalDegree =
        TEXT(R"({"name":"pad","role":"pad","octave":4,)")
        TEXT(R"("instrument":{"generator":{"kind":"osc","params":{"waveform":"sine","frequencyHz":440}}},)")
        TEXT(R"("generation":{"mode":"drone","params":{"degrees":[0,1.5]}}})");
    ExpectFailure(*this, TEXT("fractional degree"), ScoreWithTrack(FractionalDegree),
        ErrorCodes::ERR_INVALID_RECIPE, TEXT("tracks[0].generation.params.degrees[1]"));

    // An out-of-range degree is caught by the row's own bounds, one level earlier.
    const FString HugeDegree =
        TEXT(R"({"name":"pad","role":"pad","octave":4,)")
        TEXT(R"("instrument":{"generator":{"kind":"osc","params":{"waveform":"sine","frequencyHz":440}}},)")
        TEXT(R"("generation":{"mode":"drone","params":{"degrees":[900]}}})");
    ExpectFailure(*this, TEXT("out-of-range degree"), ScoreWithTrack(HugeDegree),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("tracks[0].generation.params.degrees[0]"));

    // The whole reachable pitch set is pre-validated, so a spread that would push a legal
    // degree off the keyboard is named at the spread rather than at render time.
    const FString SpreadTrack =
        TEXT(R"({"name":"pad","role":"pad","octave":8,)")
        TEXT(R"("instrument":{"generator":{"kind":"osc","params":{"waveform":"sine","frequencyHz":440}}},)")
        TEXT(R"("generation":{"mode":"sparse_events","params":{"degrees":[0],"eventsPerBar":1,)")
        TEXT(R"("durationBeatsMin":1,"durationBeatsMax":2,"octaveSpread":2}}})");
    ExpectFailure(*this, TEXT("octave spread leaves the keyboard"), ScoreWithTrack(SpreadTrack),
        ErrorCodes::ERR_INVALID_PARAMS, TEXT("tracks[0].generation.params.octaveSpread"));

    return true;
}
