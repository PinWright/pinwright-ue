// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwFxChainB.cpp - the second half of the recipe effect vocabulary: chorus, flanger,
// phaser, ringmod, pitchshift, compressor, eq, gain, width, reverse, convolve.
// (filter / distort / delay / reverb are chain A.)
//
// Every effect processes an FPwDspSpan IN PLACE. Right == nullptr is a mono layer
// chain; both channels set is the stereo master bus.
//
// ---------------------------------------------------------------------------
// Engine DSP used, and where this file composes instead
// ---------------------------------------------------------------------------
// Everything below is a plain exported class from Runtime/SignalProcessing, present
// unchanged on UE 5.3-5.8, with no device and no optional plugin behind it. Nothing
// here uses a symbol newer than 5.3, so this file owes no row in
// docs/engine-version-support.md. The Synthesis plugin's FSourceEffect* wrappers are
// deliberately NOT used - they drag in AudioModulation through
// Audio::FModulationDestination and add nothing this path needs.
//
//   flanger     Audio::FFlanger            (DSP/Flanger.h)
//   ringmod     Audio::FRingModulation     (DSP/RingModulation.h)
//   pitchshift  Audio::FTapDelayPitchShifter + Audio::FDelay
//   compressor  Audio::FDynamicsProcessor  (DSP/DynamicsProcessor.h)
//   eq          3x Audio::FBiquadFilter    (DSP/Filter.h)
//   convolve    Audio::IConvolutionAlgorithm via Audio::FConvolutionFactory, plus
//               Audio::ISampleRateConverter to bring the impulse response to the render rate
//               (PwResolveSourceBuffer returns the asset at its NATIVE rate - see below)
//   chorus      Audio::FDelay + Audio::FLFO       - see "composed, not wrapped" below
//   phaser      Audio::FBiquadFilter + Audio::FLFO - see "composed, not wrapped" below
//   gain        Audio::ConvertToLinear (DSP/Dsp.h)
//   width       mid/side arithmetic; reverse: Algo::Reverse
//
// COMPOSED, NOT WRAPPED. Audio::FChorus and Audio::FPhaser exist, and both are
// themselves thin compositions of FDelay / FLFO / FBiquadFilter - but neither can
// express its schema row, and the gap is not at the edges, it is at the DEFAULTS:
//
//   * FChorus has exactly three delay taps, EChorusDelays::{Left,Center,Right}, a
//     compile-time constant (Chorus.h:11-20). The schema's `voices` is 1..8 and
//     DEFAULTS TO 2. Wrapping it would either silently ignore `voices` on every
//     recipe (rpc-design.md §1: a parameter reported as honoured and discarded) or
//     reject every recipe that did not ask for exactly 3.
//   * FPhaser has six fixed all-pass stages, no sweep-range control at all, and
//     clamps feedback to [0, 1] (Phaser.cpp:46, :90-95). The schema has `stages`
//     2..12 (default 4), a REQUIRED `depth` 0..1, and signed feedback -0.99..0.99.
//     There is no value of `depth` a wrapper could honour.
//
// So both are rebuilt here from the same engine primitives those classes use, which
// keeps every parameter measurable instead of decorative. The remaining unhonourable
// parameters are reported as errors, never ignored:
//
//   * pitchshift `formantPreserve` - UE 5.8 SignalProcessing ships no phase vocoder
//     (no STFT-domain shifter of any kind), and a tap-delay shifter transposes the
//     spectral envelope with the pitch by construction. Setting it is an
//     UNSUPPORTED_OPTION error, not a silently-dropped flag.
//   * ringmod `rateHz` outside 10..10000 Hz - FRingModulation clamps the carrier
//     into that window (RingModulation.cpp:57) while the schema row allows
//     0.1..20000. A clamped carrier is a wrong answer reported as a right one, so
//     an out-of-window rate is an INVALID_PARAMS error naming the supported range.
//
// ---------------------------------------------------------------------------
// Contracts every effect in this file keeps
// ---------------------------------------------------------------------------
// 1. FAILURE LEAVES THE BUFFER BIT-IDENTICAL. Every fallible step - parameter
//    validation, enum parsing, impulse-response resolution, convolution-algorithm
//    construction - runs BEFORE the first sample is written, and the wet signal is
//    always built in a scratch array that is blended back only on the success path.
//    A half-processed span would silently corrupt every later effect in the chain.
// 2. mix == 0 IS BIT-IDENTICAL TO THE INPUT. Effects with a `mix` row return early
//    without touching the span rather than computing In*1 + Wet*0, which is only
//    bit-identical while Wet stays finite. `width` == 1 returns early for the same
//    reason: (L+R)/2 + (L-R)/2 is not exactly L in float.
// 3. NO SILENT DEGRADATION (rpc-design.md §3). Bounds, required-ness, defaults and
//    closed enum sets are all read from the effect's own row in the FxTable spec
//    (PwSynthRecipe.h) rather than restated here, so the DSP layer and the parser
//    cannot drift; an unrecognised enum value is an error naming the valid set.
// 4. A VALUE THE ENGINE WOULD SILENTLY CLAMP IS REJECTED, NOT CLAMPED. Several of
//    these classes narrow their inputs without saying so - Audio::FBiquadFilter pins
//    every cutoff into [5 Hz, 0.9 * Nyquist] (Filter.cpp:63), FRingModulation pins its
//    carrier to 10..10000 Hz - and the schema rows are wider than both. A clamped
//    value renders a band or a carrier the recipe did not ask for and reports success,
//    so caller-supplied frequencies are range-checked against what the engine can
//    actually realise at THIS render rate. The one deliberate exception is the phaser's
//    swept all-pass frequencies, which are derived from `depth` rather than named by
//    the caller; those ride the clamp and say so at the call site.
// 5. DETERMINISM BY CONSTRUCTION. Not one effect here is stochastic, so none draws
//    from the FPwSeededRandom the contract passes in and every definition leaves
//    that parameter unnamed. Two renders of the same params are byte-identical
//    because there is no state to differ.

#include "AudioGen/PwSynthDsp.h"

#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwSeededRandom.h"
#include "AudioGen/PwSynthRecipe.h"
#include "Handlers/ErrorCodes.h"

#include "Algo/Reverse.h"
#include "DSP/AlignedBuffer.h"
#include "DSP/ConvolutionAlgorithm.h"
#include "DSP/Delay.h"
#include "DSP/Dsp.h"
#include "DSP/DynamicsProcessor.h"
#include "DSP/Filter.h"
#include "DSP/Flanger.h"
#include "DSP/LFO.h"
#include "DSP/Osc.h"
#include "DSP/RingModulation.h"
#include "DSP/SampleRateConverter.h"
#include "DSP/TapDelayPitchShifter.h"
#include "Math/NumericLimits.h"
#include "Math/UnrealMathUtility.h"

// Named namespace, not anonymous: the module builds with bUseUnity = true and
// Private/AudioGen/ is merged into one TU with its siblings. See CLAUDE.md > Building.
namespace PwFxChainBInternal
{
    // ---------------------------------------------------------------------
    // Tuning constants. Each one names the engine limit or the DSP fact it
    // encodes, so a future reader can argue with the number and not just the code.
    // ---------------------------------------------------------------------

    constexpr int32 MaxSpanChannels = 2;

    /** Upper bound of the chorus `voices` row; sizes the fixed tap arrays below. */
    constexpr int32 MaxChorusVoices = 8;

    /** Upper bound of the phaser `stages` row; sizes the fixed all-pass array below. */
    constexpr int32 MaxPhaserStages = 12;

    /**
     * Floor of the chorus delay sweep, in milliseconds. Audio::FChorus uses the same
     * 5 ms floor (Chorus.cpp:9); below it a chorus tap turns into a flanger, which is
     * a different effect with its own row in the schema.
     */
    constexpr double ChorusMinDelayMs = 5.0;

    /** Chorus delay-line allocation. Must clear ChorusMinDelayMs + the depthMs row max (50). */
    constexpr float ChorusDelayLineSeconds = 0.25f;

    /**
     * Delay-line length handed to Audio::FTapDelayPitchShifter, in milliseconds; inside
     * its legal 10..100 window (TapDelayPitchShifter.h:15-16). The tap crossfade runs at
     * |1 - ratio| / length, so a longer line pushes the crossfade further below the audio
     * band - 10 Hz at one octave up here, versus 100 Hz at the 10 ms minimum - at the cost
     * of a longer partially-empty read window at each end of the span.
     */
    constexpr double PitchShiftDelayMs = 50.0;

