// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwGenOscNoise.cpp - the `osc` and `noise` generator kernels of the audio.synth render path.
//
// Both kernels write a MONO buffer that the renderer has already sized to the layer's frame
// count, and both obey the same three contracts:
//
//   rpc-design.md §1 - every failure leaves OutMono byte-for-byte untouched. All validation runs
//     before the first sample is written, so a half-rendered layer is not representable.
//   rpc-design.md §3 - an unrecognised waveform, colour, modulator shape or routing is an ERROR.
//     There is no fallback to sine / white / no-op: a recipe that renders a different sound from
//     the one it names is exactly the defect an agent cannot see.
//   PwSeededRandom.h - every stochastic source is seeded from the passed-in FPwSeededRandom.
//     Audio::FWhiteNoise and Audio::FPinkNoise default-construct from FPlatformTime::Cycles()
//     (SignalProcessing/Private/Noise.cpp), which silently makes a render unreproducible, so the
//     seeded constructor is used unconditionally.
//
// The band-limited oscillator core (FPwBlepOsc) and the layer modulator (FPwSynthModulator,
// PwPrepareModulation) used to be file-local here, which is why three other generators ended up
// with their own copies of the am / ring formulas. Both now live in AudioGen/PwSynthDsp.h and are
// shared by all six generators; this file only consumes them.
//
// Parameter semantics are taken from the kind's spec table in PwSynthRecipe.cpp - required-ness,
// documented default, range and enum vocabulary are all read back out of FPwSynthKindSpec rather
// than restated here, so a schema edit cannot drift away from what the DSP enforces.
//
// Engine symbols used: Audio::FSinOsc2DRotation and Audio::GetFrequencyMultiplier (DSP/Dsp.h),
// Audio::FWhiteNoise / Audio::FPinkNoise (DSP/Noise.h), Audio::FBiquadFilter (DSP/Filter.h -
// note DSP/BiQuadFilter.h holds only the deprecated FBiquad). All four are present unchanged on
// UE 5.3 through 5.8, so this file owes no row in docs/engine-version-support.md.

#include "AudioGen/PwSynthDsp.h"

#include "AudioGen/PwSeededRandom.h"
#include "AudioGen/PwSynthRecipe.h"
#include "Handlers/ErrorCodes.h"

#include "DSP/Dsp.h"
#include "DSP/Filter.h"
#include "DSP/Noise.h"
#include "Math/UnrealMathUtility.h"

// Named (not anonymous) namespace: this module builds with bUseUnity = true, and anonymous
// namespaces in merged TUs are the ODR-collision source the plugin's Build.cs comment warns about.
namespace PwGenOscNoiseInternal
{
    // -----------------------------------------------------------------------------------------
    // Substream indices.
    //
    // FPwSeededRandom::Derive hashes the PARENT's initial seed with the index, so a substream is
    // the same stream regardless of which other substreams were taken, and taking one advances
    // nothing. These two sit above the osc `unison` ceiling (8) so a modulator or a noise source
    // can never collide with a unison voice's substream.
    // -----------------------------------------------------------------------------------------
    constexpr int32 ModulatorStreamIndex = 1000;
    constexpr int32 NoiseSourceStreamIndex = 1001;

    /** Corner of the one-pole used to build brown noise, in Hz. Two decades below the top of the
        audible band, so the pole behaves as a perfect integrator across everything above it. */
    constexpr float BrownIntegratorHz = 20.f;

    bool Fail(const TCHAR* Code, const FString& Message, FString& OutErrorCode, FString& OutError)
    {
        OutErrorCode = Code;
        OutError = Message;
        return false;
    }

    // -----------------------------------------------------------------------------------------
    // Schema-table access. The table is the single source of truth (PwSynthRecipe.h header
    // comment), so required-ness / default / range / vocabulary are read from it here too.
    // -----------------------------------------------------------------------------------------

