// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwGenFormant - the `formant` generator: creature cries, growls, roars and
// vowel-ish vocalizations, built as a classical SOURCE-FILTER model.
//
//   glottal source  ->  parallel bank of 5 resonant bandpasses  ->  mono out
//
// Nothing in Engine/Source/Runtime/SignalProcessing implements formants (there is
// no FVocoder, no formant bank, no vowel table anywhere in the module), so the
// source and the bank are written here; only the biquad itself is borrowed.
//
// WHY SOURCE AND FILTER ARE SEPARATE, AND WHY THAT IS THE POINT
// `f0Hz` moves the source. `formantShift` moves the filters. They are genuinely
// independent, and that independence is what makes this generator worth having:
// the same vowel at the same pitch turns into a mouse or a bull purely by moving
// the formants, because formant positions encode VOCAL TRACT LENGTH while f0
// encodes vocal fold length. A pitch-shifter cannot do that - it drags the
// formants along and produces chipmunks. The pitch envelope therefore multiplies
// f0 and never touches the bank.
//
// SOURCE: Rosenberg model-B glottal flow pulse, then a discrete radiation
// differentiator.
//   - Not a naive impulse train: that is spectrally flat and aliases across the
//     whole band. The Rosenberg pulse is a smooth closed-form flow shape whose
//     spectrum already falls at ~-12 dB/octave, so the harmonic sitting nearest
//     Nyquist at a typical f0 is ~80 dB down and the foldover is inaudible.
//   - The radiation differentiator y[n] = x[n] - R*x[n-1] models the lips
//     radiating volume velocity as pressure (+6 dB/octave), giving the ~-6 dB/oct
//     net source tilt of real voiced speech. It is applied AFTER sampling, so it
//     re-weights the sampled spectrum and adds no aliasing of its own. It also
//     removes the flow pulse's large DC component, which is not optional: the
//     Rosenberg pulse is strictly non-negative.
//   - The tilt is load-bearing, not cosmetic. With a raw -12 dB/oct source the
//     second formant of an open vowel is buried under the first and the output
//     reads as a hum rather than a vowel.
// Aspiration noise is Audio::FWhiteNoise, seeded from the passed-in RNG, mixed in
// at `breathiness` and deliberately NOT differentiated - the differentiator's
// ~30x low-frequency attenuation is compensated for the pulse branch at f0, and
// applying it to broadband noise as well would make any breathiness at all swamp
// the voice.
//
// JITTER AND SHIMMER: cycle-to-cycle wobble of period and amplitude. These are
// what separate a living creature from a synthesizer tone and they matter more
// than the third decimal of the formant table. The schema has no knob for them,
// so they are derived from `breathiness` over a non-zero floor (see
// JitterFraction / ShimmerDb below) - a perfectly periodic source is not a thing
// any throat produces.
//
// FILTER BANK: parallel, not cascaded. Parallel is what lets the published
// per-formant amplitudes be honoured; a cascade fixes the relative levels from
// the pole positions alone. Adjacent branches are summed with ALTERNATING SIGN
// (Klatt 1980, JASA 67(3) 971-995, section on the parallel branch): a bandpass
// leads by ~90 deg below its centre and lags by ~90 deg above it, so two adjacent
// resonators are ~180 deg out of phase in the valley between them and cancel
// unless one is inverted.
//
// LEVEL: the buffer is peak-normalized to 1.0 as the last step. The bank's
// absolute output level swings by tens of dB with vowel, f0 and shift, which
// would make the layer's gainDb unusable; level belongs to the layer's gainDb and
// ampEnvelope and to master normalize, all of which run downstream of here.
//
// Every engine symbol used here (Audio::FBiquadFilter, EBiquadFilter::Bandpass,
// Audio::FWhiteNoise(int32), Audio::ConvertToLinear, Audio::GetFrequencyMultiplier)
// is present unchanged on UE 5.3 through 5.8, so this file owes no row in
// docs/engine-version-support.md.

#include "AudioGen/PwSynthDsp.h"

#include "AudioGen/PwSeededRandom.h"
#include "AudioGen/PwSynthRecipe.h"
#include "Handlers/ErrorCodes.h"