    /** Pitch-shift delay-line allocation; must clear PitchShiftDelayMs with room to spare. */
    constexpr float PitchShiftDelayLineSeconds = 0.5f;

    /**
     * Audio::FRingModulation::SetModulationFrequency clamps the carrier into this window
     * (RingModulation.cpp:57). The schema's rateHz row is wider, so a rate outside it is
     * rejected by name instead of silently clamped.
     */
    constexpr double RingModMinCarrierHz = 10.0;
    constexpr double RingModMaxCarrierHz = 10000.0;

    /**
     * Phaser sweep geometry. Stage bases are spread over PhaserStageSpreadOctaves starting
     * at PhaserBaseHz, and the LFO lifts the whole comb by up to PhaserSweepOctaves scaled
     * by `depth`, so depth 0 is a static comb and depth 1 is a three-octave sweep. The
     * ranges are the same order as Audio::FPhaser's own hardcoded per-stage ranges
     * (Phaser.cpp:47-66), which is where the numbers come from.
     */
    constexpr double PhaserBaseHz = 200.0;
    constexpr double PhaserStageSpreadOctaves = 3.0;
    constexpr double PhaserSweepOctaves = 3.0;

    /** All-pass bandwidth in octaves. Audio::FBiquadFilter takes octaves, not Q. */
    constexpr float PhaserAllPassBandwidthOctaves = 1.0f;

    /**
     * Frames between phaser all-pass coefficient recomputations. 32 frames is a 1.5 kHz
     * control rate at 48 kHz - eight times finer than Audio::FPhaser's own 256-sample
     * control period - without recomputing up to twelve biquads every sample.
     */
    constexpr int32 PhaserControlBlockFrames = 32;

    /** Convolution partition size. Must be a power of two (UniformPartitionConvolution.cpp:499). */
    constexpr int32 ConvolutionBlockFrames = 256;

    // ---------------------------------------------------------------------
    // Span helpers
    // ---------------------------------------------------------------------

    /** Rejects an unusable span before any effect touches it. */
    bool ValidateSpan(const FPwDspSpan& InOut, int32 SampleRate, const TCHAR* EffectName,
        FString& OutErrorCode, FString& OutError)
    {
        // Existence first (rpc-design.md §7): "there were no samples" must never be
        // reported as the narrower-sounding "the effect failed".
        if (InOut.Left == nullptr || InOut.NumFrames <= 0)
        {
            OutErrorCode = ErrorCodes::ERR_AUDIO_EMPTY_BUFFER;
            OutError = FString::Printf(
                TEXT("%s: the effect was handed a span of %d frames with %s left channel; there is nothing to process"),
                EffectName, InOut.NumFrames, InOut.Left ? TEXT("a") : TEXT("no"));
            return false;
        }
        if (SampleRate <= 0)
        {
            OutErrorCode = ErrorCodes::ERR_INVALID_PARAMS;
            OutError = FString::Printf(TEXT("%s: sample rate is %d; it must be positive"),
                EffectName, SampleRate);
            return false;
        }
        return true;
    }

    /** Channel pointers actually present in the span, in order. Returns 1 (mono) or 2 (stereo). */
    int32 GatherChannels(const FPwDspSpan& InOut, float* Out[MaxSpanChannels])
    {
        Out[0] = InOut.Left;
        Out[1] = InOut.Right;
        return InOut.IsStereo() ? 2 : 1;
    }

    // ---------------------------------------------------------------------
    // Parameter reads, validated against the effect's own spec-table row
    //
    // The FxTable in PwSynthRecipe.cpp is the single source of truth for a
    // parameter's required-ness, bounds, default and closed enum set. Restating any
    // of those here would let the DSP layer and the parser drift apart silently, so
    // every read below looks the row up and validates against it. A bag that came
    // out of ParseSynthRecipe always passes; a hand-built bag (a test, or a future
    // caller that skips the parser) gets the same rejection the parser would give.
    // ---------------------------------------------------------------------

    const FPwSynthParamSpec* FindRow(EPwSynthFxKind Kind, const TCHAR* Key)
    {
        const FPwSynthKindSpec& Spec = PwSynthFxSpec(Kind);
        if (Spec.Params == nullptr)
        {
            return nullptr;
        }
        const FName Name(Key);
        for (const FPwSynthParamSpec& Row : *Spec.Params)
        {
            if (Row.Name == Name)
            {
                return &Row;
            }
        }
        return nullptr;
    }

    FString QualifiedName(EPwSynthFxKind Kind, const TCHAR* Key)
    {
        return FString::Printf(TEXT("%s.%s"), PwSynthFxKindToString(Kind), Key);
    }

    /** A row this file asks for that the spec table does not carry is a code defect, not caller error. */
    bool ReportMissingRow(EPwSynthFxKind Kind, const TCHAR* Key, FString& OutErrorCode, FString& OutError)
    {
        OutErrorCode = ErrorCodes::ERR_INTERNAL_ERROR;
        OutError = FString::Printf(
            TEXT("%s: the effect implementation reads a parameter the schema does not declare; the spec table and the DSP have drifted"),
            *QualifiedName(Kind, Key));
        return false;
    }

