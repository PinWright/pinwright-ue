// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwMusicRender.cpp - the score render loop and the stem mixdown.
//
// Three properties this file exists to hold; the reasoning for each is in PwMusicRender.h:
//
//   NO NOTE IS SILENTLY LOST. FPwAudioBuffer::MixInto returns the frames it actually mixed and
//   returns 0 on a sample-rate mismatch, so every mix here reads that return and a note that
//   contributed nothing is counted into FPwStemResult::NotesDropped rather than looking
//   identical to one that contributed (rpc-design.md §1).
//
//   THE LOOP IS SEAMLESS BY CONSTRUCTION. Everything past the loop point is folded back onto
//   the head modulo the loop length, which is precisely one period of the score repeated
//   forever. Nothing is faded and nothing is truncated.
//
//   DETERMINISM. FPwSeededRandom(Score.Seed).Derive(TrackIndex).Derive(NoteIndex), a pure hash
//   chain that advances nothing, exactly mirroring PwRenderRecipe's Root.Derive(LayerIndex).

#include "AudioGen/PwMusicRender.h"

#include "AudioGen/PwSynthDsp.h"
#include "Handlers/ErrorCodes.h"
#include "PinWrightSubsystem.h"

// Named (not anonymous) namespace: the module builds with bUseUnity = true, so an anonymous
// namespace here would still collide with a sibling translation unit's helper of the same name.
namespace PwMusicRenderInternal
{
    constexpr double Ln10 = 2.30258509299404568402;

    // Upper bound on the echo count a delay's tail estimate is allowed to claim. A feedback of
    // 0.99 needs 688 repeats to fall 60 dB, and at the schema's 5000 ms maximum delay time that
    // is a 57-minute "tail" for one note. The bound keeps the estimate finite; the voice-length
    // clamp below is what keeps it renderable.
    constexpr double MaxDelayRepeats = 256.0;

    // -----------------------------------------------------------------------------------
    // Scalar helpers. Deliberately identical in form to PwSynthDspInternal's, because a stem
    // frame index and a recipe frame index have to agree exactly or a note lands one sample
    // away from where the score put it.
    // -----------------------------------------------------------------------------------

    // Rounds rather than truncates, matching PwSynthDspInternal::MsToFrames, and returns int64
    // so an out-of-range score is detected instead of wrapping into a small positive count.
    int64 MsToFrames64(double Ms, int32 SampleRate)
    {
        if (!FMath::IsFinite(Ms))
        {
            return 0;
        }
        const double Frames = FMath::RoundToDouble(Ms * static_cast<double>(SampleRate) / 1000.0);
        return static_cast<int64>(FMath::Clamp(Frames,
            static_cast<double>(MIN_int64 / 2), static_cast<double>(MAX_int64 / 2)));
    }

    double DbToLinear(double Db)
    {
        return FMath::Pow(10.0, Db / 20.0);
    }

    // Zero and negative amplitudes report the same documented floor the recipe renderer uses,
    // so a silent stem and a silent SFX render read alike.
    double LinearToDb(double Linear)
    {
        return (Linear > 0.0)
            ? 20.0 * FMath::Loge(Linear) / Ln10
            : PwSynthRender::SilenceFloorDb;
    }

    // -----------------------------------------------------------------------------------
    // Parameter writes, range-checked against the generator's OWN spec row
    // -----------------------------------------------------------------------------------

    const FPwSynthParamSpec* FindParamSpec(const FPwSynthKindSpec& Kind, FName Name)
    {
        if (Kind.Params == nullptr)
        {
            return nullptr;
        }
        for (const FPwSynthParamSpec& Spec : *Kind.Params)
        {
            if (Spec.Name == Name)
            {
                return &Spec;
            }
        }
        return nullptr;
    }

    // Writes a number into a parameter bag after checking it against the kind's published
    // range. The range is READ FROM THE SPEC TABLE rather than copied here, so a table edit
    // cannot leave a second, stale bound behind in this file.
    bool SetNumberParam(FPwSynthParams& Params, const FPwSynthKindSpec& Kind, FName Name,
        double Value, FString& OutErrorCode, FString& OutError)
    {
        const FPwSynthParamSpec* Spec = FindParamSpec(Kind, Name);
        if (Spec == nullptr)
        {
            OutErrorCode = ErrorCodes::ERR_UNKNOWN_GENERATOR;
            OutError = FString::Printf(
                TEXT("generator '%s' has no '%s' parameter for a note's pitch to land in, so the ")
                TEXT("note would render at the instrument's own pitch instead of its own."),
                Kind.Name, *Name.ToString());
            return false;
        }

        if (!FMath::IsFinite(Value)
            || (Spec->bHasMin && Value < Spec->Min)
            || (Spec->bHasMax && Value > Spec->Max))
        {
            const FString MinText = Spec->bHasMin ? FString::SanitizeFloat(Spec->Min) : FString(TEXT("-inf"));
            const FString MaxText = Spec->bHasMax ? FString::SanitizeFloat(Spec->Max) : FString(TEXT("+inf"));

            OutErrorCode = ErrorCodes::ERR_INVALID_PARAMS;
            OutError = FString::Printf(
                TEXT("the note's pitch is %.4f Hz, outside the %s..%s range generator '%s' accepts ")
                TEXT("for '%s'. Move the note into the instrument's register or change the ")
                TEXT("instrument - clamping it would play a pitch the score never wrote."),
                Value, *MinText, *MaxText, Kind.Name, *Name.ToString());
            return false;
        }

        FPwSynthParamValue Written;
        Written.Type = EPwSynthParamType::Number;
        Written.Number = Value;
        Params.Values.Add(Name, Written);
        return true;
    }