#include "DSP/Dsp.h"
#include "DSP/Filter.h"
#include "DSP/Noise.h"

// Named (not anonymous) namespace: the module builds with bUseUnity = true.
namespace PwGenFormantInternal
{
    /** One resonance: centre frequency, level relative to F1, and -3 dB bandwidth. */
    struct FFormant
    {
        float FreqHz = 0.f;
        float AmpDb = 0.f;
        float BandwidthHz = 0.f;
    };

    constexpr int32 NumFormants = 5;

    struct FVowel
    {
        const TCHAR* Name = nullptr;
        FFormant Formants[NumFormants];
    };

    /**
     * Vowel table: the BASS voice column of the CHANT/IRCAM sung-vowel formant data
     * (Rodet, Potard & Barriere, "The CHANT Project", Computer Music Journal 8(3),
     * 1984), as published in the Csound manual's `fof` opcode vowel tables - the
     * usual reference set for exactly this job, because it is one of the few
     * published tables that gives frequency, RELATIVE AMPLITUDE and bandwidth for
     * all five formants rather than F1/F2 centres alone.
     *
     * The bass column is shipped rather than tenor/alto/soprano because the schema
     * exposes one vocal-tract knob, not a voice-type enum: bass is the longest tract
     * of the five, so `formantShift` above 1 reaches every smaller human voice and
     * below 1 keeps going into creature territory. Shipping five tables would need a
     * schema parameter that does not exist, and inventing one here would put the
     * generator out of sync with audio.synth.describe_schema.
     *
     * All five formants are shipped, and the schema's one-line summary says F1..F5
     * to match. F4/F5 are cheap and are most of what stops a synthesized vowel
     * sounding like a filtered buzz.
     */
    const FVowel VowelTable[] =
    {
        { TEXT("a"), { { 600.f,   0.f,  60.f }, { 1040.f,  -7.f,  70.f }, { 2250.f,  -9.f, 110.f }, { 2450.f,  -9.f, 120.f }, { 2750.f, -20.f, 130.f } } },
        { TEXT("e"), { { 400.f,   0.f,  40.f }, { 1620.f, -12.f,  80.f }, { 2400.f,  -9.f, 100.f }, { 2800.f, -12.f, 120.f }, { 3100.f, -18.f, 120.f } } },
        { TEXT("i"), { { 250.f,   0.f,  60.f }, { 1750.f, -30.f,  90.f }, { 2600.f, -16.f, 100.f }, { 3050.f, -22.f, 120.f }, { 3340.f, -28.f, 120.f } } },
        { TEXT("o"), { { 400.f,   0.f,  40.f }, {  750.f, -11.f,  80.f }, { 2400.f, -21.f, 100.f }, { 2600.f, -20.f, 120.f }, { 2900.f, -40.f, 120.f } } },
        { TEXT("u"), { { 350.f,   0.f,  40.f }, {  600.f, -20.f,  80.f }, { 2400.f, -32.f, 100.f }, { 2675.f, -28.f, 120.f }, { 2950.f, -36.f, 120.f } } }
    };

    /** Derived from the table, never hand-written, so the error message cannot drift from the data. */
    FString VowelNameList()
    {
        TArray<FString> Names;
        for (const FVowel& Vowel : VowelTable)
        {
            Names.Add(Vowel.Name);
        }
        return FString::Join(Names, TEXT(", "));
    }

    const FVowel* FindVowel(const FString& Name)
    {
        for (const FVowel& Vowel : VowelTable)
        {
            if (Name.Equals(Vowel.Name, ESearchCase::IgnoreCase))
            {
                return &Vowel;
            }
        }
        return nullptr;
    }

    // Schema bounds, mirrored from the `formant` row of FormantParams() in
    // PwSynthRecipe.cpp. Re-checked here because a generator may be driven from a
    // hand-built FPwSynthParams that never went through ParseSynthRecipe, and a
    // generator that trusts its caller for its ranges is a generator that renders
    // garbage instead of reporting a bad recipe.
    constexpr double MinF0Hz = 20.0;
    constexpr double MaxF0Hz = 2000.0;
    constexpr double MinFormantShift = 0.5;
    constexpr double MaxFormantShift = 2.0;

