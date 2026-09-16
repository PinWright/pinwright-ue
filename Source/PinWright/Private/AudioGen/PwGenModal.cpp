// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwGenModal - the `modal` generator: a bank of exponentially decaying resonant modes.
// This is the impact/glass/metal/wood/stone/bell source; everything those sounds are made
// of is "a set of partials, each with its own frequency, level and decay time".
//
// RESONATOR FORMULATION
// ---------------------
// One two-pole all-pole resonator per mode, driven by an exciter signal x[n]:
//
//     r     = exp(-ln(1000) / (T60Seconds * SampleRate))      // ln(1000) = 60 dB of amplitude
//     theta = 2*PI*FreqHz / SampleRate
//     y[n]  = g*x[n] + 2*r*cos(theta)*y[n-1] - r*r*y[n-2]
//
// Deliberately NOT Audio::FBiquadFilter in bandpass mode. Two reasons, both load-bearing at
// the Q a metal mode needs (a 2 s T60 at 1 kHz is Q ~ 4500):
//   1. FBiquadFilter is a direct-form biquad parameterised by BANDWIDTH, not by Q and not by
//      T60, and it clamps its own cutoff internally - so the decay time the caller asked for
//      is not a quantity the filter can be told, only one that can be approximated.
//   2. Its float direct-form coefficients lose the pole radius to rounding exactly where the
//      poles crowd the unit circle. The form above is a single conjugate pole pair whose
//      radius IS the coefficient, so `r < 1` is the stability proof rather than a hope.
// The recursion runs in double and only the summed result is narrowed to float, which removes
// state quantisation from the list of things that can make a 20 s tail misbehave.
//
// PER-MODE NORMALIZATION (why modeGainsDb is not a lie)
// -----------------------------------------------------
// The raw resonator's own gain varies wildly with frequency and decay: its unit-impulse
// response is h[n] = r^n * sin((n+1)*theta) / sin(theta), which peaks near 1/sin(theta) - a
// factor of 15.3 at 500 Hz and 1.6 at 5 kHz on a 48 kHz bus. Summing raw resonators would make
// modeGainsDb a knob that means something different at every frequency, which is the rpc-design
// §1 defect class (a number published under a name it does not measure).
//
// So each mode's input is scaled by g = LinearGain / H, where H is the peak ABSOLUTE value of
// that mode's unit-impulse response, computed in closed form:
//
//     alpha = -ln(r)                            (> 0)
//     eps   = min(theta, PI - theta)            (see the folding note below)
//     d/dn [ r^n sin((n+1)eps) ] = 0  =>  tan((n+1)eps) = eps/alpha
//     nStar = atan2(eps, alpha)/eps - 1         (the first, and therefore global, hump)
//     H     = max over n in {max(0,floor(nStar)), that+1} of r^n |sin((n+1)eps)| / sin(eps)
//
// The damped sine's first hump is its global maximum and the function is unimodal there, so
// checking the two integers bracketing nStar is exact, not a sample-and-hope. H >= |h[0]| = 1
// always, so g <= LinearGain and the normalization can never amplify.
//
// The eps folding is not cosmetic. Above SampleRate/4 the sampled sine no longer resolves its
// own humps - consecutive integer n step over whole continuous lobes - and solving the
// stationary point in theta directly reports the peak of a curve the samples never visit. The
// identity sin((n+1)(PI - eps)) = (-1)^n * sin((n+1)eps) with sin(PI - eps) = sin(eps) makes the
// discrete envelope of a near-Nyquist mode EXACTLY that of a mode at eps, so folding turns the
// hard case back into the solved one. Without it a 20 kHz / 50 ms mode on a 48 kHz bus
// normalizes to 1.0 when its true impulse peak is 1.99 - a ~6 dB lie in modeGainsDb, in the one
// register where a bank most needs its top modes not to be hot.
//
// The promise this buys: with the `impulse` exciter, a mode's peak sample amplitude is exactly
// its modeGainsDb, and two modes 12 dB apart in the request are 12 dB apart in the render. With
// `noise` / `strike` the bank is additionally shaped by the burst's own spectrum (see below),
// but the RELATIVE mode levels still hold to that shaping, because every mode is scaled by its
// own frequency- and decay-dependent H before the shared exciter reaches it.
//
// H is computed once, at the mode's BASE frequency. A pitch envelope or an fm excursion
// therefore moves the mode's level slightly as it moves its frequency - which is the direction
// a real body's radiated level moves with its pitch, and re-solving H per block would instead
// hold the level pinned while the pitch bends, which is the less physical of the two.
//
// EXCITERS (the schema's closed set: impulse | noise | strike)
// ------------------------------------------------------------
// The point of choosing an exciter is its SPECTRUM, not its level, so all three are normalized
// to agree with the unit impulse in the band they share and differ only in what they do above it:
//   impulse - one sample of 1.0. Flat spectrum, every mode excited equally. exciterMs is unused
//             by this exciter: a single sample has no length.
//   strike  - a raised-cosine mallet pulse of exciterMs, scaled to unit AREA. Unit area is unit
//             momentum, so it delivers exactly the impulse's low-frequency drive and differs
//             only by rolling the top off (the window's first null sits at 2/exciterMs Hz) -
//             which is the difference between a hammer and a soft beater. Normalizing it to
//             unit energy instead would make a 2 ms strike ~18 dB HOTTER than the impulse and a
//             10 ms one ~31 dB hotter, i.e. "longer contact, louder hit", which is backwards.
//   noise   - exciterMs of Audio::FWhiteNoise seeded from Rng under the same raised-cosine
//             window (so the burst neither clicks on nor off), scaled to unit L2 ENERGY. Area is
//             not available here - the sample signs are random and the sum is ~0 - and by
//             Parseval unit energy puts its mean spectral magnitude at the impulse's flat 1.0,
//             so it lands at the same level with per-mode scatter. That scatter is what makes
//             repeated hits sound like repeated hits rather than one hit played twice.
//
// COEFFICIENT UPDATE RATE
// -----------------------
// Coefficients are refreshed once per 64-frame block, not per sample. 64 frames is 1.33 ms at
// 48 kHz - two orders of magnitude finer than any audible pitch gesture - while amortising the
// bank's 32 cos() calls over 64 samples. 128 would be equally defensible; 64 was chosen for the
// finer fm resolution. Only A1 = 2*r*cos(theta) moves: r and the per-mode input gain are fixed
// at setup, so no amount of pitch or fm motion can push a pole outside the unit circle.
//
// FAILURE DIRECTION (rpc-design §1, §3)
// -------------------------------------
// Every check runs before the first write to OutMono, so a rejected render leaves the caller's
// buffer exactly as it found it. An unrecognised exciter is an ERROR - never a silent fallback
// to impulse, which would render a plausible sound for a recipe the caller cannot fix. A mode
// at or above Nyquist is an error naming the offending index (its cos(theta) folds, so it would
// otherwise ring at some unrelated mirrored frequency), as is a non-positive decay (r >= 1, an
// oscillator that never stops) and a mode count past the schema's cap.