    // -----------------------------------------------------------------------------------
    // Pitch placement
    // -----------------------------------------------------------------------------------

    // True for the kinds whose pitch is carried by a constant pitchEnvelope offset rather than
    // by an absolute frequency parameter. See the table in PwMusicRender.h.
    bool UsesPitchEnvelope(EPwSynthGeneratorKind Kind)
    {
        return Kind == EPwSynthGeneratorKind::Modal
            || Kind == EPwSynthGeneratorKind::Sample
            || Kind == EPwSynthGeneratorKind::Granular
            || Kind == EPwSynthGeneratorKind::Noise;
    }

    // Which of those reference the TRACK'S TONIC rather than something inside the instrument.
    // `modal` measures against its own modeFreqsHz[0], so resolving a tonic for a modal track
    // would let an out-of-MIDI octave abort a render that never needed the value.
    bool NeedsTrackTonic(EPwSynthGeneratorKind Kind)
    {
        return UsesPitchEnvelope(Kind) && Kind != EPwSynthGeneratorKind::Modal;
    }

    // Semitone offset a pitchEnvelope-carried generator needs for this note.
    //
    // `modal` references its own bank: mode 0 is taken as the instrument's fundamental, so the
    // offset lands mode 0 exactly on the note and every mode RATIO - the timbre - survives.
    // Everything else references the TRACK'S TONIC, because a recorded sample and a noise burst
    // carry no knowable pitch and the tonic is the only reference the score does define.
    bool NoteSemitoneOffset(const FPwSynthLayer& Instrument, int32 NoteMidi, int32 TonicMidi,
        double NoteFrequencyHz, double& OutSemitones, FString& OutErrorCode, FString& OutError)
    {
        if (Instrument.Generator.Kind != EPwSynthGeneratorKind::Modal)
        {
            OutSemitones = static_cast<double>(NoteMidi - TonicMidi);
            return true;
        }

        const TArray<double>* Modes = Instrument.Generator.Params.GetNumbers(FName(TEXT("modeFreqsHz")));
        if (Modes == nullptr || Modes->Num() == 0 || !((*Modes)[0] > 0.0))
        {
            OutErrorCode = ErrorCodes::ERR_INVALID_PARAMS;
            OutError = TEXT("the 'modal' instrument carries no positive modeFreqsHz[0], so there is ")
                       TEXT("no fundamental for the note's pitch to be measured against.");
            return false;
        }

        // 12 * log2(target / fundamental). Written out rather than via FMath::Log2, which is a
        // float overload and would round a cent-level offset.
        OutSemitones = 12.0 * FMath::Loge(NoteFrequencyHz / (*Modes)[0]) / FMath::Loge(2.0);
        return true;
    }