    bool ReportOutOfRange(EPwSynthFxKind Kind, const TCHAR* Key, const FPwSynthParamSpec& Row,
        double Value, FString& OutErrorCode, FString& OutError)
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_PARAMS;
        OutError = FString::Printf(TEXT("%s is %g, outside the documented range [%g, %g]"),
            *QualifiedName(Kind, Key), Value,
            Row.bHasMin ? Row.Min : -TNumericLimits<double>::Max(),
            Row.bHasMax ? Row.Max : TNumericLimits<double>::Max());
        return false;
    }

    bool ReportMissingRequired(EPwSynthFxKind Kind, const TCHAR* Key, const FPwSynthParamSpec& Row,
        FString& OutErrorCode, FString& OutError)
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_PARAMS;
        OutError = FString::Printf(TEXT("%s is required and was not supplied (%s)"),
            *QualifiedName(Kind, Key), Row.Meaning ? Row.Meaning : TEXT("no default is safe"));
        return false;
    }

    bool ReadNumber(EPwSynthFxKind Kind, const FPwSynthParams& Params, const TCHAR* Key,
        double& Out, FString& OutErrorCode, FString& OutError)
    {
        const FPwSynthParamSpec* Row = FindRow(Kind, Key);
        if (Row == nullptr)
        {
            return ReportMissingRow(Kind, Key, OutErrorCode, OutError);
        }

        const FName Name(Key);
        if (!Params.Has(Name))
        {
            if (Row->bRequired)
            {
                return ReportMissingRequired(Kind, Key, *Row, OutErrorCode, OutError);
            }
            Out = Row->DefaultNumber;
            return true;
        }

        Out = Params.GetNumber(Name);
        if (!FMath::IsFinite(Out)
            || (Row->bHasMin && Out < Row->Min)
            || (Row->bHasMax && Out > Row->Max))
        {
            return ReportOutOfRange(Kind, Key, *Row, Out, OutErrorCode, OutError);
        }
        return true;
    }

    bool ReadInt(EPwSynthFxKind Kind, const FPwSynthParams& Params, const TCHAR* Key,
        int32& Out, FString& OutErrorCode, FString& OutError)
    {
        double AsNumber = 0.0;
        if (!ReadNumber(Kind, Params, Key, AsNumber, OutErrorCode, OutError))
        {
            return false;
        }
        Out = static_cast<int32>(FMath::RoundToDouble(AsNumber));
        return true;
    }

    bool ReadBool(EPwSynthFxKind Kind, const FPwSynthParams& Params, const TCHAR* Key,
        bool& bOut, FString& OutErrorCode, FString& OutError)
    {
        const FPwSynthParamSpec* Row = FindRow(Kind, Key);
        if (Row == nullptr)
        {
            return ReportMissingRow(Kind, Key, OutErrorCode, OutError);
        }

        const FName Name(Key);
        if (!Params.Has(Name))
        {
            if (Row->bRequired)
            {
                return ReportMissingRequired(Kind, Key, *Row, OutErrorCode, OutError);
            }
            bOut = Row->DefaultNumber != 0.0;
            return true;
        }
        bOut = Params.GetBool(Name);
        return true;
    }

    bool ReadString(EPwSynthFxKind Kind, const FPwSynthParams& Params, const TCHAR* Key,
        FString& Out, FString& OutErrorCode, FString& OutError)
    {
        const FPwSynthParamSpec* Row = FindRow(Kind, Key);
        if (Row == nullptr)
        {
            return ReportMissingRow(Kind, Key, OutErrorCode, OutError);
        }

        const FName Name(Key);
        Out = Params.Has(Name)
            ? Params.GetString(Name)
            : FString(Row->DefaultString ? Row->DefaultString : TEXT(""));

        if (Row->bRequired && Out.IsEmpty())
        {
            return ReportMissingRequired(Kind, Key, *Row, OutErrorCode, OutError);
        }
        return true;
    }

    /**
     * Reads a closed-set parameter and returns its index in the row's own pipe-separated
     * vocabulary. An unrecognised value is an error naming the whole valid set - never a
     * fallback to the first entry (rpc-design.md §3).
     */
    bool ReadEnumIndex(EPwSynthFxKind Kind, const FPwSynthParams& Params, const TCHAR* Key,
        int32& OutIndex, FString& OutErrorCode, FString& OutError)
    {
        const FPwSynthParamSpec* Row = FindRow(Kind, Key);
        if (Row == nullptr || Row->EnumValues == nullptr)
        {
            return ReportMissingRow(Kind, Key, OutErrorCode, OutError);
        }

        FString Value;
        if (!ReadString(Kind, Params, Key, Value, OutErrorCode, OutError))
        {
            return false;
        }

        TArray<FString> Allowed;
        FString(Row->EnumValues).ParseIntoArray(Allowed, TEXT("|"), /*InCullEmpty=*/true);
        for (int32 Index = 0; Index < Allowed.Num(); ++Index)
        {
            if (Allowed[Index].Equals(Value, ESearchCase::IgnoreCase))
            {
                OutIndex = Index;
                return true;
            }
        }

        OutErrorCode = ErrorCodes::ERR_INVALID_PARAMS;
        OutError = FString::Printf(TEXT("%s is '%s'; the valid values are %s"),
            *QualifiedName(Kind, Key), *Value, Row->EnumValues);
        return false;
    }

    // ---------------------------------------------------------------------
    // Wet/dry
    // ---------------------------------------------------------------------

    /**
     * True when the effect is fully dry. Callers return success WITHOUT touching the span,
     * which is what makes mix == 0 bit-identical: computing In*1 + Wet*0 is only
     * bit-identical while every wet sample stays finite, and a resonant feedback path is
     * exactly where that stops being true.
     */
    bool IsFullyDry(double Mix)
    {
        return Mix <= 0.0;
    }

    /** Channel[i] = Channel[i] * (1 - Mix) + Wet[i] * Mix. */
    void BlendWet(float* Channel, const float* Wet, int32 NumFrames, double Mix)
    {
        if (Mix >= 1.0)
        {
            FMemory::Memcpy(Channel, Wet, static_cast<SIZE_T>(NumFrames) * sizeof(float));
            return;
        }
        const float WetGain = static_cast<float>(Mix);
        const float DryGain = static_cast<float>(1.0 - Mix);
        for (int32 Index = 0; Index < NumFrames; ++Index)
        {
            Channel[Index] = Channel[Index] * DryGain + Wet[Index] * WetGain;
        }
    }

    /**
     * Audio::FBiquadFilter clamps every cutoff into [5 Hz, 0.9 * Nyquist] and says nothing
     * about it (Filter.cpp:63). A caller-supplied band frequency outside that window would
     * therefore be filtered at a frequency nobody asked for and reported as applied, so the
     * usable window is published here and checked instead (rpc-design.md §1/§3).
     *
     * Note this is a function of the RENDER rate, not of the schema: the schema's `midHz`
     * row reaches 20 kHz, which is above 0.9 * Nyquist for every sample rate below ~44.4 kHz.
     */
    constexpr double MinBiquadCutoffHz = 5.0;

    double MaxBiquadCutoffHz(int32 SampleRate)
    {
        return 0.9 * 0.5 * static_cast<double>(SampleRate);
    }

    bool RequireFilterFrequency(EPwSynthFxKind Kind, const TCHAR* Key, double Hz, int32 SampleRate,
        FString& OutErrorCode, FString& OutError)
    {
        const double MaxHz = MaxBiquadCutoffHz(SampleRate);
        if (Hz >= MinBiquadCutoffHz && Hz <= MaxHz)
        {
            return true;
        }
        OutErrorCode = ErrorCodes::ERR_INVALID_PARAMS;
        OutError = FString::Printf(
            TEXT("%s is %g Hz, outside the %g..%g Hz a biquad can realise at the %d Hz render rate; Audio::FBiquadFilter would silently clamp it and the rendered band would not be the one requested. Lower the frequency, or raise the recipe's sampleRate."),
            *QualifiedName(Kind, Key), Hz, MinBiquadCutoffHz, MaxHz, SampleRate);
        return false;
    }

    /**
     * Q -> Audio::FBiquadFilter bandwidth in octaves. The engine takes OCTAVES, not Q, and
     * builds alpha as sin(w0) * sinh(0.5 * ln2 * BW * w0 / sin(w0)) (Filter.cpp:281). Setting
     * that equal to the RBJ Q form alpha = sin(w0) / 2Q and solving exactly gives
     *
     *     BW = 2 * asinh(1 / 2Q) * sin(w0) / (ln2 * w0)
     *
     * The sin(w0)/w0 factor is NOT optional: it is 0.997 at 1 kHz / 48 kHz, where dropping it
     * looks harmless, but 0.73 at 10 kHz / 48 kHz - a 27% error in the rendered -3 dB width.
     * FMath has no Asinh, so it is spelled out as log(x + sqrt(x^2 + 1)).
     *
     * CenterHz must already be inside the window RequireFilterFrequency accepts, so this
     * agrees with the omega the filter itself will compute.
     */
    float QToBandwidthOctaves(double Q, double CenterHz, int32 SampleRate)
    {
        const double X = 1.0 / (2.0 * FMath::Max(Q, UE_DOUBLE_SMALL_NUMBER));
        const double Asinh = FMath::Loge(X + FMath::Sqrt(X * X + 1.0));
        // Same ln(2) literal Filter.cpp:281 uses when it turns Bandwidth back into alpha.
        constexpr double NaturalLog2 = 0.69314718055994530942;

        const double Omega = 2.0 * UE_DOUBLE_PI * CenterHz / static_cast<double>(SampleRate);
        const double OmegaCorrection = (Omega > UE_DOUBLE_SMALL_NUMBER)
            ? FMath::Sin(Omega) / Omega
            : 1.0;

        return static_cast<float>(2.0 * Asinh * OmegaCorrection / NaturalLog2);
    }
}

// ===========================================================================
// chorus - N modulated delay taps summed against the dry signal
// ===========================================================================

bool PwFxChorus(const FPwSynthParams& Params, int32 SampleRate, FPwSeededRandom& /*Rng*/,
    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError)
{
    using namespace PwFxChainBInternal;
    constexpr EPwSynthFxKind Kind = EPwSynthFxKind::Chorus;

    if (!ValidateSpan(InOut, SampleRate, TEXT("chorus"), OutErrorCode, OutError))
    {
        return false;
    }

    double RateHz = 0.0;
    double DepthMs = 0.0;
    double Mix = 0.0;
    int32 Voices = 0;
    if (!ReadNumber(Kind, Params, TEXT("rateHz"), RateHz, OutErrorCode, OutError)
        || !ReadNumber(Kind, Params, TEXT("depthMs"), DepthMs, OutErrorCode, OutError)
        || !ReadNumber(Kind, Params, TEXT("mix"), Mix, OutErrorCode, OutError)
        || !ReadInt(Kind, Params, TEXT("voices"), Voices, OutErrorCode, OutError))
    {
        return false;
    }

    // Guards the fixed tap arrays against a widened spec row rather than overrunning them.
    if (Voices < 1 || Voices > MaxChorusVoices)
    {
        OutErrorCode = ErrorCodes::ERR_INTERNAL_ERROR;
        OutError = FString::Printf(
            TEXT("chorus.voices is %d; this implementation carries tap state for 1..%d"),
            Voices, MaxChorusVoices);
        return false;
    }

    if (IsFullyDry(Mix))
    {
        return true;
    }

    float* Channels[MaxSpanChannels] = { nullptr, nullptr };
    const int32 NumChannels = GatherChannels(InOut, Channels);
    const int32 NumFrames = InOut.NumFrames;

    TArray<float> Wet;
    Wet.SetNumUninitialized(NumFrames);

    for (int32 Channel = 0; Channel < NumChannels; ++Channel)
    {
        Audio::FDelay Taps[MaxChorusVoices];
        Audio::FLFO Lfos[MaxChorusVoices];

        // Voices are spread evenly around the LFO cycle so they never bunch together, and
        // the right channel is offset by half a voice step so a stereo master widens
        // instead of producing two identical mono chorus channels.
        const double ChannelPhaseOffset = (Channel == 1) ? 0.5 : 0.0;

        for (int32 Voice = 0; Voice < Voices; ++Voice)
        {
            Taps[Voice].Init(static_cast<float>(SampleRate), ChorusDelayLineSeconds);

            Lfos[Voice].Init(static_cast<float>(SampleRate));
            Lfos[Voice].SetType(Audio::ELFO::Triangle);
            Lfos[Voice].SetFrequency(static_cast<float>(RateHz));
            Lfos[Voice].SetPhaseOffset(static_cast<float>((Voice + ChannelPhaseOffset) / Voices));
            Lfos[Voice].Update();
            Lfos[Voice].Start();
        }

        const float* Dry = Channels[Channel];
        const float VoiceScale = 1.f / static_cast<float>(Voices);

        for (int32 Frame = 0; Frame < NumFrames; ++Frame)
        {
            float Sum = 0.f;
            for (int32 Voice = 0; Voice < Voices; ++Voice)
            {
                // Unipolar LFO: the tap sweeps ChorusMinDelayMs .. ChorusMinDelayMs + depthMs,
                // so depthMs is the peak-to-peak swing the schema says it is.
                const float Unipolar = Audio::GetUnipolar(Lfos[Voice].Generate());
                Taps[Voice].SetDelayMsec(static_cast<float>(ChorusMinDelayMs + Unipolar * DepthMs));
                Sum += Taps[Voice].Read();
                Taps[Voice].WriteDelayAndInc(Dry[Frame]);
            }
            Wet[Frame] = Sum * VoiceScale;
        }

        BlendWet(Channels[Channel], Wet.GetData(), NumFrames, Mix);
    }

    return true;
}