#include "AudioGen/PwSynthDsp.h"

#include "AudioGen/PwSeededRandom.h"
#include "AudioGen/PwSynthRecipe.h"
#include "Handlers/ErrorCodes.h"

#include "DSP/Dsp.h"
#include "DSP/Noise.h"
#include "Math/UnrealMathUtility.h"

// Named (not anonymous) namespace: this module builds with bUseUnity = true, and anonymous
// namespaces in merged TUs are the ODR-collision source the plugin's Build.cs comment warns about.
namespace PwGenModalInternal
{
    /** ln(1000). A T60 is the time to lose 60 dB of amplitude, i.e. a factor of 1000. */
    constexpr double Ln1000 = 6.907755278982137;

    /** Coefficient refresh granularity, in frames. See the header comment for why 64. */
    constexpr int32 CoefficientBlockFrames = 64;

    /**
     * Mirrors the modal schema's MaxArrayNum (ModalParams() in PwSynthRecipe.cpp). Duplicated
     * on purpose rather than trusted: this function is callable without the parser, and a
     * 10,000-mode bank is a hang, not a sound. Enforced as an error, never a clamp - a
     * truncated bank is a different instrument, and the caller cannot see the truncation.
     */
    constexpr int32 MaxModes = 32;

    /**
     * Hard ceiling on the pole radius. exp() of a strictly positive argument is below 1 in
     * exact arithmetic, so this only defends against a T60 so long that the exponential rounds
     * up to 1.0 - which would park the poles on the unit circle and ring forever.
     */
    constexpr double MaxPoleRadius = 1.0 - 1.0e-12;

