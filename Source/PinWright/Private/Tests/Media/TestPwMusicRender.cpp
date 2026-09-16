// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the score renderer (AudioGen/PwMusicRender.h).
//
// The property that matters here is SEAMLESSNESS, and it is asserted the only way that means
// anything: a stem is rendered, concatenated with itself, and the join is compared against the
// deltas inside the body. Two wrong implementations are specifically targeted by that test -
// one that TRUNCATES at the loop point (its head is digital silence where the wrapped tail
// belongs, and its join steps) and one that FADES at the loop point (its join is smooth
// because it ducked the audio to zero, and its head is still silent). Both fail; only the
// wrap-around passes.
//
// Everything else is measured against ground truth rather than against "it returned true": a
// note at MIDI 69 has a known STFT bin, a note at beat 4 of a 120 bpm 4/4 score has a known
// sample index (96000, not "about two seconds"), a mixdown has a known per-sample sum, and a
// tempo that does not divide the sample rate has a known non-zero residual and a known tempo
// that would be exact. Per rpc-design.md §12 the failure direction carries equal weight: a
// sample rate the synth kernel cannot render, an empty track list and a note past the score's
// end must each abort with a registered code and leave an UNMEASURED report, because a
// half-rendered set of stems that reports success is worse than no stems at all.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwMusicRender.h"
#include "AudioGen/PwMusicScore.h"
#include "AudioGen/PwStft.h"
#include "AudioGen/PwSynthDsp.h"
#include "Handlers/ErrorCodes.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Named (not anonymous) namespace: the module builds with bUseUnity = true, and an anonymous
// namespace here would still collide with a sibling test file's identically-named helper.
namespace PwMusicRenderTests
{
    constexpr int32 TestSampleRate = 48000;

    /** Fixtures go through the real parser, so every test renders the score production produces. */
    bool BuildScore(const FString& Json, FPwMusicScore& Out, FString& OutError)
    {
        TSharedPtr<FJsonObject> Root;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
        if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
        {
            OutError = TEXT("fixture is not valid JSON");
            return false;
        }
        return ParseMusicScore(Root, Out, OutError);
    }

    FString ScoreJson(double Bpm, int32 Bars, const FString& Tracks, int32 Seed = 7,
        int32 SampleRate = TestSampleRate)
    {
        return FString::Printf(
            TEXT(R"({"seed":%d,"sampleRate":%d,"bpm":%f,"bars":%d,)")
            TEXT(R"("timeSignature":{"numerator":4,"denominator":4},)")
            TEXT(R"("key":{"root":"c","scale":"major"},"tracks":[%s]})"),
            Seed, SampleRate, Bpm, Bars, *Tracks);
    }

    /** A steady oscillator with no envelope: the whole note is body, so nothing wraps. */
    FString SteadyOscInstrument(const TCHAR* Waveform = TEXT("sine"), double PhaseTurns = 0.0)
    {
        return FString::Printf(
            TEXT(R"({"generator":{"kind":"osc","params":{"waveform":"%s","frequencyHz":440,"phase":%f}}})"),
            Waveform, PhaseTurns);
    }

    /**
     * An oscillator whose amp envelope RELEASES TO EXACTLY ZERO after ReleaseMs. That final zero
     * is what tells the renderer the voice ends - PwSynthDsp's ApplyAmpEnvelope holds the last
     * point's value forever otherwise - so it is also what makes the note carry a tail past its
     * own notated length.
     */
    FString DecayingOscInstrument(double ReleaseMs)
    {
        return FString::Printf(
            TEXT(R"({"generator":{"kind":"osc","params":{"waveform":"sine","frequencyHz":440}},)")
            TEXT(R"("ampEnvelope":[{"timeMs":0,"value":1,"curve":"linear"},{"timeMs":%f,"value":0}]})"),
            ReleaseMs);
    }

    FString ExplicitTrack(const TCHAR* Name, const TCHAR* Role, int32 Octave,
        const FString& Instrument, const FString& Notes, double GainDb = 0.0, double Pan = 0.0)
    {
        return FString::Printf(
            TEXT(R"({"name":"%s","role":"%s","octave":%d,"gainDb":%f,"pan":%f,)")
            TEXT(R"("instrument":%s,"generation":{"mode":"explicit"},"notes":[%s]})"),
            Name, Role, Octave, GainDb, Pan, *Instrument, *Notes);
    }

    FString MidiNote(double StartBeat, double DurationBeats, int32 Midi, double Velocity = 1.0)
    {
        return FString::Printf(
            TEXT(R"({"startBeat":%f,"durationBeats":%f,"midi":%d,"velocity":%f})"),
            StartBeat, DurationBeats, Midi, Velocity);
    }

    FString DegreeNote(double StartBeat, double DurationBeats, int32 Degree, double Velocity = 1.0)
    {
        return FString::Printf(
            TEXT(R"({"startBeat":%f,"durationBeats":%f,"degree":%d,"velocity":%f})"),
            StartBeat, DurationBeats, Degree, Velocity);
    }