    const FPwSynthParamSpec* FindSpec(const FPwSynthKindSpec& Kind, FName Name)
    {
        if (!Kind.Params)
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

    /** The closed set a spec row publishes, for use in an error message so the message and the
        schema cannot disagree. Empty when the row is not an enum. */
    FString EnumVocabulary(const FPwSynthKindSpec& Kind, const TCHAR* Key)
    {
        const FPwSynthParamSpec* Spec = FindSpec(Kind, FName(Key));
        return (Spec && Spec->EnumValues) ? FString(Spec->EnumValues) : FString();
    }

    /**
     * Reads one Number / Integer row. A missing required row and an out-of-range value are both
     * errors: substituting a plausible value the caller cannot see is what makes a wrong render
     * look like a working one. An absent optional row materialises the table's documented default.
     */
    bool ReadNumber(const FPwSynthParams& Params, const FPwSynthKindSpec& Kind, const TCHAR* Key,
        double& OutValue, FString& OutErrorCode, FString& OutError)
    {
        const FName Name(Key);
        const FPwSynthParamSpec* Spec = FindSpec(Kind, Name);
        if (!Spec)
        {
            return Fail(ErrorCodes::ERR_INVALID_PARAMS, FString::Printf(
                TEXT("'%s' is not a parameter of the '%s' generator."), Key, Kind.Name),
                OutErrorCode, OutError);
        }

        if (!Params.Has(Name))
        {
            if (Spec->bRequired)
            {
                return Fail(ErrorCodes::ERR_INVALID_PARAMS, FString::Printf(
                    TEXT("'%s' is required by the '%s' generator and was not supplied."), Key, Kind.Name),
                    OutErrorCode, OutError);
            }
            OutValue = Spec->DefaultNumber;
            return true;
        }

        OutValue = Params.GetNumber(Name);
        if (Spec->bHasMin && OutValue < Spec->Min)
        {
            return Fail(ErrorCodes::ERR_INVALID_PARAMS, FString::Printf(
                TEXT("'%s' is %g, below the minimum %g."), Key, OutValue, Spec->Min),
                OutErrorCode, OutError);
        }
        if (Spec->bHasMax && OutValue > Spec->Max)
        {
            return Fail(ErrorCodes::ERR_INVALID_PARAMS, FString::Printf(
                TEXT("'%s' is %g, above the maximum %g."), Key, OutValue, Spec->Max),
                OutErrorCode, OutError);
        }
        return true;
    }

    /** Reads one Enum row as its raw token. The caller maps the token to its own enum and reports
        an unrecognised one; there is deliberately no "nearest match" step. */
    bool ReadEnumString(const FPwSynthParams& Params, const FPwSynthKindSpec& Kind, const TCHAR* Key,
        FString& OutValue, FString& OutErrorCode, FString& OutError)
    {
        const FName Name(Key);
        const FPwSynthParamSpec* Spec = FindSpec(Kind, Name);
        if (!Spec)
        {
            return Fail(ErrorCodes::ERR_INVALID_PARAMS, FString::Printf(
                TEXT("'%s' is not a parameter of the '%s' generator."), Key, Kind.Name),
                OutErrorCode, OutError);
        }

        if (!Params.Has(Name))
        {
            if (Spec->bRequired || !Spec->DefaultString)
            {
                return Fail(ErrorCodes::ERR_INVALID_PARAMS, FString::Printf(
                    TEXT("'%s' is required by the '%s' generator and was not supplied."), Key, Kind.Name),
                    OutErrorCode, OutError);
            }
            OutValue = Spec->DefaultString;
            return true;
        }

        OutValue = Params.GetString(Name);
        return true;
    }

    // -----------------------------------------------------------------------------------------
    // Waveform / colour vocabularies.
    // -----------------------------------------------------------------------------------------

    bool ParseWaveform(const FString& In, EPwOscWave& Out)
    {
        if (In.Equals(TEXT("sine"), ESearchCase::IgnoreCase))     { Out = EPwOscWave::Sine;     return true; }
        if (In.Equals(TEXT("triangle"), ESearchCase::IgnoreCase)) { Out = EPwOscWave::Triangle; return true; }
        if (In.Equals(TEXT("saw"), ESearchCase::IgnoreCase))      { Out = EPwOscWave::Saw;      return true; }
        if (In.Equals(TEXT("square"), ESearchCase::IgnoreCase))   { Out = EPwOscWave::Square;   return true; }
        if (In.Equals(TEXT("pulse"), ESearchCase::IgnoreCase))    { Out = EPwOscWave::Pulse;    return true; }
        return false;
    }

    enum class EPwNoiseColor : uint8
    {
        White,
        Pink,
        Brown,
        Blue,
        Violet
    };

    bool ParseNoiseColor(const FString& In, EPwNoiseColor& Out)
    {
        if (In.Equals(TEXT("white"), ESearchCase::IgnoreCase))  { Out = EPwNoiseColor::White;  return true; }
        if (In.Equals(TEXT("pink"), ESearchCase::IgnoreCase))   { Out = EPwNoiseColor::Pink;   return true; }
        if (In.Equals(TEXT("brown"), ESearchCase::IgnoreCase))  { Out = EPwNoiseColor::Brown;  return true; }
        if (In.Equals(TEXT("blue"), ESearchCase::IgnoreCase))   { Out = EPwNoiseColor::Blue;   return true; }
        if (In.Equals(TEXT("violet"), ESearchCase::IgnoreCase)) { Out = EPwNoiseColor::Violet; return true; }
        return false;
    }

    // -----------------------------------------------------------------------------------------
    // Pitch envelope.
    // -----------------------------------------------------------------------------------------

    /** True when the envelope can never change pitch, and the constant offset it contributes.
        An empty envelope is a flat 0. */
    bool IsPitchEnvelopeConstant(const TArray<FPwSynthPitchPoint>& Envelope, double& OutSemitones)
    {
        OutSemitones = (Envelope.Num() > 0) ? Envelope[0].Semitones : 0.0;
        for (const FPwSynthPitchPoint& Point : Envelope)
        {
            if (Point.Semitones != OutSemitones)
            {
                return false;
            }
        }
        return true;
    }

    /**
     * Cursored evaluator for a layer-relative pitch envelope. Segments are linear in semitones;
     * the first point's value is held before the first point and the last point's after the last,
     * so the envelope never extrapolates off its own ends. Points are assumed ascending in TimeMs,
     * which ParseSynthRecipe guarantees.
     *
     * The cursor matters: the render walks time strictly forward, and a per-sample linear scan of
     * a 64-point envelope (the schema ceiling) over a 60 s render is ~184M comparisons.
     */
    struct FPwPitchEnvelopeReader
    {
        explicit FPwPitchEnvelopeReader(const TArray<FPwSynthPitchPoint>& InPoints)
            : Points(InPoints)
        {
        }

        double ValueAt(double TimeMs)
        {
            const int32 Num = Points.Num();
            if (Num == 0)
            {
                return 0.0;
            }
            if (Num == 1 || TimeMs <= Points[0].TimeMs)
            {
                return Points[0].Semitones;
            }
            if (TimeMs >= Points[Num - 1].TimeMs)
            {
                return Points[Num - 1].Semitones;
            }

            while (Cursor + 1 < Num - 1 && TimeMs > Points[Cursor + 1].TimeMs)
            {
                ++Cursor;
            }

            const FPwSynthPitchPoint& A = Points[Cursor];
            const FPwSynthPitchPoint& B = Points[Cursor + 1];
            const double Span = B.TimeMs - A.TimeMs;
            if (Span <= 0.0)
            {
                return B.Semitones;     // coincident points: the later one wins
            }
            const double Alpha = FMath::Clamp((TimeMs - A.TimeMs) / Span, 0.0, 1.0);
            return FMath::Lerp(A.Semitones, B.Semitones, Alpha);
        }

        const TArray<FPwSynthPitchPoint>& Points;
        int32 Cursor = 0;
    };

    // -----------------------------------------------------------------------------------------
    // Noise sources.
    // -----------------------------------------------------------------------------------------

    /** A deterministic int32 seed for an engine generator that takes one. Uses the substream's
        INITIAL seed rather than a draw, so seeding never advances the parent stream and the value
        does not depend on the order the render happens to visit its sources in. */
    int32 SubstreamSeed(const FPwSeededRandom& Rng, int32 StreamIndex)
    {
        return Rng.Derive(StreamIndex).Stream.GetInitialSeed();
    }

    template <typename TSource>
    void FillFromSource(TSource& Source, TArrayView<float> Out)
    {
        for (int32 Index = 0; Index < Out.Num(); ++Index)
        {
            Out[Index] = Source.Generate();
        }
    }

    /**
     * One-sample difference, which has |H(f)| = 2*sin(pi*f/fs): a clean +6 dB/octave tilt across
     * the band. Applied to white (flat) it gives violet (+6 dB/oct); applied to pink (-3 dB/oct)
     * it gives blue (+3 dB/oct).
     *
     * 1/sqrt(2) is the exact make-up gain for white - the difference of two independent samples
     * has twice the variance - and an approximation for pink, whose neighbouring samples are
     * positively correlated, so blue lands a little under pink's RMS.
     */
    template <typename TSource>
    void FillDifferentiated(TSource& Source, TArrayView<float> Out)
    {
        constexpr float MakeUpGain = 0.70710678f;   // 1 / sqrt(2)

        float Previous = Source.Generate();
        for (int32 Index = 0; Index < Out.Num(); ++Index)
        {
            const float Current = Source.Generate();
            Out[Index] = (Current - Previous) * MakeUpGain;
            Previous = Current;
        }
    }
}

// =============================================================================================
// osc
// =============================================================================================

bool PwGenOsc(const FPwSynthParams& Params, const TArray<FPwSynthPitchPoint>& PitchEnvelope,
    const FPwSynthModulation& Modulation, int32 SampleRate, FPwSeededRandom& Rng,
    TArrayView<float> OutMono, FString& OutErrorCode, FString& OutError)
{
    using namespace PwGenOscNoiseInternal;

    OutErrorCode.Reset();
    OutError.Reset();

    // ---- validation. Nothing below writes to OutMono until every check has passed. ----

    if (SampleRate <= 0)
    {
        return Fail(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("sampleRate is %d; a render needs a positive sample rate."), SampleRate),
            OutErrorCode, OutError);
    }
    if (OutMono.Num() <= 0)
    {
        return Fail(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
            TEXT("the osc generator was handed a zero-length buffer; an empty render and a silent one are different outcomes."),
            OutErrorCode, OutError);
    }