// ===========================================================================
// flanger - Audio::FFlanger with an external feedback loop
// ===========================================================================

bool PwFxFlanger(const FPwSynthParams& Params, int32 SampleRate, FPwSeededRandom& /*Rng*/,
    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError)
{
    using namespace PwFxChainBInternal;
    constexpr EPwSynthFxKind Kind = EPwSynthFxKind::Flanger;

    if (!ValidateSpan(InOut, SampleRate, TEXT("flanger"), OutErrorCode, OutError))
    {
        return false;
    }

    double RateHz = 0.0;
    double DepthMs = 0.0;
    double Mix = 0.0;
    double Feedback = 0.0;
    if (!ReadNumber(Kind, Params, TEXT("rateHz"), RateHz, OutErrorCode, OutError)
        || !ReadNumber(Kind, Params, TEXT("depthMs"), DepthMs, OutErrorCode, OutError)
        || !ReadNumber(Kind, Params, TEXT("mix"), Mix, OutErrorCode, OutError)
        || !ReadNumber(Kind, Params, TEXT("feedback"), Feedback, OutErrorCode, OutError))
    {
        return false;
    }

    if (IsFullyDry(Mix))
    {
        return true;
    }

    float* Channels[MaxSpanChannels] = { nullptr, nullptr };
    const int32 NumChannels = GatherChannels(InOut, Channels);
    const int32 NumFrames = InOut.NumFrames;

    TArray<float> Wet;
    Wet.SetNumUninitialized(NumFrames);

    for (int32 Channel = 0; Channel < NumChannels; ++Channel)
    {
        Audio::FFlanger Flanger;
        Flanger.Init(static_cast<float>(SampleRate));

        // Pure wet out of the engine class: the dry/wet blend is this file's, so `mix` == 0
        // can be an untouched buffer rather than a rounding-dependent identity.
        Flanger.SetMixLevel(1.f);

        // Order matters - SetModulationDepth clamps against CenterDelayMsec (Flanger.cpp:37-44).
        // Centre == depth makes the delay sweep 0 .. 2*depthMs, the classic through-zero shape.
        Flanger.SetCenterDelay(static_cast<float>(DepthMs));
        Flanger.SetModulationDepth(static_cast<float>(DepthMs));
        Flanger.SetModulationRate(static_cast<float>(RateHz));

        // One-frame blocks. FFlanger advances its LFO once per ProcessAudio call at
        // SampleRate/InNumSamples (Flanger.cpp:70), so a block of one is a per-sample LFO -
        // and it is the only block size at which the feedback path carries the delay line's
        // own delay plus one sample, instead of a whole block, which would add an audible
        // fixed comb the caller never asked for.
        Audio::FAlignedFloatBuffer InFrame;
        Audio::FAlignedFloatBuffer OutFrame;
        InFrame.AddZeroed(1);
        OutFrame.AddZeroed(1);

        const float* Dry = Channels[Channel];
        const float FeedbackGain = static_cast<float>(Feedback);
        float PreviousWet = 0.f;

        for (int32 Frame = 0; Frame < NumFrames; ++Frame)
        {
            InFrame[0] = Dry[Frame] + FeedbackGain * PreviousWet;
            Flanger.ProcessAudio(InFrame, 1, OutFrame);
            PreviousWet = OutFrame[0];
            Wet[Frame] = OutFrame[0];
        }

        BlendWet(Channels[Channel], Wet.GetData(), NumFrames, Mix);
    }

    return true;
}

// ===========================================================================
// phaser - swept all-pass chain with signed feedback
// ===========================================================================

bool PwFxPhaser(const FPwSynthParams& Params, int32 SampleRate, FPwSeededRandom& /*Rng*/,
    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError)
{
    using namespace PwFxChainBInternal;
    constexpr EPwSynthFxKind Kind = EPwSynthFxKind::Phaser;

    if (!ValidateSpan(InOut, SampleRate, TEXT("phaser"), OutErrorCode, OutError))
    {
        return false;
    }

    double RateHz = 0.0;
    double Depth = 0.0;
    double Mix = 0.0;
    double Feedback = 0.0;
    int32 Stages = 0;
    if (!ReadNumber(Kind, Params, TEXT("rateHz"), RateHz, OutErrorCode, OutError)
        || !ReadNumber(Kind, Params, TEXT("depth"), Depth, OutErrorCode, OutError)
        || !ReadNumber(Kind, Params, TEXT("mix"), Mix, OutErrorCode, OutError)
        || !ReadInt(Kind, Params, TEXT("stages"), Stages, OutErrorCode, OutError)
        || !ReadNumber(Kind, Params, TEXT("feedback"), Feedback, OutErrorCode, OutError))
    {
        return false;
    }

    if (Stages < 1 || Stages > MaxPhaserStages)
    {
        OutErrorCode = ErrorCodes::ERR_INTERNAL_ERROR;
        OutError = FString::Printf(
            TEXT("phaser.stages is %d; this implementation carries all-pass state for 1..%d"),
            Stages, MaxPhaserStages);
        return false;
    }

    if (IsFullyDry(Mix))
    {
        return true;
    }

    float* Channels[MaxSpanChannels] = { nullptr, nullptr };
    const int32 NumChannels = GatherChannels(InOut, Channels);
    const int32 NumFrames = InOut.NumFrames;

    TArray<float> Wet;
    Wet.SetNumUninitialized(NumFrames);

    // Stage bases climb PhaserStageSpreadOctaves from PhaserBaseHz, so a two-stage phaser
    // and a twelve-stage phaser cover the same band with different notch density.
    const double StageStepOctaves = (Stages > 1)
        ? PhaserStageSpreadOctaves / static_cast<double>(Stages - 1)
        : 0.0;

    for (int32 Channel = 0; Channel < NumChannels; ++Channel)
    {
        // Init before anything else touches these. A default-constructed FBiquadFilter holds
        // a null coefficient array (Filter.cpp:70) and SetFrequency / SetParams write through
        // it, so the sweep loop below must never reach a stage that was not initialized here:
        // it is bounded by the same `Stages` count, and Stages was range-checked above.
        Audio::FBiquadFilter AllPass[MaxPhaserStages];
        for (int32 Stage = 0; Stage < Stages; ++Stage)
        {
            AllPass[Stage].Init(static_cast<float>(SampleRate), /*InNumChannels=*/1,
                Audio::EBiquadFilter::AllPass, static_cast<float>(PhaserBaseHz),
                PhaserAllPassBandwidthOctaves);
        }

        // The LFO runs at the control rate, not the audio rate: it produces exactly one
        // value per coefficient update, which is the only rate the coefficients change at.
        Audio::FLFO Lfo;
        Lfo.Init(static_cast<float>(SampleRate) / static_cast<float>(PhaserControlBlockFrames));
        Lfo.SetType(Audio::ELFO::Sine);
        Lfo.SetFrequency(static_cast<float>(RateHz));
        // Quadrature on the right channel, the same trick Audio::FPhaser uses for its own
        // stereo image (Phaser.cpp:139).
        Lfo.SetPhaseOffset((Channel == 1) ? 0.25f : 0.f);
        Lfo.Update();
        Lfo.Start();

        const float* Dry = Channels[Channel];
        const float FeedbackGain = static_cast<float>(Feedback);
        float PreviousWet = 0.f;

        for (int32 BlockStart = 0; BlockStart < NumFrames; BlockStart += PhaserControlBlockFrames)
        {
            const float Unipolar = Audio::GetUnipolar(Lfo.Generate());
            const double SweepOctaves = static_cast<double>(Unipolar) * Depth * PhaserSweepOctaves;

            for (int32 Stage = 0; Stage < Stages; ++Stage)
            {
                const double StageHz = PhaserBaseHz
                    * FMath::Pow(2.0, Stage * StageStepOctaves + SweepOctaves);
                // Deliberately unlike eq: these frequencies are derived from `depth`, not
                // supplied by the caller, so riding FBiquadFilter's own [5, 0.9*Nyquist]
                // cutoff clamp (Filter.cpp:63) is the right behaviour - a sweep that would
                // run past Nyquist stops at Nyquist instead of failing the whole render. No
                // caller-named value is being silently substituted.
                AllPass[Stage].SetFrequency(static_cast<float>(StageHz));
            }

            const int32 BlockEnd = FMath::Min(BlockStart + PhaserControlBlockFrames, NumFrames);
            for (int32 Frame = BlockStart; Frame < BlockEnd; ++Frame)
            {
                float Sample = Dry[Frame] + FeedbackGain * PreviousWet;
                for (int32 Stage = 0; Stage < Stages; ++Stage)
                {
                    AllPass[Stage].ProcessAudioFrame(&Sample, &Sample);
                }
                PreviousWet = Sample;
                Wet[Frame] = Sample;
            }
        }

        BlendWet(Channels[Channel], Wet.GetData(), NumFrames, Mix);
    }

    return true;
}