    /**
     * A quiet NaN built from its IEEE-754 bit pattern rather than from sqrt(-1) or <limits>,
     * which a fast-math build is free to constant-fold into something that is not a NaN at all -
     * silently turning the ordering test below into a test of nothing. Same construction as
     * TestPwAudioCompare.cpp's; duplicated rather than shared because Unity merges these
     * translation units and a shared helper would need a header of its own for one line.
     */
    float MakeNaN()
    {
        constexpr uint32 QuietNaNBits = 0x7FC00000u;
        float Value = 0.f;
        FMemory::Memcpy(&Value, &QuietNaNBits, sizeof(Value));
        return Value;
    }

    /** Bit-for-bit, not "within tolerance". A tolerance would pass the defect under test. */
    bool ChannelsAreByteIdentical(const TArray<float>& A, const TArray<float>& B)
    {
        if (A.Num() != B.Num())
        {
            return false;
        }
        return A.Num() == 0
            || FMemory::Memcmp(A.GetData(), B.GetData(), A.Num() * sizeof(float)) == 0;
    }

    bool StemsAreByteIdentical(const FPwStemResult& A, const FPwStemResult& B)
    {
        return A.Buffer.SampleRate == B.Buffer.SampleRate
            && ChannelsAreByteIdentical(A.Buffer.Left, B.Buffer.Left)
            && ChannelsAreByteIdentical(A.Buffer.Right, B.Buffer.Right);
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

    /** Largest sample-to-sample step anywhere in [First, First + Count). */
    float MaxDelta(const TArray<float>& Channel, int32 First, int32 Count)
    {
        float Largest = 0.f;
        const int32 Last = FMath::Min(First + Count, Channel.Num());
        for (int32 Index = FMath::Max(First, 1); Index < Last; ++Index)
        {
            Largest = FMath::Max(Largest, FMath::Abs(Channel[Index] - Channel[Index - 1]));
        }
        return Largest;
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

    /**
     * The full "nothing survived the failure" contract, asserted the same way everywhere.
     *
     * The report starts pre-populated with a sentinel, so a renderer that aborted but left its
     * caller's report alone would fail here rather than pass a code-only assertion.
     */
    void CheckAbortedRender(FAutomationTestBase& Test, const FString& What,
        bool bReturned, const FPwMusicRenderReport& Report, const FString& Code,
        const FString& Error, const FString& ExpectedCode)
    {
        Test.TestFalse(What + TEXT(" aborts the render"), bReturned);
        Test.TestEqual(What + TEXT(" reports its code"), Code, ExpectedCode);
        Test.TestFalse(What + TEXT(" carries a message"), Error.IsEmpty());
        Test.TestFalse(What + TEXT(" leaves the report unmeasured"), Report.bMeasured);
        Test.TestEqual(What + TEXT(" leaves no partial stems"), Report.Stems.Num(), 0);
        Test.TestEqual(What + TEXT(" leaves no note count"), Report.TotalNotes, 0);
    }

    /** Pre-populated so "the renderer cleared it" is distinguishable from "it was already empty". */
    FPwMusicRenderReport SentinelReport()
    {
        FPwMusicRenderReport Report;
        Report.bMeasured = true;
        Report.TotalNotes = 4242;
        Report.Stems.AddDefaulted();
        return Report;
    }
}

// =========================================================================
// Pitch: the note the score wrote is the note that comes out
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwMusicRenderPitchTest,
    "PinWright.audio.music.render.OneNoteStemCarriesItsNotePitch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwMusicRenderPitchTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicRenderTests;

    // MIDI 69 is A4 = 440 Hz exactly, so the expected bin needs no tuning assumption of its own.
    // The instrument's own frequencyHz is 440 too - deliberately, so this test would still pass
    // a renderer that ignored the note. The second half of the test moves the note off it.
    const FString Json = ScoreJson(120.0, 1,
        ExplicitTrack(TEXT("lead"), TEXT("lead"), 4, SteadyOscInstrument(), MidiNote(0.0, 2.0, 69)));

    FPwMusicScore Score;
    FString ParseError;
    if (!BuildScore(Json, Score, ParseError))
    {
        AddError(FString::Printf(TEXT("fixture did not parse: %s"), *ParseError));
        return false;
    }

    const auto MeasureFundamental = [this](const FPwMusicScore& InScore, const TCHAR* Label) -> double
    {
        FPwMusicRenderReport Report;
        FString Code;
        FString Error;
        if (!PwRenderScoreStems(InScore, Report, Code, Error))
        {
            AddError(FString::Printf(TEXT("%s: render failed: %s %s"), Label, *Code, *Error));
            return 0.0;
        }

        TestTrue(FString::Printf(TEXT("%s: report is measured"), Label), Report.bMeasured);
        TestEqual(FString::Printf(TEXT("%s: one stem"), Label), Report.Stems.Num(), 1);
        if (Report.Stems.Num() != 1)
        {
            return 0.0;
        }

        const FPwStemResult& Stem = Report.Stems[0];
        TestTrue(FString::Printf(TEXT("%s: stem is measured"), Label), Stem.bMeasured);
        TestEqual(FString::Printf(TEXT("%s: the note reached the stem"), Label), Stem.NotesRendered, 1);
        TestEqual(FString::Printf(TEXT("%s: nothing was dropped"), Label), Stem.NotesDropped, 0);

        FPwStftSettings Settings;
        Settings.FftSize = 4096;
        Settings.HopSize = 1024;

        FPwStftResult Stft;
        FPwStftError StftError;
        if (!PwComputeStft(Stem.Buffer.Left, Stem.Buffer.SampleRate, Settings, Stft, &StftError))
        {
            AddError(FString::Printf(TEXT("%s: STFT failed: %s %s"), Label, *StftError.Code, *StftError.Message));
            return 0.0;
        }

        // Frame 2 covers samples 2048..6144, comfortably inside the note and clear of its edge.
        const int32 Bin = PeakBin(Stft, 2);
        return static_cast<double>(Bin) * static_cast<double>(Stft.BinHz);
    };

    const double MeasuredA4 = MeasureFundamental(Score, TEXT("midi 69"));
    TestTrue(FString::Printf(TEXT("midi 69 renders at 440 Hz (measured %.2f Hz)"), MeasuredA4),
        FMath::Abs(MeasuredA4 - 440.0) < 18.0);

    // An octave up. This is what separates "the renderer placed the pitch" from "the instrument
    // happened to be tuned to the note already": the instrument still says 440.
    Score.Tracks[0].Notes[0].Midi = 81;
    const double MeasuredA5 = MeasureFundamental(Score, TEXT("midi 81"));
    TestTrue(FString::Printf(TEXT("midi 81 renders at 880 Hz (measured %.2f Hz)"), MeasuredA5),
        FMath::Abs(MeasuredA5 - 880.0) < 18.0);

    return true;
}

// =========================================================================
// Timing: the note lands on the exact sample, not near it
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwMusicRenderNoteStartFrameTest,
    "PinWright.audio.music.render.NoteStartsOnTheExactFrame",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwMusicRenderNoteStartFrameTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicRenderTests;