    const FPwSynthKindSpec& Kind = PwSynthGeneratorSpec(EPwSynthGeneratorKind::Osc);

    FString WaveformName;
    if (!ReadEnumString(Params, Kind, TEXT("waveform"), WaveformName, OutErrorCode, OutError))
    {
        return false;
    }
    EPwOscWave Wave = EPwOscWave::Sine;
    if (!ParseWaveform(WaveformName, Wave))
    {
        return Fail(ErrorCodes::ERR_INVALID_PARAMS, FString::Printf(
            TEXT("'%s' is not an osc waveform; expected one of %s."),
            *WaveformName, *EnumVocabulary(Kind, TEXT("waveform"))), OutErrorCode, OutError);
    }

    double FrequencyHz = 0.0;
    double PulseWidth = 0.5;
    double PhaseTurns = 0.0;
    double DetuneCents = 0.0;
    double UnisonCount = 1.0;
    double UnisonSpreadCents = 0.0;
    if (!ReadNumber(Params, Kind, TEXT("frequencyHz"), FrequencyHz, OutErrorCode, OutError)
        || !ReadNumber(Params, Kind, TEXT("pulseWidth"), PulseWidth, OutErrorCode, OutError)
        || !ReadNumber(Params, Kind, TEXT("phase"), PhaseTurns, OutErrorCode, OutError)
        || !ReadNumber(Params, Kind, TEXT("detuneCents"), DetuneCents, OutErrorCode, OutError)
        || !ReadNumber(Params, Kind, TEXT("unison"), UnisonCount, OutErrorCode, OutError)
        || !ReadNumber(Params, Kind, TEXT("unisonSpreadCents"), UnisonSpreadCents, OutErrorCode, OutError))
    {
        return false;
    }