// ===========================================================================
// ringmod - Audio::FRingModulation
// ===========================================================================

bool PwFxRingmod(const FPwSynthParams& Params, int32 SampleRate, FPwSeededRandom& /*Rng*/,
    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError)
{
    using namespace PwFxChainBInternal;
    constexpr EPwSynthFxKind Kind = EPwSynthFxKind::RingMod;

    if (!ValidateSpan(InOut, SampleRate, TEXT("ringmod"), OutErrorCode, OutError))
    {
        return false;
    }

    double RateHz = 0.0;
    double Mix = 0.0;
    int32 WaveformIndex = 0;
    if (!ReadNumber(Kind, Params, TEXT("rateHz"), RateHz, OutErrorCode, OutError)
        || !ReadNumber(Kind, Params, TEXT("mix"), Mix, OutErrorCode, OutError)
        || !ReadEnumIndex(Kind, Params, TEXT("waveform"), WaveformIndex, OutErrorCode, OutError))
    {
        return false;
    }

    // The schema's rateHz row is wider than the carrier oscillator the engine class drives.
    // Clamping here would render a different carrier than the recipe asked for and report
    // success, so the narrower window is stated as a rejection instead (rpc-design.md §1).
    if (RateHz < RingModMinCarrierHz || RateHz > RingModMaxCarrierHz)
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_PARAMS;
        OutError = FString::Printf(
            TEXT("ringmod.rateHz is %g Hz; Audio::FRingModulation clamps its carrier to %g..%g Hz, so a rate outside that window cannot be rendered as written"),
            RateHz, RingModMinCarrierHz, RingModMaxCarrierHz);
        return false;
    }

    // Index order matches the waveform row's own "sine|triangle|saw|square" vocabulary.
    static const Audio::EOsc::Type WaveformByIndex[] = {
        Audio::EOsc::Sine, Audio::EOsc::Triangle, Audio::EOsc::Saw, Audio::EOsc::Square
    };
    if (WaveformIndex < 0 || WaveformIndex >= static_cast<int32>(UE_ARRAY_COUNT(WaveformByIndex)))
    {
        OutErrorCode = ErrorCodes::ERR_INTERNAL_ERROR;
        OutError = FString::Printf(
            TEXT("ringmod.waveform resolved to index %d; the schema vocabulary and the oscillator table have drifted"),
            WaveformIndex);
        return false;
    }

    if (IsFullyDry(Mix))
    {
        return true;
    }

    float* Channels[MaxSpanChannels] = { nullptr, nullptr };
    const int32 NumChannels = GatherChannels(InOut, Channels);
    const int32 NumFrames = InOut.NumFrames;

    TArray<float> Wet;
    Wet.SetNumUninitialized(NumFrames);

    for (int32 Channel = 0; Channel < NumChannels; ++Channel)
    {
        Audio::FRingModulation Modulator;
        Modulator.Init(static_cast<float>(SampleRate), /*InNumChannels=*/1);
        // SetModulatorWaveType first: SetModulationFrequency is what re-runs Osc.Update().
        Modulator.SetModulatorWaveType(WaveformByIndex[WaveformIndex]);
        Modulator.SetModulationFrequency(static_cast<float>(RateHz));
        Modulator.SetModulationDepth(1.f);
        // Pure product out; the dry/wet blend is this file's, for the mix == 0 contract.
        Modulator.SetDryLevel(0.f);
        Modulator.SetWetLevel(1.f);

        Modulator.ProcessAudio(Channels[Channel], NumFrames, Wet.GetData());

        BlendWet(Channels[Channel], Wet.GetData(), NumFrames, Mix);
    }

    return true;
}

// ===========================================================================
// pitchshift - Audio::FTapDelayPitchShifter over an Audio::FDelay
// ===========================================================================

bool PwFxPitchshift(const FPwSynthParams& Params, int32 SampleRate, FPwSeededRandom& /*Rng*/,
    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError)
{
    using namespace PwFxChainBInternal;
    constexpr EPwSynthFxKind Kind = EPwSynthFxKind::PitchShift;

    if (!ValidateSpan(InOut, SampleRate, TEXT("pitchshift"), OutErrorCode, OutError))
    {
        return false;
    }

    double Semitones = 0.0;
    double Mix = 0.0;
    bool bFormantPreserve = false;
    if (!ReadNumber(Kind, Params, TEXT("semitones"), Semitones, OutErrorCode, OutError)
        || !ReadNumber(Kind, Params, TEXT("mix"), Mix, OutErrorCode, OutError)
        || !ReadBool(Kind, Params, TEXT("formantPreserve"), bFormantPreserve, OutErrorCode, OutError))
    {
        return false;
    }

    // Formant preservation needs a spectral-envelope estimate the time-domain shifter
    // cannot produce, and UE 5.8's SignalProcessing module ships no phase vocoder to build
    // one with. Rendering an ordinary shift and reporting success would be the exact defect
    // rpc-design.md §1 is about, so the flag is refused by name.
    if (bFormantPreserve)
    {
        OutErrorCode = ErrorCodes::ERR_UNSUPPORTED_OPTION;
        OutError = TEXT("pitchshift.formantPreserve is not supported: the engine's only pitch shifters (Audio::FTapDelayPitchShifter, Audio::FLinearPitchShifter) resample, which moves the formants with the pitch, and UE 5.8 SignalProcessing carries no phase vocoder to hold them still. Set formantPreserve to false, or shape the formants with a following eq or filter effect.");
        return false;
    }

    if (IsFullyDry(Mix))
    {
        return true;
    }

    float* Channels[MaxSpanChannels] = { nullptr, nullptr };
    const int32 NumChannels = GatherChannels(InOut, Channels);
    const int32 NumFrames = InOut.NumFrames;

    // The shifter's two taps read across a whole delay-line length, so the read window is
    // centred on the current frame by priming the line with half a length of input first.
    // Without the priming the whole output would sit PitchShiftDelayMs late, which reads as
    // a flam against the dry signal at any mix below 1. The cost is that the first and last
    // half-window of the span are drawn from a partially empty line and taper.
    // RoundToInt32, not RoundToInt: the double overload of RoundToInt returns int64, which
    // would make the FMath::Min template fail to deduce against an int32.
    const int32 PrimeFrames = FMath::Min(NumFrames,
        FMath::RoundToInt32(0.5 * PitchShiftDelayMs * SampleRate / 1000.0));

    TArray<float> Wet;
    Wet.SetNumUninitialized(NumFrames);

    for (int32 Channel = 0; Channel < NumChannels; ++Channel)
    {
        Audio::FDelay Line;
        Line.Init(static_cast<float>(SampleRate), PitchShiftDelayLineSeconds);

        Audio::FTapDelayPitchShifter Shifter;
        Shifter.Init(static_cast<float>(SampleRate), static_cast<float>(Semitones),
            static_cast<float>(PitchShiftDelayMs));

        const float* Dry = Channels[Channel];

        for (int32 Frame = 0; Frame < PrimeFrames; ++Frame)
        {
            Line.WriteDelayAndInc(Dry[Frame]);
        }

        for (int32 Frame = 0; Frame < NumFrames; ++Frame)
        {
            Wet[Frame] = Shifter.ReadDopplerShiftedTapFromDelay(Line);
            const int32 FeedFrame = Frame + PrimeFrames;
            Line.WriteDelayAndInc(FeedFrame < NumFrames ? Dry[FeedFrame] : 0.f);
        }

        BlendWet(Channels[Channel], Wet.GetData(), NumFrames, Mix);
    }

    return true;
}