    // Rewrites `Layer` so it sounds at this note's pitch. The layer arrives as a copy of the
    // track's instrument with its ORIGINAL pitch envelope, so the offset is applied fresh every
    // note and never accumulates.
    bool ApplyNotePitch(FPwSynthLayer& Layer, int32 NoteMidi, int32 TonicMidi,
        FString& OutErrorCode, FString& OutError)
    {
        const EPwSynthGeneratorKind Kind = Layer.Generator.Kind;
        const FPwSynthKindSpec& Spec = PwSynthGeneratorSpec(Kind);
        const double FrequencyHz = PwScoreMidiToFrequencyHz(static_cast<double>(NoteMidi));

        if (Kind == EPwSynthGeneratorKind::Osc)
        {
            return SetNumberParam(Layer.Generator.Params, Spec, FName(TEXT("frequencyHz")),
                FrequencyHz, OutErrorCode, OutError);
        }

        if (Kind == EPwSynthGeneratorKind::Formant)
        {
            return SetNumberParam(Layer.Generator.Params, Spec, FName(TEXT("f0Hz")),
                FrequencyHz, OutErrorCode, OutError);
        }

        if (!UsesPitchEnvelope(Kind))
        {
            // Unspecified / Count, or a kind added to the enum without a row here. Rendering it
            // at the instrument's own pitch would produce a track that plays one note over and
            // over while every stage reported success.
            OutErrorCode = ErrorCodes::ERR_UNKNOWN_GENERATOR;
            OutError = FString::Printf(
                TEXT("generator kind %d has no rule for where a note's pitch goes. Add its row to ")
                TEXT("PwMusicRenderInternal::ApplyNotePitch rather than letting it render unpitched."),
                static_cast<int32>(Kind));
            return false;
        }

        double Semitones = 0.0;
        if (!NoteSemitoneOffset(Layer, NoteMidi, TonicMidi, FrequencyHz, Semitones, OutErrorCode, OutError))
        {
            return false;
        }

        if (Layer.PitchEnvelope.Num() == 0)
        {
            FPwSynthPitchPoint Point;
            Point.TimeMs = 0.0;
            Point.Semitones = Semitones;
            Layer.PitchEnvelope.Add(Point);
        }
        else
        {
            // The instrument's own envelope is an ARTICULATION (a drop, a scoop) and stays
            // exactly as authored; the note's pitch is a constant offset added on top of it.
            for (FPwSynthPitchPoint& Point : Layer.PitchEnvelope)
            {
                Point.Semitones += Semitones;
            }
        }
        return true;
    }

    // -----------------------------------------------------------------------------------
    // Voice length
    // -----------------------------------------------------------------------------------

    // Milliseconds the instrument's fx chain keeps ringing after its input stops. Summed from
    // the effects' own parameters rather than guessed at. Three kinds are counted - reverb,
    // delay, and the two modulated delay lines - and every other kind contributes 0. The three
    // places that estimate is knowingly short, stated rather than hidden:
    //   convolve    its tail IS the impulse response's length, which cannot be known without
    //               loading the asset, so it contributes 0 and is truncated at the voice end.
    //   filter      a high-Q resonator rings past its input; the schema publishes Q, not a
    //               decay time, so there is no parameter to derive a length from.
    //   compressor  a slow release changes the level of the tail rather than extending it.
    double InstrumentTailMs(const TArray<FPwSynthFx>& Fx)
    {
        double TailMs = 0.0;
        for (const FPwSynthFx& Effect : Fx)
        {
            switch (Effect.Kind)
            {
            case EPwSynthFxKind::Reverb:
                TailMs += Effect.Params.GetNumber(FName(TEXT("preDelayMs")), 0.0)
                        + Effect.Params.GetNumber(FName(TEXT("decayMs")), 0.0);
                break;

            case EPwSynthFxKind::Delay:
            {
                const double TimeMs = Effect.Params.GetNumber(FName(TEXT("timeMs")), 0.0);
                const double Feedback = Effect.Params.GetNumber(FName(TEXT("feedback")), 0.0);
                // Echoes until the regenerated signal is 60 dB down. feedback 0 is one echo.
                double Repeats = 1.0;
                if (Feedback > 0.0 && Feedback < 1.0)
                {
                    Repeats = FMath::CeilToDouble(-60.0 / (20.0 * FMath::Loge(Feedback) / Ln10));
                }
                TailMs += TimeMs * FMath::Min(Repeats, MaxDelayRepeats);
                break;
            }

            case EPwSynthFxKind::Chorus:
            case EPwSynthFxKind::Flanger:
                // A modulated delay line rings for its own depth and no longer.
                TailMs += Effect.Params.GetNumber(FName(TEXT("depthMs")), 0.0);
                break;

            default:
                break;
            }
        }
        return TailMs;
    }