    // Rosenberg model-B shape constants: the flow rises over the first 40% of the
    // period and falls over the next 16%, leaving a 44% closed phase. Open quotient
    // 0.56 is Rosenberg's own fit to inverse-filtered speech (Rosenberg, "Effect of
    // glottal pulse shape on the quality of natural vowels", JASA 49(2), 1971).
    constexpr double OpenPhase = 0.40;
    constexpr double ClosePhase = 0.16;

    // Jitter / shimmer floors and breathiness slopes. The floors are the middle of
    // the normal modal-voice range and the ceilings reach the rough/pathological
    // range that growls live in (Baken & Orlikoff, "Clinical Measurement of Speech
    // and Voice", 2nd ed.: normal local jitter 0.2-1.0%, normal shimmer 0.1-0.5 dB).
    constexpr double MinJitterFraction = 0.004;     // 0.4% at breathiness 0
    constexpr double MaxJitterFraction = 0.025;     // 2.5% at breathiness 1
    constexpr double MinShimmerDb = 0.25;
    constexpr double MaxShimmerDb = 2.50;

    // Radiation differentiator corner. 10 Hz sits below the schema's lowest legal f0
    // (20 Hz), so the filter is a near-ideal differentiator across the whole voiced
    // band and a DC blocker below it.
    constexpr double RadiationCornerHz = 10.0;

    // Aspiration level relative to the f0-compensated pulse branch. A perceptual
    // balance, not a physical constant: it is what makes voicing 0.5 / breathiness
    // 0.5 read as "half breathy" instead of "noise with a hint of pitch".
    constexpr float AspirationScale = 0.5f;

    /**
     * Rosenberg model-B glottal FLOW, evaluated at a normalized position inside the
     * cycle. Peak 1.0 at the end of the opening phase, zero through the closed phase.
     */
    float RosenbergFlow(double Phase)
    {
        if (Phase < OpenPhase)
        {
            return static_cast<float>(0.5 * (1.0 - FMath::Cos(UE_DOUBLE_PI * Phase / OpenPhase)));
        }
        if (Phase < OpenPhase + ClosePhase)
        {
            return static_cast<float>(FMath::Cos(0.5 * UE_DOUBLE_PI * (Phase - OpenPhase) / ClosePhase));
        }
        return 0.f;
    }

    /**
     * UE's biquad takes bandwidth in OCTAVES and builds the RBJ cookbook alpha as
     *     Alpha = sin(w0) * sinh(0.5 * ln2 * BwOctaves * w0 / sin(w0))
     * (SignalProcessing/Private/Filter.cpp). A formant table gives bandwidth in Hz,
     * i.e. Q = Fc / BwHz, and the RBJ bandpass wants Alpha = sin(w0) / (2Q).
     * Inverting the engine's expression exactly - not approximately - gives
     *     BwOctaves = (2 / ln2) * asinh(1 / 2Q) * sin(w0) / w0
     * so the rendered -3 dB width is the published one and not a near miss.
     */
    float BandwidthHzToOctaves(double CenterHz, double BandwidthHz, double SampleRate)
    {
        constexpr double Ln2 = 0.69314718055994530942;

        const double W0 = 2.0 * UE_DOUBLE_PI * CenterHz / SampleRate;
        const double SinW0 = FMath::Sin(W0);
        const double Q = CenterHz / FMath::Max(1.0, BandwidthHz);
        const double X = 1.0 / (2.0 * Q);
        const double Asinh = FMath::Loge(X + FMath::Sqrt(X * X + 1.0));

        // sin(w0)/w0 -> 1 as w0 -> 0; guard the removable singularity rather than
        // dividing by a near-zero W0 at very low centre frequencies.
        const double SincCorrection = (W0 > UE_DOUBLE_SMALL_NUMBER) ? (SinW0 / W0) : 1.0;

        return static_cast<float>((2.0 / Ln2) * Asinh * SincCorrection);
    }

    /**
     * Piecewise-linear pitch envelope reader with a carried cursor. Render time is
     * monotonic, so the cursor makes this O(samples + points) instead of the
     * O(samples * points) a naive per-sample search would cost on a 64-point curve.
     * Before the first point and after the last it holds the endpoint value.
     */
    struct FPitchEnvelopeReader
    {
        explicit FPitchEnvelopeReader(const TArray<FPwSynthPitchPoint>& InPoints)
            : Points(InPoints)
        {
        }