    // Validated and built here, before the render section, because PwPrepareModulation is the
    // only way to obtain a configured modulator - see the note on it in PwSynthDsp.h.
    FPwSynthModulator Modulator;
    if (!PwPrepareModulation(Modulation, /*bAcceptsFm*/ true, TEXT("osc"), SampleRate,
            Rng.Derive(ModulatorStreamIndex), Modulator, OutErrorCode, OutError))
    {
        return false;
    }

    // ---- render ----

    const int32 NumFrames = OutMono.Num();
    const int32 NumVoices = FMath::Max(1, FMath::RoundToInt(UnisonCount));
    const float Nyquist = 0.5f * static_cast<float>(SampleRate);

    // The schema scopes pulseWidth to the `pulse` waveform, so `square` is a fixed 50% duty
    // whatever was supplied.
    const float EffectivePulseWidth = (Wave == EPwOscWave::Pulse) ? static_cast<float>(PulseWidth) : 0.5f;

    double ConstantSemitones = 0.0;
    const bool bConstantPitch = IsPitchEnvelopeConstant(PitchEnvelope, ConstantSemitones);

    // Fast path: Audio::FSinOsc2DRotation is a SIMD 2D-rotation sine generator, roughly 10x
    // faster than a per-sample FMath::Sin with about -100 dB distortion (DSP/Dsp.h). Its
    // frequency is fixed for the whole call, so it is used exactly when pitch cannot move - one
    // voice, a pitch envelope that never changes value, and no fm. am and ring are amplitude
    // operations applied afterwards, so they do not disqualify it.
    const bool bFastSine = Wave == EPwOscWave::Sine
        && NumVoices == 1
        && bConstantPitch
        && Modulation.Routing != EPwSynthModulationRouting::Fm;