// ===========================================================================
// compressor - Audio::FDynamicsProcessor
// ===========================================================================

bool PwFxCompressor(const FPwSynthParams& Params, int32 SampleRate, FPwSeededRandom& /*Rng*/,
    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError)
{
    using namespace PwFxChainBInternal;
    constexpr EPwSynthFxKind Kind = EPwSynthFxKind::Compressor;

    if (!ValidateSpan(InOut, SampleRate, TEXT("compressor"), OutErrorCode, OutError))
    {
        return false;
    }

    double ThresholdDb = 0.0;
    double Ratio = 0.0;
    double AttackMs = 0.0;
    double ReleaseMs = 0.0;
    double KneeDb = 0.0;
    double MakeupDb = 0.0;
    if (!ReadNumber(Kind, Params, TEXT("thresholdDb"), ThresholdDb, OutErrorCode, OutError)
        || !ReadNumber(Kind, Params, TEXT("ratio"), Ratio, OutErrorCode, OutError)
        || !ReadNumber(Kind, Params, TEXT("attackMs"), AttackMs, OutErrorCode, OutError)
        || !ReadNumber(Kind, Params, TEXT("releaseMs"), ReleaseMs, OutErrorCode, OutError)
        || !ReadNumber(Kind, Params, TEXT("kneeDb"), KneeDb, OutErrorCode, OutError)
        || !ReadNumber(Kind, Params, TEXT("makeupDb"), MakeupDb, OutErrorCode, OutError))
    {
        return false;
    }

    float* Channels[MaxSpanChannels] = { nullptr, nullptr };
    const int32 NumChannels = GatherChannels(InOut, Channels);
    const int32 NumFrames = InOut.NumFrames;

    Audio::FDynamicsProcessor Compressor;
    Compressor.Init(static_cast<float>(SampleRate), NumChannels);
    Compressor.SetProcessingMode(Audio::EDynamicsProcessingMode::Compressor);
    // Zero lookahead. FDynamicsProcessor defaults to 10 ms (DynamicsProcessor.cpp:14), which
    // would delay the whole span by 10 ms relative to every other effect in the chain.
    Compressor.SetLookaheadMsec(0.f);
    Compressor.SetAttackTime(static_cast<float>(AttackMs));
    Compressor.SetReleaseTime(static_cast<float>(ReleaseMs));
    Compressor.SetThreshold(static_cast<float>(ThresholdDb));
    Compressor.SetRatio(static_cast<float>(Ratio));
    Compressor.SetKneeBandwidth(static_cast<float>(KneeDb));
    // makeupDb is the post-compression trim; FDynamicsProcessor applies OutputGain after
    // the computed gain reduction, which is exactly what makeup means.
    Compressor.SetOutputGain(static_cast<float>(MakeupDb));
    // Linked detection on the master bus, so gain reduction does not pull the stereo image
    // toward whichever channel happened to be louder.
    Compressor.SetChannelLinkMode(NumChannels == 2
        ? Audio::EDynamicsProcessorChannelLinkMode::Average
        : Audio::EDynamicsProcessorChannelLinkMode::Disabled);

    // FDynamicsProcessor's block entry point is interleaved; the span is deinterleaved.
    const int32 NumSamples = NumFrames * NumChannels;
    TArray<float> Interleaved;
    TArray<float> Processed;
    Interleaved.SetNumUninitialized(NumSamples);
    Processed.SetNumUninitialized(NumSamples);

    for (int32 Frame = 0; Frame < NumFrames; ++Frame)
    {
        for (int32 Channel = 0; Channel < NumChannels; ++Channel)
        {
            Interleaved[Frame * NumChannels + Channel] = Channels[Channel][Frame];
        }
    }

    Compressor.ProcessAudio(Interleaved.GetData(), NumSamples, Processed.GetData());

    for (int32 Frame = 0; Frame < NumFrames; ++Frame)
    {
        for (int32 Channel = 0; Channel < NumChannels; ++Channel)
        {
            Channels[Channel][Frame] = Processed[Frame * NumChannels + Channel];
        }
    }

    return true;
}

// ===========================================================================
// eq - low shelf, parametric peak, high shelf
// ===========================================================================

bool PwFxEq(const FPwSynthParams& Params, int32 SampleRate, FPwSeededRandom& /*Rng*/,
    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError)
{
    using namespace PwFxChainBInternal;
    constexpr EPwSynthFxKind Kind = EPwSynthFxKind::Eq;

    if (!ValidateSpan(InOut, SampleRate, TEXT("eq"), OutErrorCode, OutError))
    {
        return false;
    }

    double LowGainDb = 0.0;
    double MidGainDb = 0.0;
    double HighGainDb = 0.0;
    double LowHz = 0.0;
    double MidHz = 0.0;
    double MidQ = 0.0;
    double HighHz = 0.0;
    if (!ReadNumber(Kind, Params, TEXT("lowGainDb"), LowGainDb, OutErrorCode, OutError)
        || !ReadNumber(Kind, Params, TEXT("midGainDb"), MidGainDb, OutErrorCode, OutError)
        || !ReadNumber(Kind, Params, TEXT("highGainDb"), HighGainDb, OutErrorCode, OutError)
        || !ReadNumber(Kind, Params, TEXT("lowHz"), LowHz, OutErrorCode, OutError)
        || !ReadNumber(Kind, Params, TEXT("midHz"), MidHz, OutErrorCode, OutError)
        || !ReadNumber(Kind, Params, TEXT("midQ"), MidQ, OutErrorCode, OutError)
        || !ReadNumber(Kind, Params, TEXT("highHz"), HighHz, OutErrorCode, OutError))
    {
        return false;
    }

    // The three band frequencies are caller-supplied, so an unrealisable one is rejected
    // rather than ridden into Audio::FBiquadFilter's silent cutoff clamp.
    if (!RequireFilterFrequency(Kind, TEXT("lowHz"), LowHz, SampleRate, OutErrorCode, OutError)
        || !RequireFilterFrequency(Kind, TEXT("midHz"), MidHz, SampleRate, OutErrorCode, OutError)
        || !RequireFilterFrequency(Kind, TEXT("highHz"), HighHz, SampleRate, OutErrorCode, OutError))
    {
        return false;
    }

    float* Channels[MaxSpanChannels] = { nullptr, nullptr };
    const int32 NumChannels = GatherChannels(InOut, Channels);
    const int32 NumFrames = InOut.NumFrames;

    // Audio::FBiquadFilter's shelf coefficients ignore Bandwidth entirely - they are the
    // fixed S = 1 RBJ shelves (Filter.cpp:358-384) - so only the peaking band converts a Q.
    const float ShelfBandwidthOctaves = 1.f;
    const float MidBandwidthOctaves = QToBandwidthOctaves(MidQ, MidHz, SampleRate);

    for (int32 Channel = 0; Channel < NumChannels; ++Channel)
    {
        // Init before anything else touches these: a default-constructed FBiquadFilter holds
        // a null coefficient array (Filter.cpp:70), and SetParams / SetFrequency / SetGainDB
        // all write through it. Every stage here is fully configured by its Init call, so no
        // setter runs on an uninitialized filter.
        Audio::FBiquadFilter LowShelf;
        Audio::FBiquadFilter MidPeak;
        Audio::FBiquadFilter HighShelf;
        LowShelf.Init(static_cast<float>(SampleRate), 1, Audio::EBiquadFilter::LowShelf,
            static_cast<float>(LowHz), ShelfBandwidthOctaves, static_cast<float>(LowGainDb));
        MidPeak.Init(static_cast<float>(SampleRate), 1, Audio::EBiquadFilter::ParametricEQ,
            static_cast<float>(MidHz), MidBandwidthOctaves, static_cast<float>(MidGainDb));
        HighShelf.Init(static_cast<float>(SampleRate), 1, Audio::EBiquadFilter::HighShelf,
            static_cast<float>(HighHz), ShelfBandwidthOctaves, static_cast<float>(HighGainDb));

        // The single-channel fast path reads each input sample before writing the matching
        // output sample (Filter.cpp:131-146), so in-place cascading is safe.
        float* Samples = Channels[Channel];
        LowShelf.ProcessAudio(Samples, NumFrames, Samples);
        MidPeak.ProcessAudio(Samples, NumFrames, Samples);
        HighShelf.ProcessAudio(Samples, NumFrames, Samples);
    }

    return true;
}