    // Beat 4 of a 4/4 score at 120 bpm is 4 * 60000/120 = 2000.0 ms, which at 48 kHz is frame
    // 96000 exactly. Asserted on the sample index, not on a duration in milliseconds: a
    // renderer that is one frame out is inaudible and still wrong, and only the index says so.
    constexpr int32 ExpectedStartFrame = 96000;

    // Phase 0.25 turns puts the sine at its peak on its own first sample, so "the note has
    // started" is observable on the very first frame rather than a quarter cycle later.
    const FString Json = ScoreJson(120.0, 2,
        ExplicitTrack(TEXT("lead"), TEXT("lead"), 4,
            SteadyOscInstrument(TEXT("sine"), 0.25), MidiNote(4.0, 1.0, 69)));

    FPwMusicScore Score;
    FString ParseError;
    if (!BuildScore(Json, Score, ParseError))
    {
        AddError(FString::Printf(TEXT("fixture did not parse: %s"), *ParseError));
        return false;
    }

    FPwMusicRenderReport Report;
    FString Code;
    FString Error;
    if (!PwRenderScoreStems(Score, Report, Code, Error))
    {
        AddError(FString::Printf(TEXT("render failed: %s %s"), *Code, *Error));
        return false;
    }

    TestEqual(TEXT("two bars of 4/4 at 120 bpm is 192000 frames"),
        Report.Stems[0].Buffer.NumFrames(), 192000);

    const TArray<float>& Left = Report.Stems[0].Buffer.Left;

    // Everything before the note is EXACTLY zero - the stem was zero-initialized and nothing was
    // mixed there. An approximate assertion would pass a renderer that smeared the attack.
    int32 FirstNonZero = INDEX_NONE;
    for (int32 Frame = 0; Frame < Left.Num(); ++Frame)
    {
        if (Left[Frame] != 0.f)
        {
            FirstNonZero = Frame;
            break;
        }
    }

    TestEqual(TEXT("the note's first sample is frame 96000"), FirstNonZero, ExpectedStartFrame);
    // `== 0.f`, not TestEqual's KINDA_SMALL_NUMBER tolerance: the claim is that nothing at all
    // was written there, and a tolerance would accept a renderer that smeared the attack.
    TestTrue(TEXT("the frame before it is exactly zero"), Left[ExpectedStartFrame - 1] == 0.f);
    TestTrue(TEXT("the note's first sample carries the oscillator's peak"),
        FMath::Abs(Left[ExpectedStartFrame]) > 0.5f);

    return true;
}