    if (bFastSine)
    {
        // f = frequencyHz * 2^((semitones + cents/100) / 12), the same fold as the general path.
        const float Semitones = static_cast<float>(ConstantSemitones + DetuneCents / 100.0);
        const float Hz = FMath::Clamp(
            static_cast<float>(FrequencyHz) * Audio::GetFrequencyMultiplier(Semitones),
            -Nyquist, Nyquist);

        Audio::FSinOsc2DRotation SinOsc(2.f * UE_PI * static_cast<float>(PhaseTurns));
        SinOsc.GenerateBuffer(static_cast<float>(SampleRate), Hz, OutMono.GetData(), NumFrames);

        if (Modulator.IsAmplitudeRouting())
        {
            for (int32 Frame = 0; Frame < NumFrames; ++Frame)
            {
                OutMono[Frame] *= static_cast<float>(Modulator.AmplitudeScale(Modulator.Generate()));
            }
        }
        return true;
    }

    // Unison. `unisonSpreadCents` is the TOTAL spread the schema documents, so the stack spans
    // [-spread/2, +spread/2] and a single voice is exactly the undetuned oscillator.
    const FPwSynthParamSpec* SpreadSpec = FindSpec(Kind, FName(TEXT("unisonSpreadCents")));
    const double MaxSpread = (SpreadSpec && SpreadSpec->bHasMax && SpreadSpec->Max > 0.0) ? SpreadSpec->Max : 100.0;