// ===========================================================================
// gain
// ===========================================================================

bool PwFxGain(const FPwSynthParams& Params, int32 SampleRate, FPwSeededRandom& /*Rng*/,
    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError)
{
    using namespace PwFxChainBInternal;
    constexpr EPwSynthFxKind Kind = EPwSynthFxKind::Gain;

    if (!ValidateSpan(InOut, SampleRate, TEXT("gain"), OutErrorCode, OutError))
    {
        return false;
    }

    double GainDb = 0.0;
    if (!ReadNumber(Kind, Params, TEXT("gainDb"), GainDb, OutErrorCode, OutError))
    {
        return false;
    }

    // Audio::ConvertToLinear is Pow(10, dB/20); 0 dB comes out as exactly 1.0f, so an
    // explicit 0 dB gain leaves the span bit-identical without a special case.
    const float Linear = Audio::ConvertToLinear(static_cast<float>(GainDb));

    float* Channels[MaxSpanChannels] = { nullptr, nullptr };
    const int32 NumChannels = GatherChannels(InOut, Channels);

    for (int32 Channel = 0; Channel < NumChannels; ++Channel)
    {
        float* Samples = Channels[Channel];
        for (int32 Frame = 0; Frame < InOut.NumFrames; ++Frame)
        {
            Samples[Frame] *= Linear;
        }
    }

    return true;
}

// ===========================================================================
// width - mid/side, master bus only
// ===========================================================================

bool PwFxWidth(const FPwSynthParams& Params, int32 SampleRate, FPwSeededRandom& /*Rng*/,
    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError)
{
    using namespace PwFxChainBInternal;
    constexpr EPwSynthFxKind Kind = EPwSynthFxKind::Width;

    if (!ValidateSpan(InOut, SampleRate, TEXT("width"), OutErrorCode, OutError))
    {
        return false;
    }

    // The schema already marks width bMasterOnly, so the parser rejects it inside a layer
    // chain. Defended here anyway, and named rather than no-oped: a silent pass-through
    // would be a stereo control that reports success and cannot possibly have done
    // anything, which is exactly what §1 exists to stop.
    if (!InOut.IsStereo())
    {
        OutErrorCode = ErrorCodes::ERR_UNSUPPORTED_OPERATION;
        OutError = TEXT("width operates on the stereo mix bus and was handed a mono span. Layers are mono by construction, so move the width effect into master.fx.");
        return false;
    }

    double Width = 0.0;
    if (!ReadNumber(Kind, Params, TEXT("width"), Width, OutErrorCode, OutError))
    {
        return false;
    }

    // Width 1 is defined as "leave the image alone", and the mid/side round trip is not
    // bit-exact in float: (L+R)/2 + (L-R)/2 loses the smaller operand when the two channels
    // differ by more than 24 bits of mantissa. Identity is therefore an untouched buffer.
    if (Width == 1.0)
    {
        return true;
    }

    const float Side = static_cast<float>(Width);
    float* Left = InOut.Left;
    float* Right = InOut.Right;

    for (int32 Frame = 0; Frame < InOut.NumFrames; ++Frame)
    {
        const float Mid = (Left[Frame] + Right[Frame]) * 0.5f;
        const float Difference = (Left[Frame] - Right[Frame]) * 0.5f * Side;
        Left[Frame] = Mid + Difference;
        Right[Frame] = Mid - Difference;
    }

    return true;
}

// ===========================================================================
// reverse
// ===========================================================================

bool PwFxReverse(const FPwSynthParams& /*Params*/, int32 SampleRate, FPwSeededRandom& /*Rng*/,
    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError)
{
    using namespace PwFxChainBInternal;

    if (!ValidateSpan(InOut, SampleRate, TEXT("reverse"), OutErrorCode, OutError))
    {
        return false;
    }

    // No parameter rows at all (the reverse spec table is empty), so there is nothing to
    // validate and nothing to blend: a reversal is exact and self-inverse.
    Algo::Reverse(InOut.Left, InOut.NumFrames);
    if (InOut.IsStereo())
    {
        Algo::Reverse(InOut.Right, InOut.NumFrames);
    }

    return true;
}

// ===========================================================================
// convolve - Audio::IConvolutionAlgorithm against an impulse-response asset
// ===========================================================================