    // How long one instance of the instrument is rendered for this note.
    //
    // The BODY is the author's statement and is never shortened: it is the note's own duration,
    // extended to the instrument's amp envelope when the envelope runs longer.
    //
    // The TAIL is this file's own estimate and is added ONLY when the amp envelope ends at
    // exactly 0. PwSynthDsp's ApplyAmpEnvelope HOLDS the last point's value forever, so an
    // envelope that ends non-zero (or is absent) never releases the generator and extra frames
    // would be more note rather than a decay.
    bool ComputeVoiceMs(const FPwMusicScore& Score, const FPwSynthLayer& Instrument,
        const FPwMusicNote& Note, double& OutVoiceMs, FString& OutErrorCode, FString& OutError)
    {
        const double BodyMs = PwScoreBeatToMs(Score, Note.DurationBeats);
        if (!FMath::IsFinite(BodyMs) || BodyMs <= 0.0)
        {
            OutErrorCode = ErrorCodes::ERR_INVALID_RECIPE;
            OutError = FString::Printf(
                TEXT("durationBeats %.6f is %.4f ms; a note with no length has no samples to ")
                TEXT("produce and is an error rather than a silent skip."),
                Note.DurationBeats, BodyMs);
            return false;
        }

        double EnvelopeEndMs = 0.0;
        bool bEnvelopeReleases = false;
        if (Instrument.AmpEnvelope.Num() > 0)
        {
            const FPwSynthEnvelopePoint& Last = Instrument.AmpEnvelope.Last();
            EnvelopeEndMs = Last.TimeMs;
            bEnvelopeReleases = (Last.Value == 0.0);
        }

        double VoiceMs = FMath::Max(BodyMs, EnvelopeEndMs);
        if (VoiceMs > PwSynthLimits::MaxDurationMs)
        {
            OutErrorCode = ErrorCodes::ERR_INVALID_RECIPE;
            OutError = FString::Printf(
                TEXT("the note is %.2f ms long, past the %.0f ms longest single voice the synth ")
                TEXT("kernel renders. Shorten the note, raise the tempo, or split it."),
                VoiceMs, PwSynthLimits::MaxDurationMs);
            return false;
        }

        if (bEnvelopeReleases)
        {
            const double TailMs = InstrumentTailMs(Instrument.Fx);
            const double WithTail = VoiceMs + TailMs;
            if (WithTail > PwSynthLimits::MaxDurationMs)
            {
                // Clamping OUR OWN estimate, not the author's note - which is why this is a
                // logged clamp and the body overrun above is an error. A truncated tail can
                // click where it wraps, so it is said out loud rather than absorbed.
                UE_LOG(LogPinWrightSubsystem, Warning,
                    TEXT("PwRenderScoreStems: the instrument's fx tail (%.0f ms) does not fit the ")
                    TEXT("%.0f ms voice limit beside a %.0f ms note; the tail is truncated to %.0f ms ")
                    TEXT("and may click where it wraps."),
                    TailMs, PwSynthLimits::MaxDurationMs, VoiceMs,
                    PwSynthLimits::MaxDurationMs - VoiceMs);
            }
            VoiceMs = FMath::Min(WithTail, static_cast<double>(PwSynthLimits::MaxDurationMs));
        }

        OutVoiceMs = VoiceMs;
        return true;
    }

    // -----------------------------------------------------------------------------------
    // Measurement
    // -----------------------------------------------------------------------------------

    // Peak of a finished buffer, in dBFS, with the non-finite check FIRST.
    //
    // The order is load-bearing and is the reason ERR_AUDIO_NON_FINITE_SAMPLES exists as its own
    // code: NaN compares false against every threshold, so a buffer full of it scans to a peak
    // of 0 and would be reported as digital silence - the smaller, more plausible-sounding
    // failure - unless it is named first (rpc-design.md §7).
    bool MeasurePeakDb(const FPwAudioBuffer& Buffer, const FString& What, double& OutPeakDb,
        FString& OutErrorCode, FString& OutError)
    {
        int32 NonFinite = 0;
        int32 FirstBadFrame = INDEX_NONE;

        const auto ScanChannel = [&NonFinite, &FirstBadFrame](const TArray<float>& Channel)
        {
            for (int32 Frame = 0; Frame < Channel.Num(); ++Frame)
            {
                if (!FMath::IsFinite(Channel[Frame]))
                {
                    ++NonFinite;
                    if (FirstBadFrame == INDEX_NONE || Frame < FirstBadFrame)
                    {
                        FirstBadFrame = Frame;
                    }
                }
            }
        };
        ScanChannel(Buffer.Left);
        ScanChannel(Buffer.Right);

        if (NonFinite > 0)
        {
            OutErrorCode = ErrorCodes::ERR_AUDIO_NON_FINITE_SAMPLES;
            OutError = FString::Printf(
                TEXT("%s carries %d non-finite samples, first at frame %d. One of them poisons ")
                TEXT("every sum and every threshold downstream, so the render stops here rather ")
                TEXT("than handing back a buffer that measures as silence."),
                *What, NonFinite, FirstBadFrame);
            return false;
        }

        double Peak = 0.0;
        for (const float Sample : Buffer.Left)
        {
            Peak = FMath::Max(Peak, static_cast<double>(FMath::Abs(Sample)));
        }
        for (const float Sample : Buffer.Right)
        {
            Peak = FMath::Max(Peak, static_cast<double>(FMath::Abs(Sample)));
        }

        OutPeakDb = LinearToDb(Peak);
        return true;
    }

    // -----------------------------------------------------------------------------------
    // Per-note plan, computed before anything is rendered because the stem's length depends
    // on how far the LAST note runs past the loop point.
    // -----------------------------------------------------------------------------------
    struct FNotePlan
    {
        int32 StartFrame = 0;
        int32 VoiceFrames = 0;
        double VoiceMs = 0.0;
    };
}