// =========================================================================
// The seamless loop - the point of the chunk
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwMusicRenderSeamlessLoopTest,
    "PinWright.audio.music.render.LoopSeamHasNoDiscontinuity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwMusicRenderSeamlessLoopTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicRenderTests;

    // One bar of 4/4 at 120 bpm = 2000 ms = 96000 frames. The single note starts at beat 3
    // (1500 ms, frame 72000) and its envelope releases over 1500 ms, so its voice runs to frame
    // 144000 - 48000 frames PAST the loop point. There is deliberately NO note at beat 0, so
    // every sample in the head of the stem can only have arrived there by wrapping.
    const FString Json = ScoreJson(120.0, 1,
        ExplicitTrack(TEXT("bell"), TEXT("lead"), 4, DecayingOscInstrument(1500.0),
            MidiNote(3.0, 1.0, 69)));

    FPwMusicScore Score;
    FString ParseError;
    if (!BuildScore(Json, Score, ParseError))
    {
        AddError(FString::Printf(TEXT("fixture did not parse: %s"), *ParseError));
        return false;
    }

    FPwMusicRenderReport Report;
    FString Code;
    FString Error;
    if (!PwRenderScoreStems(Score, Report, Code, Error))
    {
        AddError(FString::Printf(TEXT("render failed: %s %s"), *Code, *Error));
        return false;
    }

    const FPwAudioBuffer& Stem = Report.Stems[0].Buffer;
    const int32 LoopFrames = Stem.NumFrames();
    TestEqual(TEXT("the stem is exactly one loop long"), LoopFrames, 96000);
    if (LoopFrames != 96000)
    {
        return false;
    }

    // 1. THE WRAP HAPPENED AT ALL. No note starts before beat 3, so a truncating renderer leaves
    //    the head digitally silent - and so does a fading one, which removes the discontinuity by
    //    removing the audio rather than by moving the tail where it belongs.
    const float HeadPeak = MaxAbs(Stem.Left, 0, 4096);
    TestTrue(FString::Printf(TEXT("the head carries the wrapped tail (peak %.4f)"), HeadPeak),
        HeadPeak > 0.05f);

    // 2. THE JOIN IS CONTINUOUS. Two copies back to back is what a looping player produces, and
    //    the step across the join must be no larger than the steps inside the body.
    TArray<float> Looped;
    Looped.Reserve(LoopFrames * 2);
    Looped.Append(Stem.Left);
    Looped.Append(Stem.Left);

    // The body's own bound, measured with the join excluded from both directions.
    const float BodyMaxDelta = MaxDelta(Stem.Left, 1, LoopFrames - 1);
    const float SeamDelta = FMath::Abs(Looped[LoopFrames] - Looped[LoopFrames - 1]);
    TestTrue(FString::Printf(TEXT("the seam step %.6f is within the body's %.6f bound"),
            SeamDelta, BodyMaxDelta),
        SeamDelta <= BodyMaxDelta);

    // Not just the one sample either: every step in a window straddling the join.
    const float SeamWindowDelta = MaxDelta(Looped, LoopFrames - 64, 128);
    TestTrue(FString::Printf(TEXT("every step across the join (%.6f) is within the body's %.6f bound"),
            SeamWindowDelta, BodyMaxDelta),
        SeamWindowDelta <= BodyMaxDelta);

    // 3. NOTHING WAS DUCKED. A boundary fade is continuous precisely because it walks the signal
    //    to zero, so the level on both sides of the join has to be checked as well as the step.
    //    256 frames is over two cycles at 440 Hz, so the window is guaranteed to see a crest.
    const float BeforeSeamPeak = MaxAbs(Stem.Left, LoopFrames - 256, 256);
    const float AfterSeamPeak = MaxAbs(Stem.Left, 0, 256);
    TestTrue(FString::Printf(TEXT("the level before the join is not faded (peak %.4f)"), BeforeSeamPeak),
        BeforeSeamPeak > 0.1f);
    TestTrue(FString::Printf(TEXT("the level after the join is not faded (peak %.4f)"), AfterSeamPeak),
        AfterSeamPeak > 0.1f);

    return true;
}