bool PwFxConvolve(const FPwSynthParams& Params, int32 SampleRate, FPwSeededRandom& /*Rng*/,
    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError)
{
    using namespace PwFxChainBInternal;
    constexpr EPwSynthFxKind Kind = EPwSynthFxKind::Convolve;

    if (!ValidateSpan(InOut, SampleRate, TEXT("convolve"), OutErrorCode, OutError))
    {
        return false;
    }

    FString ImpulsePath;
    double Mix = 0.0;
    bool bNormalize = false;
    if (!ReadString(Kind, Params, TEXT("impulsePath"), ImpulsePath, OutErrorCode, OutError)
        || !ReadNumber(Kind, Params, TEXT("mix"), Mix, OutErrorCode, OutError)
        || !ReadBool(Kind, Params, TEXT("normalize"), bNormalize, OutErrorCode, OutError))
    {
        return false;
    }

    // Resolved BEFORE the mix == 0 early-out on purpose: a typo'd impulse path that returns
    // success because the effect happened to be fully dry is a recipe that looks correct and
    // renders nothing the moment the mix is raised.
    FPwAudioBuffer Impulse;
    if (!PwResolveSourceBuffer(ImpulsePath, Impulse, OutErrorCode, OutError))
    {
        return false;
    }

    const int32 SourceFrames = Impulse.NumFrames();
    if (SourceFrames <= 0)
    {
        OutErrorCode = ErrorCodes::ERR_AUDIO_EMPTY_BUFFER;
        OutError = FString::Printf(
            TEXT("convolve.impulsePath '%s' resolved to a buffer with no frames; there is no impulse response to convolve with"),
            *ImpulsePath);
        return false;
    }
    if (Impulse.SampleRate <= 0)
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_PARAMS;
        OutError = FString::Printf(
            TEXT("convolve.impulsePath '%s' resolved with a sample rate of %d, so its duration in seconds is unknown and it cannot be matched to the %d Hz render"),
            *ImpulsePath, Impulse.SampleRate, SampleRate);
        return false;
    }

    float* Channels[MaxSpanChannels] = { nullptr, nullptr };
    const int32 NumChannels = GatherChannels(InOut, Channels);
    const int32 NumFrames = InOut.NumFrames;

    // CHANNEL POLICY, chosen deliberately: one impulse response per span channel - left
    // convolves with the impulse's left, right with its right. A mono impulse arrives
    // duplicated across both FPwAudioBuffer channels (PwAudioBuffer.h), so a mono impulse on
    // a stereo bus is dual-mono and a true-stereo impulse keeps its own channel character.
    // The rejected alternative was collapsing the impulse to mono, which would silently throw
    // away half of a true-stereo impulse; taking only Left would do the same thing while
    // looking correct on every mono impulse anyone happened to test with.
    const float* SourceIr[MaxSpanChannels] = { Impulse.Left.GetData(), Impulse.Right.GetData() };
    if (NumChannels == 2 && Impulse.Right.Num() < SourceFrames)
    {
        OutErrorCode = ErrorCodes::ERR_AUDIO_EMPTY_BUFFER;
        OutError = FString::Printf(
            TEXT("convolve.impulsePath '%s' resolved to a buffer whose channels disagree in length (%d vs %d frames)"),
            *ImpulsePath, Impulse.Left.Num(), Impulse.Right.Num());
        return false;
    }

    // RESAMPLE TO THE RENDER RATE. PwResolveSourceBuffer hands back the asset at its NATIVE
    // rate - it cannot know what rate this render is at - so matching the two is this
    // consumer's job. Convolving a 44.1 kHz impulse straight into a 48 kHz render stretches
    // it by 8.8%: the tail gets longer and every resonance moves down, which reads as a
    // wrong-sounding reverb rather than as a bug.
    //
    // Audio::ISampleRateConverter::ProcessFullbuffer is the offline whole-buffer entry point
    // (DSP/SampleRateConverter.h), which is the shape this needs;
    // Audio::FMultichannelLinearResampler is the streaming sibling and would mean building
    // circular-buffer plumbing for a one-shot conversion. Its ratio is INPUT frames consumed
    // per OUTPUT frame produced, so going from the impulse's rate to the render rate is
    // ImpulseRate / RenderRate. Converters are per channel and freshly constructed, so both
    // channels are resampled from the same starting phase.
    TArray<float> WorkingIr[MaxSpanChannels];
    if (Impulse.SampleRate == SampleRate)
    {
        for (int32 Channel = 0; Channel < NumChannels; ++Channel)
        {
            WorkingIr[Channel].Append(SourceIr[Channel], SourceFrames);
        }
    }
    else
    {
        const float FrameRatio = static_cast<float>(Impulse.SampleRate) / static_cast<float>(SampleRate);
        for (int32 Channel = 0; Channel < NumChannels; ++Channel)
        {
            TUniquePtr<Audio::ISampleRateConverter> Converter(
                Audio::ISampleRateConverter::CreateSampleRateConverter());
            if (!Converter.IsValid())
            {
                OutErrorCode = ErrorCodes::ERR_OPERATION_FAILED;
                OutError = FString::Printf(
                    TEXT("convolve could not create a sample-rate converter to bring impulse '%s' from %d Hz to %d Hz"),
                    *ImpulsePath, Impulse.SampleRate, SampleRate);
                return false;
            }
            Converter->Init(FrameRatio, /*InNumChannels=*/1);
            Converter->ProcessFullbuffer(SourceIr[Channel], SourceFrames, WorkingIr[Channel]);
        }

        // Both channels saw the same input length and the same ratio, so they agree; the trim
        // is a structural guarantee rather than an expected correction.
        int32 CommonFrames = WorkingIr[0].Num();
        for (int32 Channel = 1; Channel < NumChannels; ++Channel)
        {
            CommonFrames = FMath::Min(CommonFrames, WorkingIr[Channel].Num());
        }
        for (int32 Channel = 0; Channel < NumChannels; ++Channel)
        {
            WorkingIr[Channel].SetNum(CommonFrames);
        }

        if (CommonFrames <= 0)
        {
            OutErrorCode = ErrorCodes::ERR_AUDIO_EMPTY_BUFFER;
            OutError = FString::Printf(
                TEXT("convolve.impulsePath '%s' is %d frames at %d Hz, too short to resample to the %d Hz render rate"),
                *ImpulsePath, SourceFrames, Impulse.SampleRate, SampleRate);
            return false;
        }
    }

    const int32 ImpulseFrames = WorkingIr[0].Num();

    // Normalization scale, measured on the RESAMPLED impulse and applied before anything is
    // written. Unity ENERGY per the schema row, taken from the LOUDEST channel rather than the
    // summed energy of all of them: a per-channel sum would scale a dual-mono impulse by
    // 1/sqrt(2) purely because the bus is stereo, which would make even a unit impulse
    // quieten the signal.
    if (bNormalize)
    {
        double MaxChannelEnergy = 0.0;
        for (int32 Channel = 0; Channel < NumChannels; ++Channel)
        {
            double Energy = 0.0;
            for (int32 Frame = 0; Frame < ImpulseFrames; ++Frame)
            {
                const double Sample = WorkingIr[Channel][Frame];
                Energy += Sample * Sample;
            }
            MaxChannelEnergy = FMath::Max(MaxChannelEnergy, Energy);
        }
        if (MaxChannelEnergy <= 0.0)
        {
            OutErrorCode = ErrorCodes::ERR_AUDIO_EMPTY_BUFFER;
            OutError = FString::Printf(
                TEXT("convolve.impulsePath '%s' is silent, so it carries no energy to normalize to; convolving with it would return silence"),
                *ImpulsePath);
            return false;
        }
        const float NormalizeScale = static_cast<float>(1.0 / FMath::Sqrt(MaxChannelEnergy));
        for (int32 Channel = 0; Channel < NumChannels; ++Channel)
        {
            for (int32 Frame = 0; Frame < ImpulseFrames; ++Frame)
            {
                WorkingIr[Channel][Frame] *= NormalizeScale;
            }
        }
    }

    if (IsFullyDry(Mix))
    {
        return true;
    }

    Audio::FConvolutionSettings Settings;
    Settings.BlockNumSamples = ConvolutionBlockFrames;
    Settings.NumInputChannels = NumChannels;
    Settings.NumOutputChannels = NumChannels;
    Settings.NumImpulseResponses = NumChannels;
    Settings.MaxNumImpulseResponseSamples = ImpulseFrames;

    TUniquePtr<Audio::IConvolutionAlgorithm> Algorithm =
        Audio::FConvolutionFactory::NewConvolutionAlgorithm(Settings);
    if (!Algorithm.IsValid())
    {
        OutErrorCode = ErrorCodes::ERR_OPERATION_FAILED;
        OutError = FString::Printf(
            TEXT("convolve could not create a convolution algorithm for %d channel(s) and a %d-sample impulse response"),
            NumChannels, ImpulseFrames);
        return false;
    }

    for (int32 Channel = 0; Channel < NumChannels; ++Channel)
    {
        Algorithm->SetImpulseResponse(Channel, WorkingIr[Channel].GetData(), ImpulseFrames);
        // Gains default to zero, so every route has to be declared or the output is silent.
        Algorithm->SetMatrixGain(Channel, Channel, Channel, 1.f);
    }

    const int32 BlockFrames = Algorithm->GetNumSamplesInBlock();
    TArray<float> InBlockStorage;
    TArray<float> OutBlockStorage;
    InBlockStorage.SetNumZeroed(BlockFrames * NumChannels);
    OutBlockStorage.SetNumZeroed(BlockFrames * NumChannels);

    const float* InBlocks[MaxSpanChannels] = { nullptr, nullptr };
    float* OutBlocks[MaxSpanChannels] = { nullptr, nullptr };
    for (int32 Channel = 0; Channel < NumChannels; ++Channel)
    {
        InBlocks[Channel] = InBlockStorage.GetData() + Channel * BlockFrames;
        OutBlocks[Channel] = OutBlockStorage.GetData() + Channel * BlockFrames;
    }

    TArray<float> Wet[MaxSpanChannels];
    for (int32 Channel = 0; Channel < NumChannels; ++Channel)
    {
        Wet[Channel].SetNumZeroed(NumFrames);
    }

    // The convolution tail past the end of the span is discarded: the span is the render
    // window the recipe asked for, and growing it here would desynchronise every other
    // layer. A recipe that wants the tail asks for a longer durationMs.
    for (int32 BlockStart = 0; BlockStart < NumFrames; BlockStart += BlockFrames)
    {
        const int32 FramesThisBlock = FMath::Min(BlockFrames, NumFrames - BlockStart);
        for (int32 Channel = 0; Channel < NumChannels; ++Channel)
        {
            float* Destination = InBlockStorage.GetData() + Channel * BlockFrames;
            FMemory::Memcpy(Destination, Channels[Channel] + BlockStart,
                static_cast<SIZE_T>(FramesThisBlock) * sizeof(float));
            if (FramesThisBlock < BlockFrames)
            {
                FMemory::Memzero(Destination + FramesThisBlock,
                    static_cast<SIZE_T>(BlockFrames - FramesThisBlock) * sizeof(float));
            }
        }

        Algorithm->ProcessAudioBlock(InBlocks, OutBlocks);

        for (int32 Channel = 0; Channel < NumChannels; ++Channel)
        {
            FMemory::Memcpy(Wet[Channel].GetData() + BlockStart,
                OutBlockStorage.GetData() + Channel * BlockFrames,
                static_cast<SIZE_T>(FramesThisBlock) * sizeof(float));
        }
    }

    for (int32 Channel = 0; Channel < NumChannels; ++Channel)
    {
        BlendWet(Channels[Channel], Wet[Channel].GetData(), NumFrames, Mix);
    }

    return true;
}