bool PwRenderScoreStems(const FPwMusicScore& Score, FPwMusicRenderReport& Out,
                        FString& OutErrorCode, FString& OutError)
{
    using namespace PwMusicRenderInternal;

    // Failure is the default: everything is cleared up front and written only on the single
    // success path at the bottom, so a partial render is never observable (rpc-design.md §1).
    Out = FPwMusicRenderReport();
    OutErrorCode.Reset();
    OutError.Reset();

    const auto Fail = [&Out, &OutError](const FString& Where) -> bool
    {
        Out = FPwMusicRenderReport();
        OutError = Where + TEXT(": ") + OutError;
        return false;
    };

    // The score and the instrument kernel have to agree about the sample rate, and this is the
    // only place they can disagree: every buffer below is created at Score.SampleRate, and
    // FPwAudioBuffer::MixInto no-ops on a mismatch, so a rate the kernel cannot render would
    // produce a stem of digital silence with every stage reporting success.
    if (Score.SampleRate < PwSynthLimits::MinSampleRate || Score.SampleRate > PwSynthLimits::MaxSampleRate)
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_RECIPE;
        OutError = FString::Printf(
            TEXT("sampleRate is %d Hz, outside the %d..%d Hz the synth kernel renders an instrument ")
            TEXT("at. The score and its instruments must share one rate - mixing across rates ")
            TEXT("pitch-shifts every note and is refused rather than resampled."),
            Score.SampleRate, PwSynthLimits::MinSampleRate, PwSynthLimits::MaxSampleRate);
        return false;
    }

    if (Score.Tracks.Num() == 0)
    {
        OutErrorCode = ErrorCodes::ERR_AUDIO_EMPTY_BUFFER;
        OutError = TEXT("the score carries no tracks, so there is no stem to render. Reported as an ")
                   TEXT("error rather than as a successful render of zero files.");
        return false;
    }

    const FPwScoreLoop Loop = PwScoreLoopLength(Score);
    if (Loop.Frames <= 0)
    {
        OutErrorCode = ErrorCodes::ERR_AUDIO_EMPTY_BUFFER;
        OutError = FString::Printf(
            TEXT("the score's loop is %lld frames (%.4f ms at %d Hz, %d bars at %.4f bpm); there is ")
            TEXT("nothing to render."),
            Loop.Frames, Loop.DurationMs, Score.SampleRate, Score.Bars, Score.Bpm);
        return false;
    }

    if (Loop.Frames > static_cast<int64>(MAX_int32))
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_RECIPE;
        OutError = FString::Printf(
            TEXT("the score's loop is %lld frames, past the %d a single buffer addresses. Lower ")
            TEXT("the sample rate, the bar count or the tempo."),
            Loop.Frames, MAX_int32);
        return false;
    }

    const int32 LoopFrames = static_cast<int32>(Loop.Frames);
    const double TotalBeats = PwScoreTotalBeats(Score);

    // The loop is only sample-exact when the tempo divides the sample rate evenly. The residual
    // is carried out in the report rather than rounded away silently; the log line exists so a
    // caller who never reads the report still learns the boundary drifted.
    if (!Loop.bExact)
    {
        UE_LOG(LogPinWrightSubsystem, Warning,
            TEXT("PwRenderScoreStems: %d bars at %.4f bpm and %d Hz is %.6f frames, not a whole ")
            TEXT("number; rendering %d frames leaves a %.6f frame residual. %.6f bpm would be exact."),
            Score.Bars, Score.Bpm, Score.SampleRate, Loop.ExactFrames, LoopFrames,
            Loop.ResidualFrames, Loop.NearestExactBpm);
    }

    TArray<FPwStemResult> Stems;
    Stems.Reserve(Score.Tracks.Num());
    int32 TotalNotes = 0;

    for (int32 TrackIndex = 0; TrackIndex < Score.Tracks.Num(); ++TrackIndex)
    {
        const FPwMusicTrack& Track = Score.Tracks[TrackIndex];
        const FString TrackPath = FString::Printf(TEXT("tracks[%d] '%s'"), TrackIndex, *Track.Name);

        // ONE door for notes, explicit and generative alike, so this loop cannot use a different
        // RNG derivation for one mode than another or forget the explicit case.
        TArray<FPwMusicNote> Notes;
        if (!PwScoreGenerateTrackNotes(Score, TrackIndex, Notes, OutErrorCode, OutError))
        {
            return Fail(TrackPath);
        }

        if (Notes.Num() == 0)
        {
            OutErrorCode = ErrorCodes::ERR_AUDIO_EMPTY_BUFFER;
            OutError = TEXT("the track materialized no notes, so its stem would be digital silence. ")
                       TEXT("An empty stem and a quiet one are different outcomes.");
            return Fail(TrackPath);
        }

        // ---- pass 1: plan, so the stem can be sized to fit every tail ----------------
        TArray<FNotePlan> Plans;
        Plans.SetNum(Notes.Num());
        int64 FurthestEndFrame = LoopFrames;

        for (int32 NoteIndex = 0; NoteIndex < Notes.Num(); ++NoteIndex)
        {
            const FPwMusicNote& Note = Notes[NoteIndex];
            const FString NotePath = FString::Printf(TEXT("%s note %d"), *TrackPath, NoteIndex);

            if (!FMath::IsFinite(Note.StartBeat) || Note.StartBeat < PwMusicLimits::MinBeat)
            {
                OutErrorCode = ErrorCodes::ERR_INVALID_RECIPE;
                OutError = FString::Printf(
                    TEXT("startBeat is %.6f; a note cannot begin before the score."), Note.StartBeat);
                return Fail(NotePath);
            }

            if (Note.StartBeat >= TotalBeats)
            {
                OutErrorCode = ErrorCodes::ERR_INVALID_RECIPE;
                OutError = FString::Printf(
                    TEXT("startBeat %.6f is at or past the score's %.6f beats (%d bars of %d), so the ")
                    TEXT("note lands in the next repetition of the loop rather than in this one. ")
                    TEXT("Lengthen the score or move the note."),
                    Note.StartBeat, TotalBeats, Score.Bars, Score.TimeSignature.Numerator);
                return Fail(NotePath);
            }

            double VoiceMs = 0.0;
            if (!ComputeVoiceMs(Score, Track.Instrument, Note, VoiceMs, OutErrorCode, OutError))
            {
                return Fail(NotePath);
            }

            const int64 StartFrame64 = MsToFrames64(PwScoreBeatToMs(Score, Note.StartBeat), Score.SampleRate);
            const int64 VoiceFrames64 = MsToFrames64(VoiceMs, Score.SampleRate);
            if (StartFrame64 < 0 || StartFrame64 >= LoopFrames || VoiceFrames64 <= 0)
            {
                OutErrorCode = ErrorCodes::ERR_AUDIO_EMPTY_BUFFER;
                OutError = FString::Printf(
                    TEXT("the note lands at frame %lld for %lld frames inside a %d frame loop, which ")
                    TEXT("puts no samples on the stem."),
                    StartFrame64, VoiceFrames64, LoopFrames);
                return Fail(NotePath);
            }

            Plans[NoteIndex].StartFrame = static_cast<int32>(StartFrame64);
            Plans[NoteIndex].VoiceFrames = static_cast<int32>(VoiceFrames64);
            Plans[NoteIndex].VoiceMs = VoiceMs;
            FurthestEndFrame = FMath::Max(FurthestEndFrame, StartFrame64 + VoiceFrames64);
        }

        // Exactly as much overhang as the notes need - the longest tail, measured, not a fixed
        // allowance. Everything past LoopFrames is folded back onto the head further down.
        if (FurthestEndFrame > static_cast<int64>(MAX_int32))
        {
            OutErrorCode = ErrorCodes::ERR_INVALID_RECIPE;
            OutError = FString::Printf(
                TEXT("the track's longest note tail ends at frame %lld, past the %d a single buffer ")
                TEXT("addresses."),
                FurthestEndFrame, MAX_int32);
            return Fail(TrackPath);
        }
        const int32 StemFrames = static_cast<int32>(FurthestEndFrame);

        // The tonic anchors the pitch of every generator that has no absolute frequency
        // parameter. Resolved once per track, and only its failure is interesting: a track whose
        // own degree 0 falls outside MIDI cannot place any note.
        int32 TonicMidi = 0;
        if (NeedsTrackTonic(Track.Instrument.Generator.Kind))
        {
            if (!PwScoreDegreeToMidi(Score.Key.Scale, Score.Key.Root, Track.Octave, 0,
                    TonicMidi, OutErrorCode, OutError))
            {
                return Fail(TrackPath + TEXT(" tonic"));
            }
        }

        // The skeleton, built once. Per note only `seed`, `durationMs` and the pitch change, so
        // the fx chains and parameter bags are not rebuilt 512 times - per-note rendering is the
        // expensive part of this file.
        FPwSynthRecipe Skeleton;
        Skeleton.Version = PwSynthLimits::RecipeVersion;
        Skeleton.SampleRate = Score.SampleRate;
        Skeleton.Layers.Add(Track.Instrument);

        FPwSynthLayer& SkeletonLayer = Skeleton.Layers[0];
        // The score owns these three, which is why PwMusicScore rejects them inside an
        // instrument. A hand-built track never went through that parser, so they are overwritten
        // here rather than trusted. `pan` goes into the layer so PwRenderRecipe's own
        // equal-power law places the track once and this file carries no second pan law;
        // `gainDb` stays at unity and is applied to the finished stem instead, because
        // PwRenderRecipe clamps to +/-1 and a positive per-note gain would clip note by note.
        SkeletonLayer.StartMs = 0.0;
        SkeletonLayer.GainDb = 0.0;
        SkeletonLayer.Pan = Track.Pan;

        const TArray<FPwSynthPitchPoint> AuthoredPitchEnvelope = Track.Instrument.PitchEnvelope;
        const FPwSynthParams AuthoredGeneratorParams = Track.Instrument.Generator.Params;

        // Order-independent per-track substream, exactly mirroring PwRenderRecipe's
        // Root.Derive(LayerIndex). Deriving advances nothing, so track 3's stream is the same
        // whether or not tracks 0-2 rendered first.
        const FPwSeededRandom TrackRng = FPwSeededRandom(Score.Seed).Derive(TrackIndex);

        FPwAudioBuffer Stem;
        Stem.SampleRate = Score.SampleRate;
        Stem.SetNumFrames(StemFrames);

        FPwStemResult Result;
        Result.TrackName = Track.Name;

        // ---- pass 2: render each note and mix it in ---------------------------------
        for (int32 NoteIndex = 0; NoteIndex < Notes.Num(); ++NoteIndex)
        {
            const FPwMusicNote& Note = Notes[NoteIndex];
            const FNotePlan& Plan = Plans[NoteIndex];
            const FString NotePath = FString::Printf(TEXT("%s note %d"), *TrackPath, NoteIndex);

            int32 NoteMidi = 0;
            if (!PwScoreResolveNoteMidi(Score, Track, Note, NoteMidi, OutErrorCode, OutError))
            {
                return Fail(NotePath);
            }

            // Reset the two pitch-carrying fields to what the author wrote before applying this
            // note's pitch, so an offset can never accumulate across notes.
            SkeletonLayer.PitchEnvelope = AuthoredPitchEnvelope;
            SkeletonLayer.Generator.Params = AuthoredGeneratorParams;
            if (!ApplyNotePitch(SkeletonLayer, NoteMidi, TonicMidi, OutErrorCode, OutError))
            {
                return Fail(NotePath);
            }

            // Per-note substream of the track's stream. Re-rendering one track therefore cannot
            // perturb another, and editing note 5 cannot move note 4's noise.
            Skeleton.Seed = TrackRng.Derive(NoteIndex).Stream.GetInitialSeed();
            Skeleton.DurationMs = Plan.VoiceMs;

            FPwAudioBuffer Voice;
            FPwRenderReport VoiceReport;
            if (!PwRenderRecipe(Skeleton, Voice, VoiceReport, OutErrorCode, OutError))
            {
                return Fail(NotePath);
            }

            // Measured, not assumed: PwRenderRecipe owns the rate it renders at, and a buffer
            // that came back at a different one would be silently refused by MixInto below.
            if (Voice.SampleRate != Score.SampleRate)
            {
                OutErrorCode = ErrorCodes::ERR_INVALID_RECIPE;
                OutError = FString::Printf(
                    TEXT("the instrument rendered at %d Hz but the score is %d Hz; mixing across ")
                    TEXT("rates would drop the note entirely."),
                    Voice.SampleRate, Score.SampleRate);
                return Fail(NotePath);
            }

            // MixInto's own return, never the frame count that was requested. Zero means the note
            // reached the stem nowhere at all.
            const int32 Mixed = Voice.MixInto(Stem, static_cast<float>(Note.Velocity), Plan.StartFrame);
            if (Mixed <= 0)
            {
                ++Result.NotesDropped;
                UE_LOG(LogPinWrightSubsystem, Warning,
                    TEXT("PwRenderScoreStems: %s mixed 0 of %d frames at stem frame %d; it ")
                    TEXT("contributes nothing."),
                    *NotePath, Voice.NumFrames(), Plan.StartFrame);
            }
            else
            {
                ++Result.NotesRendered;
            }
        }

        // ---- the wrap: everything past the loop point folds back onto the head ------
        //
        // stem[i % LoopFrames] += stem[i] for every i >= LoopFrames. That is one period of the
        // score repeated forever, so a tail crossing the boundary continues into the next
        // repetition instead of being cut. No source index is ever a destination index (sources
        // are >= LoopFrames, destinations < LoopFrames), so nothing is folded twice, and the
        // modulo handles a tail longer than the whole loop without a special case.
        for (int32 Frame = LoopFrames; Frame < StemFrames; ++Frame)
        {
            const int32 Destination = Frame % LoopFrames;
            Stem.Left[Destination] += Stem.Left[Frame];
            Stem.Right[Destination] += Stem.Right[Frame];
        }
        Stem.SetNumFrames(LoopFrames);

        // Track level, applied once to the finished stem - see the skeleton comment above.
        const float TrackGain = static_cast<float>(DbToLinear(Track.GainDb));
        if (TrackGain != 1.f)
        {
            for (int32 Frame = 0; Frame < LoopFrames; ++Frame)
            {
                Stem.Left[Frame] *= TrackGain;
                Stem.Right[Frame] *= TrackGain;
            }
        }

        if (!MeasurePeakDb(Stem, TrackPath, Result.PeakDb, OutErrorCode, OutError))
        {
            return Fail(TrackPath);
        }

        Result.Buffer = MoveTemp(Stem);
        Result.bMeasured = true;
        TotalNotes += Result.NotesRendered + Result.NotesDropped;
        Stems.Add(MoveTemp(Result));
    }

    Out.Loop = Loop;
    Out.Stems = MoveTemp(Stems);
    Out.TotalNotes = TotalNotes;
    Out.bMeasured = true;
    return true;
}