// =========================================================================
// Loop exactness is reported, never rounded away
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwMusicRenderLoopExactnessTest,
    "PinWright.audio.music.render.LoopExactnessIsReported",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwMusicRenderLoopExactnessTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicRenderTests;

    const auto RenderLoop = [this](double Bpm, FPwScoreLoop& OutLoop, FPwMusicScore& OutScore) -> bool
    {
        const FString Json = ScoreJson(Bpm, 1,
            ExplicitTrack(TEXT("lead"), TEXT("lead"), 4, SteadyOscInstrument(), MidiNote(0.0, 1.0, 69)));

        FString ParseError;
        if (!BuildScore(Json, OutScore, ParseError))
        {
            AddError(FString::Printf(TEXT("fixture at %.4f bpm did not parse: %s"), Bpm, *ParseError));
            return false;
        }

        FPwMusicRenderReport Report;
        FString Code;
        FString Error;
        if (!PwRenderScoreStems(OutScore, Report, Code, Error))
        {
            AddError(FString::Printf(TEXT("render at %.4f bpm failed: %s %s"), Bpm, *Code, *Error));
            return false;
        }

        // The report carries PwScoreLoopLength's answer through untouched - the renderer is not
        // allowed to smooth over an inexact loop on the way out.
        OutLoop = Report.Loop;
        TestEqual(FString::Printf(TEXT("%.4f bpm: the stem is exactly Loop.Frames long"), Bpm),
            static_cast<int64>(Report.Stems[0].Buffer.NumFrames()), OutLoop.Frames);
        return true;
    };

    // 1 bar of 4/4 at 120 bpm and 48 kHz: 4 * 60 * 48000 / 120 = 96000 frames, on the nose.
    FPwScoreLoop Exact;
    FPwMusicScore ExactScore;
    if (RenderLoop(120.0, Exact, ExactScore))
    {
        TestTrue(TEXT("120 bpm is an exact loop"), Exact.bExact);
        TestEqual(TEXT("120 bpm rounds nothing"), Exact.ResidualFrames, 0.0);
        TestEqual(TEXT("120 bpm is 96000 frames"), Exact.Frames, static_cast<int64>(96000));
    }

    // 140 bpm: 11520000 / 140 = 82285.714..., which no amount of rounding makes whole.
    FPwScoreLoop Awkward;
    FPwMusicScore AwkwardScore;
    if (RenderLoop(140.0, Awkward, AwkwardScore))
    {
        TestFalse(TEXT("140 bpm is not an exact loop"), Awkward.bExact);
        TestTrue(FString::Printf(TEXT("140 bpm reports a non-zero residual (%.6f frames)"),
                Awkward.ResidualFrames),
            FMath::Abs(Awkward.ResidualFrames) > 0.0);
        TestTrue(TEXT("140 bpm suggests a tempo"), Awkward.NearestExactBpm > 0.0);

        // The suggestion has to WORK - the whole reason for publishing it is that the caller can
        // substitute it and get an exact loop, so the test substitutes it.
        FPwMusicScore Substituted = AwkwardScore;
        Substituted.Bpm = Awkward.NearestExactBpm;
        const FPwScoreLoop Fixed = PwScoreLoopLength(Substituted);
        TestTrue(FString::Printf(TEXT("NearestExactBpm %.6f is exact when substituted"),
                Awkward.NearestExactBpm),
            Fixed.bExact);
        TestEqual(TEXT("the suggested tempo keeps the same frame count"), Fixed.Frames, Awkward.Frames);
    }

    return true;
}

// =========================================================================
// Stems and their mixdown
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwMusicRenderMixStemsTest,
    "PinWright.audio.music.render.MixedStemsEqualTheirSum",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwMusicRenderMixStemsTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicRenderTests;

    const FString Tracks = ExplicitTrack(TEXT("bass"), TEXT("bass"), 2,
            SteadyOscInstrument(TEXT("sine")), MidiNote(0.0, 2.0, 45), -6.0, -0.5)
        + TEXT(",")
        + ExplicitTrack(TEXT("lead"), TEXT("lead"), 4,
            SteadyOscInstrument(TEXT("saw")), MidiNote(1.0, 2.0, 69), 0.0, 0.5);

    FPwMusicScore Score;
    FString ParseError;
    if (!BuildScore(ScoreJson(120.0, 1, Tracks), Score, ParseError))
    {
        AddError(FString::Printf(TEXT("fixture did not parse: %s"), *ParseError));
        return false;
    }

    FPwMusicRenderReport Report;
    FString Code;
    FString Error;
    if (!PwRenderScoreStems(Score, Report, Code, Error))
    {
        AddError(FString::Printf(TEXT("render failed: %s %s"), *Code, *Error));
        return false;
    }

    TestEqual(TEXT("two tracks render two stems"), Report.Stems.Num(), 2);
    if (Report.Stems.Num() != 2)
    {
        return false;
    }

    TestEqual(TEXT("stems are named after their tracks"), Report.Stems[0].TrackName, FString(TEXT("bass")));
    TestEqual(TEXT("stems are named after their tracks"), Report.Stems[1].TrackName, FString(TEXT("lead")));
    TestEqual(TEXT("both notes were counted"), Report.TotalNotes, 2);

    // The panned tracks must actually be panned - a stem that ignored track.pan would put equal
    // energy in both channels and the mixdown assertion below would still pass.
    TestTrue(TEXT("the left-panned stem is louder on the left"),
        MaxAbs(Report.Stems[0].Buffer.Left, 0, Report.Stems[0].Buffer.NumFrames())
            > MaxAbs(Report.Stems[0].Buffer.Right, 0, Report.Stems[0].Buffer.NumFrames()));

    FPwAudioBuffer Mix;
    FString MixCode;
    FString MixError;
    if (!PwMixStems(Report.Stems, Mix, MixCode, MixError))
    {
        AddError(FString::Printf(TEXT("mixdown failed: %s %s"), *MixCode, *MixError));
        return false;
    }

    TestEqual(TEXT("the mixdown is as long as the stems"), Mix.NumFrames(), Report.Stems[0].Buffer.NumFrames());
    TestEqual(TEXT("the mixdown keeps the sample rate"), Mix.SampleRate, TestSampleRate);

    double WorstError = 0.0;
    for (int32 Frame = 0; Frame < Mix.NumFrames(); ++Frame)
    {
        const double ExpectedLeft = static_cast<double>(Report.Stems[0].Buffer.Left[Frame])
            + static_cast<double>(Report.Stems[1].Buffer.Left[Frame]);
        const double ExpectedRight = static_cast<double>(Report.Stems[0].Buffer.Right[Frame])
            + static_cast<double>(Report.Stems[1].Buffer.Right[Frame]);
        WorstError = FMath::Max(WorstError, FMath::Abs(ExpectedLeft - static_cast<double>(Mix.Left[Frame])));
        WorstError = FMath::Max(WorstError, FMath::Abs(ExpectedRight - static_cast<double>(Mix.Right[Frame])));
    }

    TestTrue(FString::Printf(TEXT("the mixdown is the per-sample sum (worst error %.9f)"), WorstError),
        WorstError < 1e-6);

    return true;
}