    TArray<FPwBlepOsc, TInlineAllocator<8>> Voices;
    TArray<float, TInlineAllocator<8>> VoiceCents;
    Voices.Reserve(NumVoices);
    VoiceCents.Reserve(NumVoices);

    for (int32 Voice = 0; Voice < NumVoices; ++Voice)
    {
        const float Position = (NumVoices > 1)
            ? (static_cast<float>(Voice) / static_cast<float>(NumVoices - 1) - 0.5f)
            : 0.f;
        VoiceCents.Add(static_cast<float>(DetuneCents) + Position * static_cast<float>(UnisonSpreadCents));

        // Per-voice substream, so a stack is reproducible and the voice COUNT does not shift what
        // the layer's other stochastic sources draw: Derive hashes the parent's initial seed with
        // the voice index, so voices 0 and 1 of a 2-voice stack are the same two voices as in a
        // 4-voice stack, and deriving them advances the parent not at all.
        FPwSeededRandom VoiceRng = Rng.Derive(Voice);

        // Phase scatter is proportional to the detune actually asked for. At spread 0 the stack is
        // exactly coherent, which is what makes the 1/N sum below level-preserving and what keeps
        // the `phase` parameter meaningful; at full spread the voices are fully scattered, which
        // is what a wide detuned stack wants. Scattering a zero-spread stack would instead build a
        // comb filter out of N copies of one waveform.
        const float ScatterTurns = static_cast<float>(FMath::Clamp(UnisonSpreadCents / MaxSpread, 0.0, 1.0));
        const float Scatter = ScatterTurns * VoiceRng.FloatInRange(-0.5f, 0.5f);

        FPwBlepOsc Osc;
        Osc.Init(Wave, EffectivePulseWidth, static_cast<float>(PhaseTurns) + Scatter);
        Voices.Add(Osc);
    }

    FPwPitchEnvelopeReader PitchReader(PitchEnvelope);

    const double MsPerFrame = 1000.0 / static_cast<double>(SampleRate);
    const float InvSampleRate = 1.f / static_cast<float>(SampleRate);
    // 1/N, so a unison stack does not change level: N coherent voices sum to N and divide back to
    // exactly the single-voice waveform, and a detuned stack can only sit below that.
    const float InvVoices = 1.f / static_cast<float>(NumVoices);

    for (int32 Frame = 0; Frame < NumFrames; ++Frame)
    {
        // One modulator step per FRAME, not per voice, so a unison stack and a single voice hear
        // the same modulator.
        const double ModValue = Modulator.Generate();
        const float DeviationHz = static_cast<float>(Modulator.FrequencyDeviationHz(ModValue));
        const float EnvSemitones = static_cast<float>(
            bConstantPitch ? ConstantSemitones : PitchReader.ValueAt(Frame * MsPerFrame));

        float Sum = 0.f;
        for (int32 Voice = 0; Voice < NumVoices; ++Voice)
        {
            // Pitch envelope and detune both fold into the instantaneous frequency:
            //   f(t) = frequencyHz * 2^((semitones(t) + cents/100) / 12)
            // and the fm deviation adds on top of that in Hz.
            const float Semitones = EnvSemitones + VoiceCents[Voice] * 0.01f;
            float Hz = static_cast<float>(FrequencyHz) * Audio::GetFrequencyMultiplier(Semitones) + DeviationHz;

            // Matches IOscBase::Update's own clamp: past Nyquist there is no waveform left to
            // render, only folded aliases.
            Hz = FMath::Clamp(Hz, -Nyquist, Nyquist);

            Sum += Voices[Voice].Generate(Hz * InvSampleRate);
        }

        OutMono[Frame] = static_cast<float>(Sum * InvVoices * Modulator.AmplitudeScale(ModValue));
    }