    /** Schema default for `exciterMs`, used when the bag was built without the parser. */
    constexpr double DefaultExciterMs = 2.0;

    enum class EExciter : uint8
    {
        Impulse,
        Noise,
        Strike
    };

    bool ExciterFromString(const FString& In, EExciter& Out)
    {
        if (In.Equals(TEXT("impulse"), ESearchCase::IgnoreCase)) { Out = EExciter::Impulse; return true; }
        if (In.Equals(TEXT("noise"), ESearchCase::IgnoreCase))   { Out = EExciter::Noise;   return true; }
        if (In.Equals(TEXT("strike"), ESearchCase::IgnoreCase))  { Out = EExciter::Strike;  return true; }
        return false;
    }

    bool Fail(FString& OutErrorCode, FString& OutError, const TCHAR* Code, const FString& Message)
    {
        OutErrorCode = Code;
        OutError = Message;
        return false;
    }

    /** One resonator. R and InputGain are fixed at setup; only A1 moves with pitch / fm. */
    struct FMode
    {
        double BaseFreqHz = 0.0;
        double R = 0.0;             // pole radius, strictly < 1
        double NegRSquared = 0.0;   // -r*r, the y[n-2] coefficient
        double A1 = 0.0;            // 2*r*cos(theta), refreshed once per block
        double InputGain = 0.0;     // LinearGain / peak-of-unit-impulse-response
        double Y1 = 0.0;
        double Y2 = 0.0;
    };

    /**
     * Peak |h[n]| of h[n] = r^n * sin((n+1)*theta) / sin(theta) over integer n >= 0, in closed
     * form. See the header comment for the derivation and for why the angle is folded about
     * PI/2 first. Never returns below 1 (|h[0]| == 1 for every mode), so the caller's division
     * by it can only attenuate.
     */
    double UnitImpulsePeak(double Theta, double Alpha)
    {
        // Above SampleRate/4 the sampled sine aliases its own lobes; sin(PI - eps) == sin(eps)
        // makes the folded angle's envelope identical to the real one, sample for sample.
        const double Eps = FMath::Min(Theta, UE_DOUBLE_PI - Theta);
        const double SinEps = FMath::Sin(Eps);
        if (!FMath::IsFinite(SinEps) || SinEps <= 0.0 || Eps <= 0.0)
        {
            return 1.0;
        }

        const double NStar = FMath::Atan2(Eps, Alpha) / Eps - 1.0;
        const double NLow = FMath::Max(0.0, FMath::FloorToDouble(NStar));

        double Peak = 1.0;
        for (int32 Step = 0; Step < 2; ++Step)
        {
            const double N = NLow + static_cast<double>(Step);
            // exp(-alpha*n) rather than Pow(r, n): n reaches millions for a sub-hertz mode on
            // a 192 kHz bus, where repeated multiplication in Pow loses the exponent's low bits.
            const double Value = FMath::Exp(-Alpha * N) * FMath::Abs(FMath::Sin((N + 1.0) * Eps)) / SinEps;
            if (FMath::IsFinite(Value))
            {
                Peak = FMath::Max(Peak, Value);
            }
        }
        return Peak;
    }

    /**
     * Piecewise-linear pitch envelope in semitones, held flat outside its own span. An empty
     * envelope means "no pitch motion", which is 0 semitones, not silence.
     */
    double EvalPitchSemitones(const TArray<FPwSynthPitchPoint>& Envelope, double TimeMs)
    {
        const int32 Num = Envelope.Num();
        if (Num == 0)
        {
            return 0.0;
        }
        if (TimeMs <= Envelope[0].TimeMs)
        {
            return Envelope[0].Semitones;
        }
        if (TimeMs >= Envelope[Num - 1].TimeMs)
        {
            return Envelope[Num - 1].Semitones;
        }
        for (int32 Index = 1; Index < Num; ++Index)
        {
            if (TimeMs <= Envelope[Index].TimeMs)
            {
                const double Span = Envelope[Index].TimeMs - Envelope[Index - 1].TimeMs;
                if (Span <= 0.0)
                {
                    // A zero-length segment is a step: the later point wins.
                    return Envelope[Index].Semitones;
                }
                const double T = (TimeMs - Envelope[Index - 1].TimeMs) / Span;
                return FMath::Lerp(Envelope[Index - 1].Semitones, Envelope[Index].Semitones, T);
            }
        }
        return Envelope[Num - 1].Semitones;
    }
}