        double SemitonesAt(double TimeMs)
        {
            if (Points.Num() == 0)
            {
                return 0.0;
            }
            if (TimeMs <= Points[0].TimeMs)
            {
                return Points[0].Semitones;
            }
            const int32 Last = Points.Num() - 1;
            while (Cursor < Last && TimeMs > Points[Cursor + 1].TimeMs)
            {
                ++Cursor;
            }
            if (Cursor >= Last)
            {
                return Points[Last].Semitones;
            }
            const FPwSynthPitchPoint& A = Points[Cursor];
            const FPwSynthPitchPoint& B = Points[Cursor + 1];
            const double Span = B.TimeMs - A.TimeMs;
            const double Alpha = (Span > 0.0) ? FMath::Clamp((TimeMs - A.TimeMs) / Span, 0.0, 1.0) : 1.0;
            return FMath::Lerp(A.Semitones, B.Semitones, Alpha);
        }

        const TArray<FPwSynthPitchPoint>& Points;
        int32 Cursor = 0;
    };

    bool Fail(const TCHAR* Code, FString&& Message, FString& OutErrorCode, FString& OutError)
    {
        OutErrorCode = Code;
        OutError = MoveTemp(Message);
        return false;
    }
}

bool PwGenFormant(const FPwSynthParams& Params, const TArray<FPwSynthPitchPoint>& PitchEnvelope,
                  const FPwSynthModulation& Modulation, int32 SampleRate, FPwSeededRandom& Rng,
                  TArrayView<float> OutMono, FString& OutErrorCode, FString& OutError)
{
    using namespace PwGenFormantInternal;

    OutErrorCode.Reset();
    OutError.Reset();

    // -----------------------------------------------------------------------
    // Validation. Everything that can fail is checked BEFORE the first write, so
    // OutMono is provably untouched on every failure path (rpc-design.md §1) with
    // no scratch copy needed. Existence is tested before range (§7) so a missing
    // parameter is never reported as an out-of-range one.
    // -----------------------------------------------------------------------
    static const FName NameF0(TEXT("f0Hz"));
    static const FName NameVowel(TEXT("vowel"));
    static const FName NameShift(TEXT("formantShift"));
    static const FName NameVoicing(TEXT("voicing"));
    static const FName NameBreathiness(TEXT("breathiness"));

    if (SampleRate <= 0)
    {
        return Fail(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("formant generator: sampleRate must be positive (got %d)."), SampleRate),
            OutErrorCode, OutError);
    }

    if (!Params.Has(NameF0))
    {
        return Fail(ErrorCodes::ERR_INVALID_PARAMS,
            TEXT("formant generator: 'f0Hz' is required and was not supplied."),
            OutErrorCode, OutError);
    }
    const double F0Hz = Params.GetNumber(NameF0);
    if (!FMath::IsFinite(F0Hz) || F0Hz < MinF0Hz || F0Hz > MaxF0Hz)
    {
        return Fail(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("formant generator: 'f0Hz' must be in [%g, %g] Hz (got %g)."),
                MinF0Hz, MaxF0Hz, F0Hz),
            OutErrorCode, OutError);
    }

    if (!Params.Has(NameVowel))
    {
        return Fail(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("formant generator: 'vowel' is required and was not supplied; valid values are %s."),
                *VowelNameList()),
            OutErrorCode, OutError);
    }
    const FString VowelName = Params.GetString(NameVowel);
    const FVowel* Vowel = FindVowel(VowelName);
    if (Vowel == nullptr)
    {
        // rpc-design.md §3: an unrecognised preset names the valid set and errors.
        // It never degrades into "just use /a/" - a caller asking for a vowel this
        // generator does not have needs to know that, not to get a different animal.
        return Fail(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("formant generator: unknown vowel '%s'; valid values are %s."),
                *VowelName, *VowelNameList()),
            OutErrorCode, OutError);
    }

    const double FormantShift = Params.Has(NameShift) ? Params.GetNumber(NameShift) : 1.0;
    if (!FMath::IsFinite(FormantShift) || FormantShift < MinFormantShift || FormantShift > MaxFormantShift)
    {
        return Fail(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("formant generator: 'formantShift' must be in [%g, %g] (got %g)."),
                MinFormantShift, MaxFormantShift, FormantShift),
            OutErrorCode, OutError);
    }

    const double Voicing = Params.Has(NameVoicing) ? Params.GetNumber(NameVoicing) : 1.0;
    if (!FMath::IsFinite(Voicing) || Voicing < 0.0 || Voicing > 1.0)
    {
        return Fail(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("formant generator: 'voicing' must be in [0, 1] (got %g)."), Voicing),
            OutErrorCode, OutError);
    }

    const double Breathiness = Params.Has(NameBreathiness) ? Params.GetNumber(NameBreathiness) : 0.0;
    if (!FMath::IsFinite(Breathiness) || Breathiness < 0.0 || Breathiness > 1.0)
    {
        return Fail(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("formant generator: 'breathiness' must be in [0, 1] (got %g)."), Breathiness),
            OutErrorCode, OutError);
    }

    if (Voicing <= 0.0 && Breathiness <= 0.0)
    {
        // Both source weights zero is a silent-silence trap: the bank would filter
        // nothing and the layer would vanish with every downstream verb reporting
        // success. Name it instead.
        return Fail(ErrorCodes::ERR_INVALID_PARAMS,
            TEXT("formant generator: 'voicing' and 'breathiness' are both 0, so the source is silent; raise at least one."),
            OutErrorCode, OutError);
    }

    // -----------------------------------------------------------------------
    // Filter bank construction. formantShift scales frequency AND bandwidth by the
    // same factor, holding Q constant: a shorter vocal tract raises the resonances
    // and widens them proportionally, and holding Q is what preserves the vowel's
    // identity while its size changes.
    // -----------------------------------------------------------------------
    const double SampleRateD = static_cast<double>(SampleRate);

    // Audio::FBiquadFilter clamps its own cutoff to 0.45 * SampleRate. A formant
    // pushed past that would silently pile onto the clamp frequency together with
    // every other out-of-range formant and manufacture a resonance that is in no
    // vowel table, so drop those branches instead.
    const double MaxFormantHz = 0.45 * SampleRateD;

    Audio::FBiquadFilter Filters[NumFormants];
    float BranchGain[NumFormants] = {};
    int32 NumActiveFormants = 0;

    for (int32 Index = 0; Index < NumFormants; ++Index)
    {
        const FFormant& Formant = Vowel->Formants[Index];
        const double CenterHz = Formant.FreqHz * FormantShift;
        if (CenterHz >= MaxFormantHz)
        {
            continue;
        }
        const double BandwidthHz = Formant.BandwidthHz * FormantShift;
        const float BandwidthOctaves = BandwidthHzToOctaves(CenterHz, BandwidthHz, SampleRateD);

        Filters[NumActiveFormants].Init(static_cast<float>(SampleRateD), 1, Audio::EBiquadFilter::Bandpass,
            static_cast<float>(CenterHz), BandwidthOctaves, 0.f);

        // Klatt's alternating sign across the parallel branches (see file header).
        // Index, not NumActiveFormants, drives the sign so a dropped high formant
        // does not flip the polarity of the ones above it.
        const float Sign = (Index % 2 == 0) ? 1.f : -1.f;
        BranchGain[NumActiveFormants] = Sign * Audio::ConvertToLinear(Formant.AmpDb);
        ++NumActiveFormants;
    }

    if (NumActiveFormants == 0)
    {
        return Fail(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("formant generator: every formant of vowel '%s' at formantShift %g lands at or above 0.45 * sampleRate (%g Hz); lower formantShift or raise sampleRate."),
                *VowelName, FormantShift, MaxFormantHz),
            OutErrorCode, OutError);
    }

    const int32 NumSamples = OutMono.Num();
    if (NumSamples == 0)
    {
        // A zero-length window is legal (a layer whose start lands on the last
        // sample of the mix). Filling zero samples exactly is what was asked for.
        return true;
    }

    // -----------------------------------------------------------------------
    // Source state.
    // -----------------------------------------------------------------------

    // Substreams, not the parent stream directly: jitter/shimmer, aspiration noise
    // and the modulator each get their own, so changing `breathiness` re-rolls the
    // aspiration without also re-rolling the jitter sequence. FPwSeededRandom::Derive
    // hashes the parent's INITIAL seed, so deriving is order-independent and consumes
    // nothing from the caller's stream.
    FPwSeededRandom CycleRng = Rng.Derive(0);
    FPwSeededRandom NoiseSeedRng = Rng.Derive(1);
    FPwSeededRandom ModSeedRng = Rng.Derive(2);

    // FRandomStream::RandRange computes (Max - Min + 1), which overflows on the full
    // int32 range and would hand every render the same seed; stay well inside it.
    Audio::FWhiteNoise AspirationNoise(NoiseSeedRng.IntInRange(1, MAX_int32 - 1));

    const double JitterFraction = FMath::Lerp(MinJitterFraction, MaxJitterFraction, Breathiness);
    const double ShimmerDb = FMath::Lerp(MinShimmerDb, MaxShimmerDb, Breathiness);

    // exp(-2*pi*Fc/Fs): first-order high-pass / differentiator coefficient.
    const double RadiationCoeff = FMath::Exp(-2.0 * UE_DOUBLE_PI * RadiationCornerHz / SampleRateD);

    // The differentiator attenuates the fundamental by |1 - R*exp(-j*w0)|, which is
    // roughly w0 and therefore roughly 30x at a typical f0. Undo exactly that much,
    // so `voicing` and `breathiness` keep the same relative meaning at 60 Hz and at
    // 800 Hz instead of the pulse branch fading out as f0 drops.
    const double W0 = 2.0 * UE_DOUBLE_PI * F0Hz / SampleRateD;
    const double RadiationReal = 1.0 - RadiationCoeff * FMath::Cos(W0);
    const double RadiationImag = RadiationCoeff * FMath::Sin(W0);
    const double RadiationGainAtF0 = FMath::Sqrt(RadiationReal * RadiationReal + RadiationImag * RadiationImag);
    const float PulseCompensation = static_cast<float>(1.0 / FMath::Max(UE_DOUBLE_SMALL_NUMBER, RadiationGainAtF0));

    FPitchEnvelopeReader PitchReader(PitchEnvelope);

    double GlottalPhase = 0.0;
    float PreviousFlow = 0.f;

    // Drawn per glottal cycle, held for its whole length. The first cycle draws
    // here so sample 0 is already perturbed rather than being the one perfectly
    // nominal cycle in the render.
    double CycleJitter = CycleRng.FloatInRange(static_cast<float>(-JitterFraction), static_cast<float>(JitterFraction));
    float CycleAmplitude = Audio::ConvertToLinear(CycleRng.FloatInRange(static_cast<float>(-ShimmerDb), static_cast<float>(ShimmerDb)));

    const float VoicingGain = static_cast<float>(Voicing);
    const float BreathGain = static_cast<float>(Breathiness) * AspirationScale;

    // The recipe hands the whole modulation block to the generator, so the generator
    // owns all three routings: fm has to be inside (it moves the glottal rate), and
    // splitting am/ring out to the renderer would leave them silently unapplied if
    // the renderer ever decided the generator had already done it.
    //
    // Validated here, before the render loop and therefore before a sample is written.
    // This replaces a hand-written clamp of `depth` into [0, 1]: the modulation row's
    // 0..100 range belongs to the fm index, and am/ring depth above 1 is now an ERROR
    // rather than a silent clamp, so a recipe cannot render at a depth its author did
    // not write. `formant` accepts fm - it moves the glottal rate.
    FPwSynthModulator Modulator;
    if (!PwPrepareModulation(Modulation, /*bAcceptsFm*/ true, TEXT("formant"), SampleRate,
            ModSeedRng, Modulator, OutErrorCode, OutError))
    {
        return false;
    }

    // -----------------------------------------------------------------------
    // Render, blockwise: the biquad's mono ProcessAudio path is a tight loop with
    // its state in registers, which a per-sample ProcessAudioFrame call cannot be.
    // Two small scratch blocks instead of two full-length buffers.
    // -----------------------------------------------------------------------
    constexpr int32 BlockSize = 512;
    float SourceBlock[BlockSize];
    float ModulatorBlock[BlockSize];
    float FilterBlock[BlockSize];

    float* const OutData = OutMono.GetData();

    for (int32 BlockStart = 0; BlockStart < NumSamples; BlockStart += BlockSize)
    {
        const int32 BlockNum = FMath::Min(BlockSize, NumSamples - BlockStart);

        for (int32 Local = 0; Local < BlockNum; ++Local)
        {
            const int32 Global = BlockStart + Local;

            // One draw per frame whatever the routing, so fm and am/ring read the same LFO.
            const double ModulatorValue = Modulator.Generate();
            ModulatorBlock[Local] = static_cast<float>(ModulatorValue);

            // Pitch envelope multiplies f0 and nothing else - the bank was built
            // once, above, and never moves.
            const double TimeMs = 1000.0 * static_cast<double>(Global) / SampleRateD;
            double InstantF0 = F0Hz * Audio::GetFrequencyMultiplier(
                static_cast<float>(PitchReader.SemitonesAt(TimeMs)));

            // depth is the FM index, so the peak deviation is index * rate. Zero for every
            // routing other than fm, so no routing test is needed here.
            InstantF0 += Modulator.FrequencyDeviationHz(ModulatorValue);

            // A deep pitch dive or a large FM index can drive the instantaneous rate
            // out of the audible band entirely; clamp the RATE rather than rejecting
            // the recipe, because f0Hz itself was already validated and this is a
            // per-sample excursion, not a bad parameter.
            InstantF0 = FMath::Clamp(InstantF0, 1.0, 0.25 * SampleRateD);

            const double PhaseStep = InstantF0 * (1.0 + CycleJitter) / SampleRateD;
            GlottalPhase += PhaseStep;
            if (GlottalPhase >= 1.0)
            {
                GlottalPhase -= FMath::FloorToDouble(GlottalPhase);
                CycleJitter = CycleRng.FloatInRange(static_cast<float>(-JitterFraction), static_cast<float>(JitterFraction));
                CycleAmplitude = Audio::ConvertToLinear(CycleRng.FloatInRange(static_cast<float>(-ShimmerDb), static_cast<float>(ShimmerDb)));
            }

            const float Flow = RosenbergFlow(GlottalPhase) * CycleAmplitude;
            const float Radiated = (Flow - static_cast<float>(RadiationCoeff) * PreviousFlow) * PulseCompensation;
            PreviousFlow = Flow;

            SourceBlock[Local] = VoicingGain * Radiated + BreathGain * AspirationNoise.Generate();
        }

        float* const OutBlock = OutData + BlockStart;
        FMemory::Memzero(OutBlock, BlockNum * sizeof(float));

        for (int32 Formant = 0; Formant < NumActiveFormants; ++Formant)
        {
            Filters[Formant].ProcessAudio(SourceBlock, BlockNum, FilterBlock);
            const float Gain = BranchGain[Formant];
            for (int32 Local = 0; Local < BlockNum; ++Local)
            {
                OutBlock[Local] += Gain * FilterBlock[Local];
            }
        }

        // am and ring both go through the one shared blend; the pass is skipped entirely for
        // the routings whose scale is a constant 1.
        if (Modulator.IsAmplitudeRouting())
        {
            for (int32 Local = 0; Local < BlockNum; ++Local)
            {
                OutBlock[Local] *= static_cast<float>(
                    Modulator.AmplitudeScale(static_cast<double>(ModulatorBlock[Local])));
            }
        }
    }

    // -----------------------------------------------------------------------
    // Peak-normalize to 1.0 (see the LEVEL note in the file header). A zero peak is
    // left alone rather than dividing by it; the only way to reach it is a recipe
    // whose source weights were already rejected above, so this is a guard, not a path.
    // -----------------------------------------------------------------------
    float Peak = 0.f;
    for (int32 Index = 0; Index < NumSamples; ++Index)
    {
        Peak = FMath::Max(Peak, FMath::Abs(OutData[Index]));
    }
    if (Peak > 0.f)
    {
        const float Scale = 1.f / Peak;
        for (int32 Index = 0; Index < NumSamples; ++Index)
        {
            OutData[Index] *= Scale;
        }
    }

    return true;
}