bool PwMixStems(const TArray<FPwStemResult>& Stems, FPwAudioBuffer& Out,
                FString& OutErrorCode, FString& OutError)
{
    using namespace PwMusicRenderInternal;

    Out = FPwAudioBuffer();
    OutErrorCode.Reset();
    OutError.Reset();

    if (Stems.Num() == 0)
    {
        OutErrorCode = ErrorCodes::ERR_AUDIO_EMPTY_BUFFER;
        OutError = TEXT("no stems to mix. An empty set is an error rather than a zero-length ")
                   TEXT("success, because a selector that matched nothing looks identical to a ")
                   TEXT("clean run otherwise.");
        return false;
    }

    // Non-finite BEFORE anything that could describe a stem as silent or empty: NaN compares
    // false against every threshold, so a poisoned stem measures as digital silence if the
    // cheaper checks run first (rpc-design.md §7).
    for (const FPwStemResult& Stem : Stems)
    {
        double UnusedPeakDb = 0.0;
        if (!MeasurePeakDb(Stem.Buffer, FString::Printf(TEXT("stem '%s'"), *Stem.TrackName),
                UnusedPeakDb, OutErrorCode, OutError))
        {
            return false;
        }
    }

    const int32 SampleRate = Stems[0].Buffer.SampleRate;
    int32 MaxFrames = 0;

    for (const FPwStemResult& Stem : Stems)
    {
        if (!Stem.Buffer.IsValid())
        {
            OutErrorCode = ErrorCodes::ERR_INVALID_PARAMS;
            OutError = FString::Printf(
                TEXT("stem '%s' has %d left and %d right frames at %d Hz; a buffer whose channels ")
                TEXT("disagree would mix only as far as the shorter one."),
                *Stem.TrackName, Stem.Buffer.Left.Num(), Stem.Buffer.Right.Num(),
                Stem.Buffer.SampleRate);
            return false;
        }

        if (Stem.Buffer.SampleRate != SampleRate)
        {
            OutErrorCode = ErrorCodes::ERR_INVALID_PARAMS;
            OutError = FString::Printf(
                TEXT("stem '%s' is %d Hz but stem '%s' is %d Hz. MixInto refuses a rate mismatch ")
                TEXT("and returns 0, so mixing them would drop one stem entirely; resample before ")
                TEXT("mixing rather than letting a stem vanish."),
                *Stem.TrackName, Stem.Buffer.SampleRate, *Stems[0].TrackName, SampleRate);
            return false;
        }

        if (Stem.Buffer.NumFrames() <= 0)
        {
            OutErrorCode = ErrorCodes::ERR_AUDIO_EMPTY_BUFFER;
            OutError = FString::Printf(
                TEXT("stem '%s' carries no frames, so it would contribute nothing to the mixdown."),
                *Stem.TrackName);
            return false;
        }

        MaxFrames = FMath::Max(MaxFrames, Stem.Buffer.NumFrames());
    }

    FPwAudioBuffer Mix;
    Mix.SampleRate = SampleRate;
    Mix.SetNumFrames(MaxFrames);

    for (const FPwStemResult& Stem : Stems)
    {
        // The return, not the request. Out is sized to the longest stem, so a short return here
        // means something structural is wrong rather than that a stem was merely quiet.
        const int32 Mixed = Stem.Buffer.MixInto(Mix, 1.f, 0);
        if (Mixed != Stem.Buffer.NumFrames())
        {
            OutErrorCode = ErrorCodes::ERR_INVALID_PARAMS;
            OutError = FString::Printf(
                TEXT("stem '%s' mixed %d of its %d frames into a %d frame mixdown; the mixdown is ")
                TEXT("incomplete and is refused rather than published."),
                *Stem.TrackName, Mixed, Stem.Buffer.NumFrames(), MaxFrames);
            return false;
        }
    }

    Out = MoveTemp(Mix);
    return true;
}