bool PwGenModal(const FPwSynthParams& Params, const TArray<FPwSynthPitchPoint>& PitchEnvelope,
                const FPwSynthModulation& Modulation, int32 SampleRate, FPwSeededRandom& Rng,
                TArrayView<float> OutMono, FString& OutErrorCode, FString& OutError)
{
    using namespace PwGenModalInternal;

    OutErrorCode.Reset();
    OutError.Reset();

    // -----------------------------------------------------------------------------------
    // Validation. Everything below this block runs before the first write to OutMono, so a
    // rejected render leaves the caller's buffer byte-for-byte as it was handed over.
    // -----------------------------------------------------------------------------------
    if (SampleRate <= 0)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("sampleRate is %d; a modal render needs a positive sample rate."), SampleRate));
    }

    const int32 NumSamples = OutMono.Num();
    if (NumSamples <= 0)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
            TEXT("the output buffer has no samples; a zero-length render is an empty result, not a silent one."));
    }

    const FName FreqsKey(TEXT("modeFreqsHz"));
    const FName DecaysKey(TEXT("modeDecaysMs"));
    const FName GainsKey(TEXT("modeGainsDb"));

    const TArray<double>* Freqs = Params.GetNumbers(FreqsKey);
    const TArray<double>* Decays = Params.GetNumbers(DecaysKey);
    const TArray<double>* Gains = Params.GetNumbers(GainsKey);

    if (!Freqs || Freqs->Num() == 0)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            TEXT("modeFreqsHz is missing or empty; a modal bank with no modes has nothing to ring."));
    }
    if (!Decays || Decays->Num() == 0)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            TEXT("modeDecaysMs is missing or empty; every mode needs its own -60 dB decay time."));
    }
    if (!Gains || Gains->Num() == 0)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            TEXT("modeGainsDb is missing or empty; every mode needs its own level."));
    }

    // The schema's Validator hook already enforces this, but re-checked rather than trusted:
    // this function is reachable from a hand-built parameter bag, and a length mismatch read
    // through an unchecked index is a crash rather than an error message.
    const int32 NumModes = Freqs->Num();
    if (Decays->Num() != NumModes)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("modeDecaysMs has %d entries but modeFreqsHz has %d; the three mode arrays describe the same modes and must be the same length."),
                Decays->Num(), NumModes));
    }
    if (Gains->Num() != NumModes)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("modeGainsDb has %d entries but modeFreqsHz has %d; the three mode arrays describe the same modes and must be the same length."),
                Gains->Num(), NumModes));
    }
    if (NumModes > MaxModes)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("the bank has %d modes; the cap is %d. Drop the inaudible modes rather than expecting a truncated bank."),
                NumModes, MaxModes));
    }

    const double Nyquist = 0.5 * static_cast<double>(SampleRate);
    for (int32 Index = 0; Index < NumModes; ++Index)
    {
        const double FreqHz = (*Freqs)[Index];
        if (!FMath::IsFinite(FreqHz) || FreqHz <= 0.0)
        {
            return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("modeFreqsHz[%d] is %g; a mode frequency must be above 0 Hz."), Index, FreqHz));
        }
        if (FreqHz >= Nyquist)
        {
            return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("modeFreqsHz[%d] is %g Hz, at or above the Nyquist frequency %g Hz for sampleRate %d; the mode would alias and its cos(theta) folds onto a mirrored frequency."),
                    Index, FreqHz, Nyquist, SampleRate));
        }

        const double DecayMs = (*Decays)[Index];
        if (!FMath::IsFinite(DecayMs) || DecayMs <= 0.0)
        {
            return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("modeDecaysMs[%d] is %g; a -60 dB decay time must be above 0 ms (a non-positive decay puts the pole on or outside the unit circle)."),
                    Index, DecayMs));
        }

        const double GainDb = (*Gains)[Index];
        if (!FMath::IsFinite(GainDb))
        {
            return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("modeGainsDb[%d] is not a finite number."), Index));
        }
    }

    // rpc-design §3: an unrecognised exciter is an error. Falling back to impulse would render
    // a perfectly plausible sound for a recipe the caller has no way to learn is wrong.
    EExciter ExciterKind = EExciter::Impulse;
    const FString ExciterName = Params.GetString(FName(TEXT("exciter")), TEXT("impulse"));
    if (!ExciterFromString(ExciterName, ExciterKind))
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("exciter '%s' is not one of impulse|noise|strike."), *ExciterName));
    }

    const double ExciterMs = Params.GetNumber(FName(TEXT("exciterMs")), DefaultExciterMs);
    const bool bExciterHasLength = (ExciterKind != EExciter::Impulse);
    if (bExciterHasLength && (!FMath::IsFinite(ExciterMs) || ExciterMs <= 0.0))
    {
        // Only validated when it is used: the impulse exciter is a single sample and has no
        // length, so rejecting its exciterMs would be an error about an ignored field.
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("exciterMs is %g; the '%s' exciter is a burst and needs a positive length."),
                ExciterMs, *ExciterName));
    }

    // The layer's modulation, validated before a sample is written. `modal` accepts fm: every
    // mode's frequency moves together, which is how a struck body under vibrato behaves.
    // Substream 1; the exciter takes 0 below.
    FPwSynthModulator Modulator;
    if (!PwPrepareModulation(Modulation, /*bAcceptsFm*/ true, TEXT("modal"), SampleRate,
            Rng.Derive(1), Modulator, OutErrorCode, OutError))
    {
        return false;
    }

    // -----------------------------------------------------------------------------------
    // Setup. Two independent substreams so the exciter's draws and the modulator's draws
    // cannot shift each other; Derive() is order-independent and does not advance Rng.
    // -----------------------------------------------------------------------------------
    FPwSeededRandom ExciterRng = Rng.Derive(0);

    TArray<double> Exciter;
    {
        const int32 BurstSamples = bExciterHasLength
            ? FMath::Max(1, FMath::RoundToInt32(ExciterMs * static_cast<double>(SampleRate) / 1000.0))
            : 1;
        Exciter.SetNumZeroed(BurstSamples);

        if (ExciterKind == EExciter::Impulse)
        {
            Exciter[0] = 1.0;
        }
        else
        {
            // Raised cosine over (n + 0.5)/N rather than n/(N-1): it never evaluates to exactly
            // zero, so a one-sample burst degenerates to a clean unit impulse instead of silence,
            // and no special case is needed for it.
            Audio::FWhiteNoise Noise(ExciterRng.IntInRange(1, MAX_int32 - 1));
            for (int32 Index = 0; Index < BurstSamples; ++Index)
            {
                const double Window = 0.5 * (1.0 - FMath::Cos(
                    2.0 * UE_DOUBLE_PI * (static_cast<double>(Index) + 0.5) / static_cast<double>(BurstSamples)));
                Exciter[Index] = (ExciterKind == EExciter::Noise)
                    ? Window * static_cast<double>(Noise.Generate())
                    : Window;
            }

            // Normalize so the burst matches the unit impulse where the two overlap: unit area
            // (= unit momentum, same low-frequency drive) for the deterministic strike, unit L2
            // energy (= the same mean broadband spectral magnitude, by Parseval) for the noise
            // burst, whose random signs make its area meaningless. See the header comment.
            double Norm = 0.0;
            if (ExciterKind == EExciter::Noise)
            {
                for (const double Sample : Exciter)
                {
                    Norm += Sample * Sample;
                }
                Norm = FMath::Sqrt(Norm);
            }
            else
            {
                for (const double Sample : Exciter)
                {
                    Norm += Sample;
                }
            }
            if (Norm > 0.0)
            {
                const double Scale = 1.0 / Norm;
                for (double& Sample : Exciter)
                {
                    Sample *= Scale;
                }
            }
        }
    }

    TArray<FMode> Modes;
    Modes.Reserve(NumModes);
    for (int32 Index = 0; Index < NumModes; ++Index)
    {
        const double T60Seconds = (*Decays)[Index] * 0.001;
        const double Alpha = Ln1000 / (T60Seconds * static_cast<double>(SampleRate));   // -ln(r)
        const double Theta = 2.0 * UE_DOUBLE_PI * (*Freqs)[Index] / static_cast<double>(SampleRate);
        const double LinearGain = FMath::Pow(10.0, (*Gains)[Index] / 20.0);

        FMode Mode;
        Mode.BaseFreqHz = (*Freqs)[Index];
        Mode.R = FMath::Min(FMath::Exp(-Alpha), MaxPoleRadius);
        Mode.NegRSquared = -(Mode.R * Mode.R);
        Mode.A1 = 2.0 * Mode.R * FMath::Cos(Theta);
        Mode.InputGain = LinearGain / UnitImpulsePeak(Theta, Alpha);
        Modes.Add(Mode);
    }

    // -----------------------------------------------------------------------------------
    // Render. Coefficients refresh per block; the recursion runs per sample.
    // -----------------------------------------------------------------------------------
    for (int32 BlockStart = 0; BlockStart < NumSamples; BlockStart += CoefficientBlockFrames)
    {
        const int32 BlockEnd = FMath::Min(BlockStart + CoefficientBlockFrames, NumSamples);

        const double BlockTimeMs = 1000.0 * static_cast<double>(BlockStart) / static_cast<double>(SampleRate);
        const double PitchMultiplier = static_cast<double>(
            Audio::GetFrequencyMultiplier(static_cast<float>(EvalPitchSemitones(PitchEnvelope, BlockTimeMs))));

        for (int32 Index = BlockStart; Index < BlockEnd; ++Index)
        {
            // Exactly one modulator draw per output frame, taken here so that fm (read once per
            // coefficient block) and am/ring (read every frame) are the same LFO observed at two
            // rates rather than two LFOs free to drift apart.
            const double ModValue = Modulator.Generate();

            if (Index == BlockStart)
            {
                // Coefficients refresh once per block, keyed on the modulator value at the
                // block's FIRST frame. Textbook FM index: peak deviation in Hz is depth * rateHz,
                // and the same deviation is added to every mode, which is how a struck body under
                // vibrato behaves - the whole object's pitch moves, it does not re-tune each
                // partial's ratio independently. FrequencyDeviationHz is 0 for every routing
                // other than fm, so this needs no routing test of its own.
                const double FmDeltaHz = Modulator.FrequencyDeviationHz(ModValue);

                for (FMode& Mode : Modes)
                {
                    const double FreqHz = Mode.BaseFreqHz * PitchMultiplier + FmDeltaHz;
                    // Clamp of a DERIVED value, not of a caller parameter: the caller's own mode
                    // frequencies were rejected outright above if they sat at or past Nyquist, and
                    // there is no honest error to raise for a frequency a pitch envelope produced
                    // mid-render. Letting it fold (cos is even and 2*PI-periodic) would silently
                    // mirror the mode onto an unrelated frequency, which is strictly worse than
                    // pinning it to the band edge.
                    const double Bounded = FMath::Clamp(FreqHz, 0.0, Nyquist);
                    Mode.A1 = 2.0 * Mode.R
                        * FMath::Cos(2.0 * UE_DOUBLE_PI * Bounded / static_cast<double>(SampleRate));
                }
            }

            const double Drive = Exciter.IsValidIndex(Index) ? Exciter[Index] : 0.0;

            double Sum = 0.0;
            for (FMode& Mode : Modes)
            {
                const double Y = Drive * Mode.InputGain + Mode.A1 * Mode.Y1 + Mode.NegRSquared * Mode.Y2;
                Mode.Y2 = Mode.Y1;
                Mode.Y1 = Y;
                Sum += Y;
            }

            // AmplitudeScale is exactly 1 for None and for fm, so it is applied unconditionally
            // rather than behind a routing flag that could fall out of step with the modulator.
            OutMono[Index] = static_cast<float>(Sum * Modulator.AmplitudeScale(ModValue));
        }
    }

    return true;
}