    return true;
}

// =============================================================================================
// noise
// =============================================================================================

bool PwGenNoise(const FPwSynthParams& Params, const TArray<FPwSynthPitchPoint>& PitchEnvelope,
    const FPwSynthModulation& Modulation, int32 SampleRate, FPwSeededRandom& Rng,
    TArrayView<float> OutMono, FString& OutErrorCode, FString& OutError)
{
    using namespace PwGenOscNoiseInternal;

    OutErrorCode.Reset();
    OutError.Reset();

    // ---- validation. Nothing below writes to OutMono until every check has passed. ----

    if (SampleRate <= 0)
    {
        return Fail(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("sampleRate is %d; a render needs a positive sample rate."), SampleRate),
            OutErrorCode, OutError);
    }
    if (OutMono.Num() <= 0)
    {
        return Fail(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
            TEXT("the noise generator was handed a zero-length buffer; an empty render and a silent one are different outcomes."),
            OutErrorCode, OutError);
    }

    const FPwSynthKindSpec& Kind = PwSynthGeneratorSpec(EPwSynthGeneratorKind::Noise);

    FString ColorName;
    if (!ReadEnumString(Params, Kind, TEXT("color"), ColorName, OutErrorCode, OutError))
    {
        return false;
    }
    EPwNoiseColor Color = EPwNoiseColor::White;
    if (!ParseNoiseColor(ColorName, Color))
    {
        return Fail(ErrorCodes::ERR_INVALID_PARAMS, FString::Printf(
            TEXT("'%s' is not a noise colour; expected one of %s."),
            *ColorName, *EnumVocabulary(Kind, TEXT("color"))), OutErrorCode, OutError);
    }

    double LowCutHz = 0.0;
    double HighCutHz = 0.0;
    if (!ReadNumber(Params, Kind, TEXT("lowCutHz"), LowCutHz, OutErrorCode, OutError)
        || !ReadNumber(Params, Kind, TEXT("highCutHz"), HighCutHz, OutErrorCode, OutError))
    {
        return false;
    }
    if (LowCutHz >= HighCutHz)
    {
        return Fail(ErrorCodes::ERR_INVALID_PARAMS, FString::Printf(
            TEXT("lowCutHz %g is not below highCutHz %g; the pair leaves no passband and would render silence."),
            LowCutHz, HighCutHz), OutErrorCode, OutError);
    }

    // `noise` has no instantaneous frequency, so bAcceptsFm is false and an `fm` block is
    // rejected rather than ignored.
    FPwSynthModulator Modulator;
    if (!PwPrepareModulation(Modulation, /*bAcceptsFm*/ false, TEXT("noise"), SampleRate,
            Rng.Derive(ModulatorStreamIndex), Modulator, OutErrorCode, OutError))
    {
        return false;
    }

    // Noise has no pitch, so a pitch envelope on a noise layer would be silently discarded - the
    // same failure as an ignored fm routing. An envelope that is flat at 0 changes nothing and is
    // accepted; one that actually moves is rejected so the caller learns the layer cannot use it.
    for (const FPwSynthPitchPoint& Point : PitchEnvelope)
    {
        if (Point.Semitones != 0.0)
        {
            return Fail(ErrorCodes::ERR_INVALID_PARAMS,
                TEXT("the 'noise' generator has no pitch for a pitchEnvelope to act on; remove the envelope or use a pitched generator."),
                OutErrorCode, OutError);
        }
    }

    // ---- render ----

    const int32 NumFrames = OutMono.Num();

    // Always the SEEDED constructor. The default ones take their seed from FPlatformTime::Cycles()
    // (SignalProcessing/Private/Noise.cpp:13, :28), which makes a render unreproducible with
    // nothing downstream able to detect that it happened.
    const int32 NoiseSeed = SubstreamSeed(Rng, NoiseSourceStreamIndex);

    switch (Color)
    {
    case EPwNoiseColor::White:
    {
        Audio::FWhiteNoise Source(NoiseSeed);
        FillFromSource(Source, OutMono);
        break;
    }

    case EPwNoiseColor::Pink:
    {
        Audio::FPinkNoise Source(NoiseSeed);
        FillFromSource(Source, OutMono);
        break;
    }

    case EPwNoiseColor::Brown:
    {
        // Brown is integrated white: -6 dB/octave. A one-pole parked at 20 Hz is a perfect
        // integrator across everything above it and, unlike a bare accumulator, cannot random-walk
        // into a DC offset over a 60 s render. Both constants are closed form: a one-pole with
        // coefficient a passes sqrt(a / (2 - a)) of a white input's RMS, so sqrt((2 - a) / a) puts
        // brown back at white's level. Brown noise is peaky, so peaks above 1.0 are expected -
        // absolute level is the master `normalize` block's job, not the generator's.
        const float Alpha = FMath::Clamp(
            2.f * UE_PI * BrownIntegratorHz / static_cast<float>(SampleRate), 1e-6f, 1.f);
        const float MakeUpGain = FMath::Sqrt((2.f - Alpha) / Alpha);

        Audio::FWhiteNoise Source(NoiseSeed);
        float State = 0.f;
        for (int32 Frame = 0; Frame < NumFrames; ++Frame)
        {
            State += Alpha * (Source.Generate() - State);
            OutMono[Frame] = State * MakeUpGain;
        }
        break;
    }

    case EPwNoiseColor::Blue:
    {
        Audio::FPinkNoise Source(NoiseSeed);
        FillDifferentiated(Source, OutMono);
        break;
    }

    case EPwNoiseColor::Violet:
    {
        Audio::FWhiteNoise Source(NoiseSeed);
        FillDifferentiated(Source, OutMono);
        break;
    }
    }

    // Band limits. Each stage is skipped when the caller left it parked at the extreme of its
    // documented range. That is an optimisation, not a behaviour change: a highpass at the row's
    // minimum and a lowpass at its maximum sit at the edges of the band and remove nothing that
    // is in the signal.
    const FPwSynthParamSpec* LowSpec = FindSpec(Kind, FName(TEXT("lowCutHz")));
    const FPwSynthParamSpec* HighSpec = FindSpec(Kind, FName(TEXT("highCutHz")));
    const bool bApplyLowCut = LowSpec && LowSpec->bHasMin && LowCutHz > LowSpec->Min;
    const bool bApplyHighCut = HighSpec && HighSpec->bHasMax && HighCutHz < HighSpec->Max;

    // Audio::FBiquadFilter's mono ProcessAudio keeps its state in locals for the whole run
    // (Private/Filter.cpp:121-151), so passing the same pointer in and out is safe.
    if (bApplyLowCut)
    {
        Audio::FBiquadFilter HighPass;
        HighPass.Init(static_cast<float>(SampleRate), 1, Audio::EBiquadFilter::Highpass,
            static_cast<float>(LowCutHz));
        HighPass.ProcessAudio(OutMono.GetData(), NumFrames, OutMono.GetData());
    }
    if (bApplyHighCut)
    {
        Audio::FBiquadFilter LowPass;
        LowPass.Init(static_cast<float>(SampleRate), 1, Audio::EBiquadFilter::Lowpass,
            static_cast<float>(HighCutHz));
        LowPass.ProcessAudio(OutMono.GetData(), NumFrames, OutMono.GetData());
    }

    // fm was rejected above, so the only routings that reach here are the amplitude ones.
    if (Modulator.IsAmplitudeRouting())
    {
        for (int32 Frame = 0; Frame < NumFrames; ++Frame)
        {
            OutMono[Frame] *= static_cast<float>(Modulator.AmplitudeScale(Modulator.Generate()));
        }
    }

    return true;
}