// =========================================================================
// Determinism
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwMusicRenderDeterminismTest,
    "PinWright.audio.music.render.SameScoreRendersByteIdentical",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwMusicRenderDeterminismTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicRenderTests;

    // Track 0 is a NOISE instrument, which is what makes this test worth running: an
    // all-oscillator score renders identically even with a broken RNG. Its notes sit at degree 0
    // because `noise` has no pitch for an off-tonic transposition to act on and says so.
    const FString NoiseInstrument = TEXT(R"({"generator":{"kind":"noise","params":{"color":"pink"}}})");
    const FString PercNotes = DegreeNote(0.0, 1.0, 0) + TEXT(",") + DegreeNote(2.0, 1.0, 0);

    const auto ScoreWithLeadWave = [&NoiseInstrument, &PercNotes](const TCHAR* LeadWave) -> FString
    {
        return ScoreJson(120.0, 1,
            ExplicitTrack(TEXT("perc"), TEXT("percussion"), 4, NoiseInstrument, PercNotes)
            + TEXT(",")
            + ExplicitTrack(TEXT("lead"), TEXT("lead"), 4, SteadyOscInstrument(LeadWave),
                MidiNote(0.0, 2.0, 69)));
    };

    const auto Render = [this](const FString& Json, FPwMusicRenderReport& OutReport, const TCHAR* Label) -> bool
    {
        FPwMusicScore Score;
        FString ParseError;
        if (!BuildScore(Json, Score, ParseError))
        {
            AddError(FString::Printf(TEXT("%s did not parse: %s"), Label, *ParseError));
            return false;
        }

        FString Code;
        FString Error;
        if (!PwRenderScoreStems(Score, OutReport, Code, Error))
        {
            AddError(FString::Printf(TEXT("%s failed to render: %s %s"), Label, *Code, *Error));
            return false;
        }
        return true;
    };

    FPwMusicRenderReport First;
    FPwMusicRenderReport Second;
    if (!Render(ScoreWithLeadWave(TEXT("saw")), First, TEXT("first render"))
        || !Render(ScoreWithLeadWave(TEXT("saw")), Second, TEXT("second render")))
    {
        return false;
    }

    TestEqual(TEXT("both renders produced two stems"), First.Stems.Num(), 2);
    TestEqual(TEXT("both renders produced two stems"), Second.Stems.Num(), 2);
    if (First.Stems.Num() != 2 || Second.Stems.Num() != 2)
    {
        return false;
    }

    TestTrue(TEXT("the same score and seed renders the noise stem byte-identically"),
        StemsAreByteIdentical(First.Stems[0], Second.Stems[0]));
    TestTrue(TEXT("the same score and seed renders the lead stem byte-identically"),
        StemsAreByteIdentical(First.Stems[1], Second.Stems[1]));

    // The substream property: the seed is derived per track index and then per note index, and
    // Derive() advances nothing, so editing track 1 cannot move a single sample of track 0.
    // Threading one advancing stream through the whole score would look equivalent and break
    // exactly this.
    FPwMusicRenderReport Edited;
    if (!Render(ScoreWithLeadWave(TEXT("square")), Edited, TEXT("edited render")))
    {
        return false;
    }

    TestTrue(TEXT("editing track 1 leaves track 0 byte-identical"),
        StemsAreByteIdentical(First.Stems[0], Edited.Stems[0]));
    TestFalse(TEXT("editing track 1 does change track 1"),
        StemsAreByteIdentical(First.Stems[1], Edited.Stems[1]));

    return true;
}

// =========================================================================
// Failure direction (rpc-design.md §12)
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwMusicRenderFailureDirectionTest,
    "PinWright.audio.music.render.FailuresAbortWithoutPublishingStems",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwMusicRenderFailureDirectionTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicRenderTests;

    const FString Json = ScoreJson(120.0, 1,
        ExplicitTrack(TEXT("lead"), TEXT("lead"), 4, SteadyOscInstrument(), MidiNote(0.0, 1.0, 69)));

    FPwMusicScore Valid;
    FString ParseError;
    if (!BuildScore(Json, Valid, ParseError))
    {
        AddError(FString::Printf(TEXT("fixture did not parse: %s"), *ParseError));
        return false;
    }

    // 1. A sample rate the instrument kernel cannot render. The score and its instruments must
    //    share one rate: FPwAudioBuffer::MixInto no-ops on a mismatch and returns 0, so a stem
    //    at the wrong rate is silence with every stage reporting success.
    {
        FPwMusicScore Score = Valid;
        Score.SampleRate = PwSynthLimits::MinSampleRate / 2;

        FPwMusicRenderReport Report = SentinelReport();
        FString Code;
        FString Error;
        const bool bRendered = PwRenderScoreStems(Score, Report, Code, Error);
        CheckAbortedRender(*this, TEXT("a sample rate outside the kernel's range"),
            bRendered, Report, Code, Error, ErrorCodes::ERR_INVALID_RECIPE);
        TestTrue(TEXT("the sample-rate error names the field and the kernel's range: ") + Error,
            Error.Contains(TEXT("sampleRate")) && Error.Contains(TEXT("8000")));
    }

    // 2. No tracks at all. An empty set is an error rather than a successful render of zero
    //    stems, for the same reason PwRenderRecipe rejects a layerless recipe.
    {
        FPwMusicScore Score = Valid;
        Score.Tracks.Empty();

        FPwMusicRenderReport Report = SentinelReport();
        FString Code;
        FString Error;
        const bool bRendered = PwRenderScoreStems(Score, Report, Code, Error);
        CheckAbortedRender(*this, TEXT("an empty track list"),
            bRendered, Report, Code, Error, ErrorCodes::ERR_AUDIO_EMPTY_BUFFER);
    }

    // 3. A note past the score's end. It would land in the NEXT repetition of the loop, i.e. on
    //    top of the first bar, which is a score defect and not something to fold in quietly.
    {
        FPwMusicScore Score = Valid;
        Score.Tracks[0].Notes[0].StartBeat = 100.0;    // the score is one bar of 4/4 = 4 beats

        FPwMusicRenderReport Report = SentinelReport();
        FString Code;
        FString Error;
        const bool bRendered = PwRenderScoreStems(Score, Report, Code, Error);
        CheckAbortedRender(*this, TEXT("a note past the score's end"),
            bRendered, Report, Code, Error, ErrorCodes::ERR_INVALID_RECIPE);
        TestTrue(TEXT("the note error names the note: ") + Error, Error.Contains(TEXT("note 0")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwMusicRenderMixFailureTest,
    "PinWright.audio.music.render.MixStemsRefusesRatherThanDroppingAStem",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwMusicRenderMixFailureTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicRenderTests;

    const auto MakeStem = [](const TCHAR* Name, int32 SampleRate, int32 Frames) -> FPwStemResult
    {
        FPwStemResult Stem;
        Stem.TrackName = Name;
        Stem.Buffer.SampleRate = SampleRate;
        Stem.Buffer.SetNumFrames(Frames);
        for (int32 Frame = 0; Frame < Frames; ++Frame)
        {
            Stem.Buffer.Left[Frame] = 0.25f;
            Stem.Buffer.Right[Frame] = 0.25f;
        }
        Stem.bMeasured = true;
        return Stem;
    };

    // Empty set.
    {
        FPwAudioBuffer Out;
        FString Code;
        FString Error;
        TestFalse(TEXT("an empty stem set fails"), PwMixStems({}, Out, Code, Error));
        TestEqual(TEXT("an empty stem set reports AUDIO_EMPTY_BUFFER"), Code,
            FString(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER));
        TestEqual(TEXT("an empty stem set publishes no buffer"), Out.NumFrames(), 0);
    }

    // Rate mismatch. MixInto would silently return 0 and the stem would vanish from the mixdown.
    {
        TArray<FPwStemResult> Stems;
        Stems.Add(MakeStem(TEXT("a"), 48000, 256));
        Stems.Add(MakeStem(TEXT("b"), 44100, 256));

        FPwAudioBuffer Out;
        FString Code;
        FString Error;
        TestFalse(TEXT("mismatched sample rates fail"), PwMixStems(Stems, Out, Code, Error));
        TestEqual(TEXT("mismatched sample rates report INVALID_PARAMS"), Code,
            FString(ErrorCodes::ERR_INVALID_PARAMS));
        TestTrue(TEXT("the message names both rates: ") + Error,
            Error.Contains(TEXT("44100")) && Error.Contains(TEXT("48000")));
        TestEqual(TEXT("nothing is published on failure"), Out.NumFrames(), 0);
    }

    // Ordering: a NaN stem beside an EMPTY stem must report the NaN. NaN compares false against
    // every threshold, so a buffer full of it scans to a peak of zero and would be reported as
    // the smaller, more plausible-sounding "this stem is silent/empty" unless it is named first
    // (rpc-design.md §7).
    {
        TArray<FPwStemResult> Stems;
        FPwStemResult Poisoned = MakeStem(TEXT("poisoned"), 48000, 256);
        Poisoned.Buffer.Left[100] = MakeNaN();
        Stems.Add(MoveTemp(Poisoned));
        Stems.Add(MakeStem(TEXT("empty"), 48000, 0));

        FPwAudioBuffer Out;
        FString Code;
        FString Error;
        TestFalse(TEXT("a non-finite stem fails"), PwMixStems(Stems, Out, Code, Error));
        TestEqual(TEXT("non-finite is named before emptiness"), Code,
            FString(ErrorCodes::ERR_AUDIO_NON_FINITE_SAMPLES));
        TestTrue(TEXT("the message names the offending stem: ") + Error,
            Error.Contains(TEXT("poisoned")));
    }

    return true;
}

// =========================================================================
// Track level: gainDb reaches the stem, at the right magnitude and the right sign
//
// Every other test in this file leaves `gainDb` (and `pan`) at the ExplicitTrack default of 0,
// which makes PwMusicRender.cpp's track-level block DEAD CODE under the suite:
//
//     const float TrackGain = static_cast<float>(DbToLinear(Track.GainDb));
//     if (TrackGain != 1.f) { ...scale the finished stem... }
//
// At gainDb 0 the guard is always false, so deleting the whole block — or inverting the sign, or
// treating the value as linear rather than dB — changed nothing any assertion could see. A track
// level silently ignored is not a subtle defect: every mix balance an agent authors is discarded
// and the failure looks like the renderer, not the level.
//
// -6.0206 dB is exactly half amplitude (20*log10(0.5)), so the expectation is arithmetic rather
// than measured, and DIRECTIONAL: a sign inversion doubles instead of halving and lands ~4x away
// from the tolerance. The peak is read off the samples rather than off the report's PeakDb so the
// assertion does not depend on the same MeasurePeakDb call the report is built from.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwMusicRenderTrackGainTest,
    "PinWright.audio.music.render.TrackGainScalesTheStem",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwMusicRenderTrackGainTest::RunTest(const FString& Parameters)
{
    using namespace PwMusicRenderTests;

    // Half amplitude, to four decimals. Named so the relationship to 0.5 is not a magic number.
    constexpr double HalfAmplitudeDb = -6.0206;
    constexpr float ExpectedRatio = 0.5f;

    const auto RenderPeak = [this](double GainDb, const TCHAR* Label, float& OutPeak) -> bool
    {
        const FString Json = ScoreJson(120.0, 1,
            ExplicitTrack(TEXT("lead"), TEXT("lead"), 4, SteadyOscInstrument(),
                MidiNote(0.0, 2.0, 69), GainDb));

        FPwMusicScore Score;
        FString ParseError;
        if (!BuildScore(Json, Score, ParseError))
        {
            AddError(FString::Printf(TEXT("%s: fixture did not parse: %s"), Label, *ParseError));
            return false;
        }

        FPwMusicRenderReport Report;
        FString Code;
        FString Error;
        if (!PwRenderScoreStems(Score, Report, Code, Error))
        {
            AddError(FString::Printf(TEXT("%s: render failed: %s %s"), Label, *Code, *Error));
            return false;
        }
        TestEqual(FString::Printf(TEXT("%s: one stem"), Label), Report.Stems.Num(), 1);
        if (Report.Stems.Num() != 1)
        {
            return false;
        }

        const FPwStemResult& Stem = Report.Stems[0];
        TestEqual(FString::Printf(TEXT("%s: the note reached the stem"), Label),
            Stem.NotesRendered, 1);
        OutPeak = MaxAbs(Stem.Buffer.Left, 0, Stem.Buffer.Left.Num());
        return true;
    };

    float UnityPeak = 0.f;
    float HalvedPeak = 0.f;
    if (!RenderPeak(0.0, TEXT("gainDb 0"), UnityPeak) ||
        !RenderPeak(HalfAmplitudeDb, TEXT("gainDb -6.02"), HalvedPeak))
    {
        return false;
    }

    // Without this the ratio below is 0/0 and every conclusion from it is vacuous.
    if (!TestTrue(FString::Printf(TEXT("the unity-gain stem carries signal (peak %.6f)"), UnityPeak),
            UnityPeak > 0.01f))
    {
        return false;
    }

    const float Ratio = HalvedPeak / UnityPeak;
    AddInfo(FString::Printf(TEXT("peak at gainDb 0: %.6f; at %.4f dB: %.6f; ratio %.6f."),
        UnityPeak, HalfAmplitudeDb, HalvedPeak, Ratio));

    // Direction first: a sign inversion produces a ratio of ~2, so this fails before the
    // magnitude assertion has to distinguish it.
    TestTrue(FString::Printf(TEXT("a negative gainDb makes the stem QUIETER (ratio %.4f)"), Ratio),
        Ratio < 1.f);
    // ...then magnitude. 2% absorbs float rounding while excluding a linear reading of the field
    // (-6.0206 as a linear factor would silently invert the phase, not halve the level) and any
    // constant-factor error.
    TestTrue(FString::Printf(TEXT("-6.0206 dB is exactly half amplitude (ratio %.4f, want %.4f)"),
        Ratio, ExpectedRatio), FMath::IsNearlyEqual(Ratio, ExpectedRatio, 0.02f));

    return true;
}
