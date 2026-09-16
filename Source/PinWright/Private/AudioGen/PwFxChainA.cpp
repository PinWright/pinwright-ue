// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwFxChainA.cpp - the four effects that carry most of the character in explosions, impacts and
// spaces: filter, distort, delay, reverb. Each processes an FPwDspSpan IN PLACE, and each handles
// both span shapes - Right == nullptr is a mono layer chain, both set is the stereo master bus.
//
// Every effect is built on a plain SignalProcessing DSP class, never on the Synthesis plugin's
// FSourceEffect* wrappers. Those wrappers are thin shells over these same inner classes and would
// drag an optional-plugin dependency into a module that has to load on a host with Synthesis
// disabled.
//
// Division of validation labour - this file does NOT re-run the parser. ParseSynthRecipe owns
// type / range / required-ness for every row of the fx spec tables in PwSynthRecipe.cpp. What is
// re-checked here is only what the DSP itself needs in order to stay well-defined when a params
// bag arrives that did not come through the parser (a unit test, a future caller): required values
// present, the two closed type vocabularies, the delay feedback ceiling, a positive delay time,
// and the span itself. One check is NOT redundant with the parser and cannot be moved into it:
// the filter's cutoff window is a function of the recipe's sampleRate, which no single spec-table
// row can express, and the engine's biquad silently clamps rather than complaining.
//
// Failure is the default (rpc-design.md §1): every rejection happens BEFORE a single sample is
// written, so a failed effect leaves the buffer bit-identical and the chain uncorrupted. There is
// no half-processed state to unwind because no processing has started.
//
// No silent fallback (rpc-design.md §3): an unrecognised filter type or distortion type is an
// error naming the valid set - read out of the spec table itself, so the message cannot drift away
// from the schema - never a degrade to "lowpass" or to a no-op.
//
// mix is honoured wherever the spec table defines one, which is distort, delay and reverb. The
// filter row has NO mix parameter, so a filter is always fully wet; that is the schema's decision,
// not an omission here. A mix of 0 returns before any processing runs, so the dry signal survives
// bit-for-bit by construction rather than by arithmetic (x * 1.0f + y * 0.0f is not bit-identical
// to x when x is -0.0f).
//
// Randomness: none of these four draw from Rng. That is deliberate and load-bearing - an effect
// that consumed the stream "just in case" would shift every later draw in the chain and silently
// change renders that never asked for it.
//
// Engine version: every symbol used here (FBiquadFilter, FWaveShaper, FFoldbackDistortion,
// FBitCrusher, FDelay, FOnePoleLPF, FPlateReverbFast) is present with the same signature on UE 5.3
// through 5.8, so this file owes no row in docs/engine-version-support.md. Two nearby symbols DO
// carry a floor and are deliberately avoided rather than guarded, because in both cases the
// portable spelling costs nothing:
//   * Audio::FIntegerDelay's per-sample entry point and its TArrayView ProcessAudio overload are
//     5.4+ (DSP/IntegerDelay.h:35,51 on 5.8; neither is on 5.3). The delay uses Audio::FDelay -
//     see PwFxDelay for the other, larger reason.
//   * FBiquadFilter's planar ProcessAudio(const float* const*, ...) overload is 5.4+
//     (DSP/Filter.h:56 on 5.8; absent on 5.3). PwFxFilter runs one single-channel filter per
//     planar channel instead, which is bit-identical - see the comment at that call site.

#include "AudioGen/PwSynthDsp.h"

#include "AudioGen/PwSeededRandom.h"
#include "AudioGen/PwSynthRecipe.h"
#include "Compat/EngineVersionCompat.h"
#include "Handlers/ErrorCodes.h"

#include "DSP/AlignedBuffer.h"
#include "DSP/BitCrusher.h"
#include "DSP/Delay.h"
#include "DSP/Filter.h"
#include "DSP/FoldbackDistortion.h"
#include "DSP/LateReflectionsFast.h"
#include "DSP/OnePole.h"
#include "DSP/ReverbFast.h"
#include "DSP/WaveShaper.h"

// Named (not anonymous) namespace with a file-specific prefix: the module builds with
// bUseUnity = true, so an anonymous namespace here can collide with another TU's helpers.
namespace PwFxChainAInternal
{
    // ---------------------------------------------------------------------------------------
    // Tunables. Each one is a documented constant rather than a literal at the point of use,
    // because each is a claim about the engine class it feeds.
    // ---------------------------------------------------------------------------------------

    /**
     * Ceiling on the delay's feedback coefficient, matching the `feedback` row's maximum in
     * DelayParams() exactly. The delay's loop gain is feedback * |damping one-pole|, and the
     * one-pole has unity DC gain, so the loop gain is exactly `feedback` at DC: at 1.0 the
     * recursion never decays and at anything above it the buffer grows without bound. A value
     * past this ceiling is rejected rather than clamped, matching the rule PwSynthRecipe.h
     * states for the schema as a whole ("Caps are validation errors, not clamps, because a
     * silently clamped recipe does not round-trip"). Because the ceiling IS the schema maximum,
     * no recipe that came through ParseSynthRecipe can ever hit this rejection.
     */
    constexpr double MaxDelayFeedback = 0.99;

    /**
     * The cutoff window Audio::FBiquadFilter will actually honour. These mirror
     * FBiquadFilter::ClampCutoffFrequency (Filter.cpp:64-67) exactly: it clamps into
     * [5 Hz, 0.45 * SampleRate] with no error, so a request outside the window would run a filter
     * the caller did not ask for. PwFxFilter rejects instead of clamping, so these constants are
     * the boundary of what a recipe may ask for at a given render rate.
     */
    constexpr double MinFilterCutoffHz = 5.0;
    constexpr double NyquistFraction = 0.45;

    /** Foldback's fold threshold, held fixed so `drive` is the only knob the schema exposes. */
    constexpr float FoldbackThresholdDb = -6.0206f;     // 0.5 linear, FFoldbackDistortion's own default

    /** Bit depth the `bitcrush` shape uses at drive 0 and at drive 100 (the schema's range). */
    constexpr float BitCrushDepthAtZeroDrive = 16.0f;
    constexpr float BitCrushDepthAtFullDrive = 1.0f;

    /**
     * DC bias fed into the waveshaper for the `tube` shape. Asymmetry is what separates a tube
     * curve from a plain soft clip - it is what produces even-order harmonics - and FWaveShaper
     * applies Bias before the curve, which is exactly the right place for it. The shaper's
     * zero-input response is subtracted afterwards so the asymmetry does not leave a DC step in
     * the layer (at drive 4 an uncorrected bias of 0.2 offsets the output by about 0.34).
     */
    constexpr float TubeBias = 0.2f;

    /**
     * Mean single-plate recirculation time of Audio::FLateReflectionsFast's Dattorro plate.
     * The recirculating path is ModulatedAPF + DelayA..D + APF + DelayG..I; DelayE/F hang off the
     * APF as taps and do not feed back. Summed from FLateReflectionsPlateDelays::DefaultLeftDelays
     * and DefaultRightDelays (LateReflectionsFast.cpp:62-107) that is 10917 and 10645 preset
     * samples, mean 10781, expressed at the hard-coded preset rate of 29761 Hz. Every delay is
     * scaled by SampleRate/29761 at construction, so this TIME is sample-rate invariant.
     */
    constexpr double PlateLoopSeconds = 10781.0 / 29761.0;      // 0.36225 s

    /** Frames per reverb block. Bounds the interleave scratch instead of mirroring the whole span. */
    constexpr int32 ReverbBlockFrames = 4096;

    /** ln(2), spelled out for the Q -> octave-bandwidth conversion; UE exposes only 1/ln(2). */
    constexpr double NaturalLog2 = 0.69314718055994530942;

    // ---------------------------------------------------------------------------------------
    // Shared plumbing.
    // ---------------------------------------------------------------------------------------

    bool Fail(const TCHAR* Code, const FString& Message, FString& OutErrorCode, FString& OutError)
    {
        OutErrorCode = Code;
        OutError = Message;
        return false;
    }

    /**
     * The closed vocabulary of an fx parameter, straight out of the spec table that
     * ParseSynthRecipe validates against. Reading it here rather than repeating the literal is
     * what stops the "valid types are ..." half of an error message from drifting away from the
     * set the parser actually accepts.
     */
    FString FxParamVocabulary(EPwSynthFxKind Kind, FName ParamName)
    {
        const FPwSynthKindSpec& Spec = PwSynthFxSpec(Kind);
        if (Spec.Params)
        {
            for (const FPwSynthParamSpec& Row : *Spec.Params)
            {
                if (Row.Name == ParamName && Row.EnumValues)
                {
                    return FString(Row.EnumValues);
                }
            }
        }
        return FString();
    }

    /** Planar channel pointers of a span: one entry for a mono layer chain, two for the master bus. */
    void GatherChannels(const FPwDspSpan& InOut, TArray<float*, TInlineAllocator<2>>& OutChannels)
    {
        OutChannels.Reset();
        OutChannels.Add(InOut.Left);
        if (InOut.IsStereo())
        {
            OutChannels.Add(InOut.Right);
        }
    }

    /**
     * Span and sample-rate sanity. A null Left or a negative frame count is a caller bug, not a
     * recipe problem, but it still has to fail loudly rather than walk off the buffer. A zero
     * frame count is a legitimate no-op and is reported as one by the caller, not as an error.
     */
    bool ValidateSpan(const FPwDspSpan& InOut, int32 SampleRate, const TCHAR* EffectName,
        FString& OutErrorCode, FString& OutError)
    {
        if (SampleRate <= 0)
        {
            return Fail(ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("%s: sample rate must be positive, got %d"), EffectName, SampleRate),
                OutErrorCode, OutError);
        }
        if (InOut.NumFrames < 0)
        {
            return Fail(ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("%s: frame count must not be negative, got %d"), EffectName, InOut.NumFrames),
                OutErrorCode, OutError);
        }
        if (InOut.NumFrames > 0 && InOut.Left == nullptr)
        {
            return Fail(ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("%s: span has %d frames but no left channel"), EffectName, InOut.NumFrames),
                OutErrorCode, OutError);
        }
        return true;
    }

    /** Presence check for a spec-table row the DSP cannot invent a value for. */
    bool RequireParam(const FPwSynthParams& Params, FName Name, const TCHAR* EffectName,
        FString& OutErrorCode, FString& OutError)
    {
        if (!Params.Has(Name))
        {
            return Fail(ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("%s: required parameter '%s' is missing"), EffectName, *Name.ToString()),
                OutErrorCode, OutError);
        }
        return true;
    }

    /**
     * Normalized cutoff in Audio::FOnePoleLPF / FBufferOnePoleLPF units: 0 = DC, 1 = Nyquist.
     * Both classes turn it into the pole coefficient as exp(-PI * normalized).
     */
    float NormalizedCutoff(double CutoffHz, int32 SampleRate)
    {
        return static_cast<float>(FMath::Clamp(2.0 * CutoffHz / static_cast<double>(SampleRate), 0.0, 1.0));
    }
}

// ===========================================================================================
// filter - Audio::FBiquadFilter (DSP/Filter.h)
//
// DSP/BiQuadFilter.h is a DIFFERENT header holding only a UE_DEPRECATED(5.5) FBiquad; the
// non-deprecated multi-channel class lives in DSP/Filter.h. IFilter is deliberately not
// subclassed - it grew a pure virtual in 5.8 - and FBiquadFilter is not derived from it anyway.
// ===========================================================================================
bool PwFxFilter(const FPwSynthParams& Params, int32 SampleRate, FPwSeededRandom& Rng,
    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError)
{
    using namespace PwFxChainAInternal;

    OutErrorCode.Reset();
    OutError.Reset();

    const TCHAR* const EffectName = TEXT("filter");
    const FName TypeKey(TEXT("type"));
    const FName CutoffKey(TEXT("cutoffHz"));
    const FName ResonanceKey(TEXT("resonance"));
    const FName GainKey(TEXT("gainDb"));

    if (!ValidateSpan(InOut, SampleRate, EffectName, OutErrorCode, OutError)
        || !RequireParam(Params, TypeKey, EffectName, OutErrorCode, OutError)
        || !RequireParam(Params, CutoffKey, EffectName, OutErrorCode, OutError))
    {
        return false;
    }

    // Closed set. An unrecognised response is an error naming the valid set (rpc-design §3);
    // EBiquadFilter also offers AllPass and the two Butterworth types, which the schema does not
    // expose, so they are unreachable rather than silently available under another spelling.
    const FString TypeName = Params.GetString(TypeKey);
    Audio::EBiquadFilter::Type FilterType = Audio::EBiquadFilter::Lowpass;
    if (TypeName.Equals(TEXT("lowpass"), ESearchCase::IgnoreCase))        { FilterType = Audio::EBiquadFilter::Lowpass; }
    else if (TypeName.Equals(TEXT("highpass"), ESearchCase::IgnoreCase))  { FilterType = Audio::EBiquadFilter::Highpass; }
    else if (TypeName.Equals(TEXT("bandpass"), ESearchCase::IgnoreCase))  { FilterType = Audio::EBiquadFilter::Bandpass; }
    else if (TypeName.Equals(TEXT("notch"), ESearchCase::IgnoreCase))     { FilterType = Audio::EBiquadFilter::Notch; }
    else if (TypeName.Equals(TEXT("lowshelf"), ESearchCase::IgnoreCase))  { FilterType = Audio::EBiquadFilter::LowShelf; }
    else if (TypeName.Equals(TEXT("highshelf"), ESearchCase::IgnoreCase)) { FilterType = Audio::EBiquadFilter::HighShelf; }
    else if (TypeName.Equals(TEXT("peaking"), ESearchCase::IgnoreCase))   { FilterType = Audio::EBiquadFilter::ParametricEQ; }
    else
    {
        return Fail(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("filter: unrecognised type '%s'; valid types are %s"),
                *TypeName, *FxParamVocabulary(EPwSynthFxKind::Filter, TypeKey)),
            OutErrorCode, OutError);
    }

    const double CutoffHz = Params.GetNumber(CutoffKey);
    const double Resonance = Params.GetNumber(ResonanceKey, 0.707);
    const double GainDb = Params.GetNumber(GainKey, 0.0);

    // FBiquadFilter does NOT reject a cutoff it cannot realise - it silently clamps into
    // [MinFilterCutoffHz, NyquistFraction * SampleRate] (Filter.cpp:64-67) and runs a filter the
    // caller never asked for. That is §1's defect class (answering a narrower question than the
    // one asked) with no channel to report it on, so the out-of-range request is rejected instead
    // (§3). The window is a cross-field constraint - the schema allows cutoffHz up to 20 kHz and
    // sampleRate down to 8 kHz - which no single spec-table row can express, so ParseSynthRecipe
    // cannot catch it and this is the only place it can be caught. The message names the render
    // rate as well as the window, because either one is what the caller has to change.
    const double MaxCutoffHz = NyquistFraction * static_cast<double>(SampleRate);
    if (CutoffHz < MinFilterCutoffHz || CutoffHz > MaxCutoffHz)
    {
        return Fail(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("filter: cutoffHz %g is outside the usable window [%g, %g] at a ")
                TEXT("%d Hz render rate; the biquad would silently clamp it. Lower cutoffHz or ")
                TEXT("raise the recipe's sampleRate"),
                CutoffHz, MinFilterCutoffHz, MaxCutoffHz, SampleRate),
            OutErrorCode, OutError);
    }

    if (InOut.NumFrames == 0)
    {
        return true;
    }

    // resonance -> bandwidth. FBiquadFilter::Init takes BANDWIDTH IN OCTAVES, not Q, and the
    // schema's `resonance` is a Q factor. The two RBJ-cookbook spellings of the biquad's alpha
    // have to agree:
    //     alpha = sin(w0) * sinh( ln(2)/2 * BW * w0 / sin(w0) )   the bandwidth form the engine
    //                                                             computes (Filter.cpp:284)
    //     alpha = sin(w0) / (2Q)                                  the Q form the schema means
    // Equating and solving for BW:
    //     BW = 2 * asinh(1 / (2Q)) * sin(w0) / (ln(2) * w0)
    // This is the exact inversion, not an approximation, so the realised -3 dB width is the width
    // the requested Q describes. asinh is spelled out as ln(x + sqrt(x*x + 1)) rather than reached
    // for in FMath. The cutoff rejection above guarantees w0 <= 2*PI*NyquistFraction, where
    // sin(w0) is 0.309 and can never reach 0.
    //
    // Sanity check: Q = 0.707 at a cutoff well below Nyquist gives BW = 1.90 octaves, the textbook
    // pairing for a Butterworth-Q biquad.
    const double Omega = 2.0 * UE_DOUBLE_PI * CutoffHz / static_cast<double>(SampleRate);
    const double SafeQ = FMath::Max(Resonance, UE_DOUBLE_KINDA_SMALL_NUMBER);
    const double HalfInvQ = 1.0 / (2.0 * SafeQ);
    const double ArcSinh = FMath::Loge(HalfInvQ + FMath::Sqrt(HalfInvQ * HalfInvQ + 1.0));
    const double BandwidthOctaves = 2.0 * ArcSinh * FMath::Sin(Omega) / (NaturalLog2 * Omega);

    TArray<float*, TInlineAllocator<2>> Channels;
    GatherChannels(InOut, Channels);

    // Init FIRST, and never SetParams on a fresh instance: FBiquadFilter's constructor leaves its
    // per-channel coefficient array null and only Init allocates it, so SetParams on an un-Init'd
    // filter dereferences null. Init takes the full parameter set anyway, so this call site has no
    // reason to ever reach for SetParams.
    Audio::FBiquadFilter Filter;
    Filter.Init(static_cast<float>(SampleRate), /*InNumChannels=*/1, FilterType,
        static_cast<float>(CutoffHz), static_cast<float>(BandwidthOctaves), static_cast<float>(GainDb));

    // One single-channel filter run once per planar channel, with Reset() clearing the state
    // between them. FBiquadFilter also has a planar ProcessAudio(const float* const*, ...) overload
    // that matches an FPwDspSpan exactly, but it is 5.4+ (absent from DSP/Filter.h on 5.3) and it
    // buys nothing here: it keeps one independent FBiquadCoeff per channel over one shared set of
    // coefficients, which is precisely what re-running a single-channel filter after a Reset()
    // produces, sample for sample. Processing in place is safe - the engine reads the source
    // sample into a local before writing the destination at the same index.
    for (int32 ChannelIndex = 0; ChannelIndex < Channels.Num(); ++ChannelIndex)
    {
        if (ChannelIndex > 0)
        {
            Filter.Reset();
        }
        Filter.ProcessAudio(Channels[ChannelIndex], InOut.NumFrames, Channels[ChannelIndex]);
    }

    return true;
}

// ===========================================================================================
// distort - Audio::FWaveShaper / FFoldbackDistortion / FBitCrusher
// ===========================================================================================
bool PwFxDistort(const FPwSynthParams& Params, int32 SampleRate, FPwSeededRandom& Rng,
    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError)
{
    using namespace PwFxChainAInternal;

    OutErrorCode.Reset();
    OutError.Reset();

    const TCHAR* const EffectName = TEXT("distort");
    const FName TypeKey(TEXT("type"));
    const FName DriveKey(TEXT("drive"));
    const FName MixKey(TEXT("mix"));
    const FName OutGainKey(TEXT("outGainDb"));

    if (!ValidateSpan(InOut, SampleRate, EffectName, OutErrorCode, OutError)
        || !RequireParam(Params, TypeKey, EffectName, OutErrorCode, OutError)
        || !RequireParam(Params, DriveKey, EffectName, OutErrorCode, OutError))
    {
        return false;
    }

    // Closed set, same rule as the filter's: unrecognised is an error naming the valid set.
    enum class EShape { SoftTanh, HardClip, Tube, Foldback, BitCrush };
    const FString TypeName = Params.GetString(TypeKey);
    EShape Shape = EShape::SoftTanh;
    if (TypeName.Equals(TEXT("soft"), ESearchCase::IgnoreCase))          { Shape = EShape::SoftTanh; }
    else if (TypeName.Equals(TEXT("hard"), ESearchCase::IgnoreCase))     { Shape = EShape::HardClip; }
    else if (TypeName.Equals(TEXT("tube"), ESearchCase::IgnoreCase))     { Shape = EShape::Tube; }
    else if (TypeName.Equals(TEXT("foldback"), ESearchCase::IgnoreCase)) { Shape = EShape::Foldback; }
    else if (TypeName.Equals(TEXT("bitcrush"), ESearchCase::IgnoreCase)) { Shape = EShape::BitCrush; }
    else
    {
        return Fail(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("distort: unrecognised type '%s'; valid types are %s"),
                *TypeName, *FxParamVocabulary(EPwSynthFxKind::Distort, TypeKey)),
            OutErrorCode, OutError);
    }

    const double Drive = Params.GetNumber(DriveKey);
    const double Mix = Params.GetNumber(MixKey, 1.0);
    const double OutGainDb = Params.GetNumber(OutGainKey, 0.0);

    // Fully dry: return before touching a sample, so the guarantee is structural.
    if (InOut.NumFrames == 0 || Mix <= 0.0)
    {
        return true;
    }

    // outGainDb is documented as a POST-SHAPER trim, so it scales the wet path only. Folding it
    // into the blend rather than into each engine class's own output gain keeps the five shapes
    // uniform (FBitCrusher has no output-gain setter at all) and keeps mix = 0 exactly dry.
    const float WetGain = FMath::Pow(10.0f, static_cast<float>(OutGainDb) / 20.0f);
    const float DryScale = static_cast<float>(1.0 - Mix);
    const float WetScale = static_cast<float>(Mix) * WetGain;

    // FWaveShaper clamps Amount to a floor of 1.0, so drive 0 is the gentlest curve the class
    // offers rather than a bypass. That is honest for each shape: normalized tanh at Amount 1
    // still adds a little odd-harmonic colour, and hard clipping at unity pre-gain genuinely does
    // nothing to a signal that never reaches full scale. The bypass is mix = 0.
    const float ShaperAmount = static_cast<float>(1.0 + Drive);

    TArray<float*, TInlineAllocator<2>> Channels;
    GatherChannels(InOut, Channels);

    TArray<float> Wet;
    Wet.SetNumUninitialized(InOut.NumFrames);

    for (float* Channel : Channels)
    {
        // A fresh instance per channel: the memoryless shapes do not care, but FBitCrusher carries
        // sample-and-hold state, and giving each channel its own copy is what makes L and R get
        // identical treatment instead of interleaved treatment.
        switch (Shape)
        {
        case EShape::SoftTanh:
        case EShape::HardClip:
        case EShape::Tube:
        {
            Audio::FWaveShaper Shaper;
            Shaper.Init(static_cast<float>(SampleRate));
            Shaper.SetType(Shape == EShape::HardClip ? Audio::EWaveShaperType::HardClip
                : Shape == EShape::Tube ? Audio::EWaveShaperType::ATan
                : Audio::EWaveShaperType::Tanh);
            Shaper.SetAmount(ShaperAmount);
            Shaper.SetBias(Shape == EShape::Tube ? TubeBias : 0.0f);
            Shaper.SetOutputGainLinear(1.0f);       // wet trim is applied in the blend below
            Shaper.ProcessAudioBuffer(Channel, Wet.GetData(), InOut.NumFrames);

            if (Shape == EShape::Tube)
            {
                // Subtract the shaper's own zero-input response. The bias is what makes the curve
                // asymmetric (even harmonics, the point of the shape), but it also shifts silence
                // off zero; measuring the offset by pushing one zero sample through the same
                // memoryless shaper removes it without duplicating the curve's maths here.
                const float ZeroIn = 0.0f;
                float DcTerm = 0.0f;
                Shaper.ProcessAudioBuffer(&ZeroIn, &DcTerm, 1);
                for (int32 Index = 0; Index < InOut.NumFrames; ++Index)
                {
                    Wet[Index] -= DcTerm;
                }
            }
            break;
        }

        case EShape::Foldback:
        {
            Audio::FFoldbackDistortion Foldback;
            Foldback.Init(static_cast<float>(SampleRate), /*InNumChannels=*/1);
            Foldback.SetThresholdDb(FoldbackThresholdDb);
            // SetInputGainDb is NOT optional: FFoldbackDistortion's constructor initialises
            // Threshold and OutputGain but leaves InputGain uninitialised
            // (FoldbackDistortion.cpp:9-15), so skipping this multiplies the signal by garbage.
            Foldback.SetInputGainDb(20.0f * FMath::LogX(10.0f,
                FMath::Max(static_cast<float>(1.0 + Drive), UE_KINDA_SMALL_NUMBER)));
            Foldback.SetOutputGainDb(0.0f);
            Foldback.ProcessAudio(Channel, InOut.NumFrames, Wet.GetData());
            break;
        }

        case EShape::BitCrush:
        {
            Audio::FBitCrusher Crusher;
            Crusher.Init(static_cast<float>(SampleRate), /*InNumChannels=*/1);
            // Both setters are mandatory for the same reason as foldback's: the constructor leaves
            // ReciprocalBitDelta uninitialised and PhaseDelta at 1.0 (BitCrusher.cpp:10-19).
            // Holding the sample-rate crush at the render rate keeps `drive` a pure bit-depth
            // control, which is what the shape is named for.
            Crusher.SetSampleRateCrush(static_cast<float>(SampleRate));
            const float BitDepth = FMath::Lerp(BitCrushDepthAtZeroDrive, BitCrushDepthAtFullDrive,
                FMath::Clamp(static_cast<float>(Drive) / 100.0f, 0.0f, 1.0f));
            Crusher.SetBitDepthCrush(BitDepth);
            Crusher.ProcessAudio(Channel, InOut.NumFrames, Wet.GetData());
            break;
        }
        }

        for (int32 Index = 0; Index < InOut.NumFrames; ++Index)
        {
            Channel[Index] = DryScale * Channel[Index] + WetScale * Wet[Index];
        }
    }

    return true;
}

// ===========================================================================================
// delay - Audio::FDelay (DSP/Delay.h) + Audio::FOnePoleLPF (DSP/OnePole.h)
//
// FIntegerDelay's block call is the cleaner interface, but a feedback delay is a per-sample
// recursion: every output sample re-enters the line through the damping one-pole, so a block API
// has to be chunked to the delay length to stay exact - four samples at the schema's 0.1 ms
// minimum. FDelay expresses the recursion directly, and its per-sample entry point exists
// unchanged on 5.3 where FIntegerDelay's does not.
// ===========================================================================================
bool PwFxDelay(const FPwSynthParams& Params, int32 SampleRate, FPwSeededRandom& Rng,
    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError)
{
    using namespace PwFxChainAInternal;

    OutErrorCode.Reset();
    OutError.Reset();

    const TCHAR* const EffectName = TEXT("delay");
    const FName TimeKey(TEXT("timeMs"));
    const FName MixKey(TEXT("mix"));
    const FName FeedbackKey(TEXT("feedback"));
    const FName DampingKey(TEXT("dampingHz"));

    if (!ValidateSpan(InOut, SampleRate, EffectName, OutErrorCode, OutError)
        || !RequireParam(Params, TimeKey, EffectName, OutErrorCode, OutError)
        || !RequireParam(Params, MixKey, EffectName, OutErrorCode, OutError))
    {
        return false;
    }

    const double TimeMs = Params.GetNumber(TimeKey);
    const double Mix = Params.GetNumber(MixKey);
    const double Feedback = Params.GetNumber(FeedbackKey, 0.0);
    const double DampingHz = Params.GetNumber(DampingKey, 20000.0);

    if (!(TimeMs > 0.0))
    {
        return Fail(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("delay: timeMs must be greater than 0, got %g"), TimeMs),
            OutErrorCode, OutError);
    }
    if (Feedback < 0.0 || Feedback > MaxDelayFeedback)
    {
        return Fail(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("delay: feedback must be within [0, %g], got %g; at or above 1.0 ")
                TEXT("the delay line is a divergent recursion and the render never settles"),
                MaxDelayFeedback, Feedback),
            OutErrorCode, OutError);
    }

    if (InOut.NumFrames == 0 || Mix <= 0.0)
    {
        return true;
    }

    const float DryScale = static_cast<float>(1.0 - Mix);
    const float WetScale = static_cast<float>(Mix);
    const float FeedbackGain = static_cast<float>(Feedback);

    // The pole coefficient is set directly rather than through SetFrequency: that setter skips the
    // update when the new normalized cutoff is within KINDA_SMALL_NUMBER of the previous one, and
    // a fresh FOnePoleLPF starts at 0, so a very low dampingHz at a very high render rate (20 Hz
    // at 192 kHz is 2.08e-4 normalized) sits right on that threshold. SetG takes the same
    // coefficient SetFrequency would have computed.
    const float DampingPole = FMath::Exp(-UE_PI * NormalizedCutoff(DampingHz, SampleRate));

    // One extra millisecond of headroom: FDelay clamps the requested delay to the buffer it was
    // initialised with, so an exact-fit buffer would be at the mercy of float rounding.
    const float BufferLengthSec = static_cast<float>(TimeMs * 0.001 + 0.001);

    TArray<float*, TInlineAllocator<2>> Channels;
    GatherChannels(InOut, Channels);

    for (float* Channel : Channels)
    {
        Audio::FDelay Line;
        Line.Init(static_cast<float>(SampleRate), BufferLengthSec);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
        Line.SetDelayMsec(static_cast<float>(TimeMs));
#else
        // UE 5.4's FDelay::Update reads at WriteIndex - (int32)(DelayInSamples + 1.0f); 5.5
        // dropped that +1 and reads at WriteIndex - (int32)DelayInSamples (Delay.cpp, Update).
        // So on 5.4 a `timeMs` delay lands ONE SAMPLE LATE. Asking for one sample less puts the
        // tap on the sample the parameter names, on every engine: the fractional weight is
        // unchanged too, because Read() uses D - floor(D) and (D-1) - floor(D-1) == D - floor(D)
        // for D >= 1. Below one sample there is nothing to subtract and the +1 stands; the
        // schema's 0.1 ms floor is 4.8 samples at 48 kHz, so that is only reachable at a render
        // rate under 10 kHz.
        const float DelayInSamples =
            static_cast<float>(TimeMs) * static_cast<float>(SampleRate) * 0.001f;
        if (DelayInSamples >= 1.0f)
        {
            Line.SetDelaySamples(DelayInSamples - 1.0f);
        }
        else
        {
            Line.SetDelayMsec(static_cast<float>(TimeMs));
        }
#endif

        Audio::FOnePoleLPF Damper;
        Damper.SetG(DampingPole);

        // The damping filter sits in the FEEDBACK path, not on the output: the first echo is an
        // undamped copy of the input and each successive regeneration picks up one more pass of
        // the low-pass, which is what makes a damped delay darken as it repeats.
        float FeedbackSample = 0.0f;
        for (int32 Index = 0; Index < InOut.NumFrames; ++Index)
        {
            const float Dry = Channel[Index];
            const float Delayed = Line.ProcessAudioSample(Dry + FeedbackSample);
            FeedbackSample = FeedbackGain * Damper.ProcessAudioSample(Delayed);
            Channel[Index] = DryScale * Dry + WetScale * Delayed;
        }
    }

    return true;
}

// ===========================================================================================
// reverb - Audio::FPlateReverbFast (DSP/ReverbFast.h)
// ===========================================================================================
bool PwFxReverb(const FPwSynthParams& Params, int32 SampleRate, FPwSeededRandom& Rng,
    const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError)
{
    using namespace PwFxChainAInternal;

    OutErrorCode.Reset();
    OutError.Reset();

    const TCHAR* const EffectName = TEXT("reverb");
    const FName DecayKey(TEXT("decayMs"));
    const FName MixKey(TEXT("mix"));
    const FName PreDelayKey(TEXT("preDelayMs"));
    const FName DampingKey(TEXT("dampingHz"));

    if (!ValidateSpan(InOut, SampleRate, EffectName, OutErrorCode, OutError)
        || !RequireParam(Params, DecayKey, EffectName, OutErrorCode, OutError)
        || !RequireParam(Params, MixKey, EffectName, OutErrorCode, OutError))
    {
        return false;
    }

    const double DecayMs = Params.GetNumber(DecayKey);
    const double Mix = Params.GetNumber(MixKey);
    const double PreDelayMs = Params.GetNumber(PreDelayKey, 0.0);
    const double DampingHz = Params.GetNumber(DampingKey, 8000.0);

    if (InOut.NumFrames == 0 || Mix <= 0.0)
    {
        return true;
    }

    // --- schema -> FPlateReverbFastSettings ------------------------------------------------
    //
    // dampingHz -> LateReflections.Dampening. The plate feeds Dampening straight into
    // FBufferOnePoleLPF::SetG (LateReflectionsFast.cpp:241-245), and that class derives its own G
    // from a normalized cutoff as exp(-PI * normalized), so exp(-PI * 2 * fc / fs) is the exact
    // inverse: the tail's high-frequency absorption corner lands on the requested frequency.
    const double Dampening = FMath::Clamp<double>(
        FMath::Exp(-UE_DOUBLE_PI * NormalizedCutoff(DampingHz, SampleRate)),
        Audio::FLateReflectionsFast::MinDampening, Audio::FLateReflectionsFast::MaxDampening);

    // decayMs -> LateReflections.Decay, treating decayMs as an RT60. The plate attenuates by
    // (1 - Dampening) * (1 - Decay) once per plate pass (LateReflectionsFast.cpp:204-211), so the
    // per-pass gain that reaches -60 dB after N = decaySeconds / PlateLoopSeconds passes is
    // g = 10^(-3/N), and Decay = 1 - g / (1 - Dampening) - i.e. Decay supplies whatever loss the
    // dampening term does not already contribute. Note the engine's inverted sense: a LOWER Decay
    // is a LONGER tail.
    //
    // ClampSettings pins the result into [MinDecay, MaxDecay] and the clamp bites at both ends.
    // Both are honest limits of the algorithm rather than recipe errors, so neither is rejected:
    //   * a decayMs well under the plate's own loop time cannot produce a tail shorter than one
    //     pass, so Decay saturates at 1.0 and the tail is a single burst of taps;
    //   * a long decayMs at a low dampingHz cannot beat the dampening term's own per-pass loss, so
    //     Decay saturates at MinDecay and the realised tail is shorter than asked. At the schema's
    //     default dampingHz of 8000 and a 48 kHz render that ceiling is roughly 5.8 s.
    const double PassCount = FMath::Max(DecayMs * 0.001 / PlateLoopSeconds, UE_DOUBLE_SMALL_NUMBER);
    const double PerPassGain = FMath::Pow(10.0, -3.0 / PassCount);
    const double Decay = 1.0 - PerPassGain / FMath::Max(1.0 - Dampening, UE_DOUBLE_SMALL_NUMBER);

    Audio::FPlateReverbFastSettings Settings;

    // Early reflections are DISABLED, not left at their defaults. The schema exposes only tail
    // controls (decayMs, preDelayMs, dampingHz) and no early-reflection knob at all, so running
    // the FDN would colour every recipe with five hidden constants the caller cannot see or shape.
    Settings.bEnableEarlyReflections = false;
    Settings.bEnableLateReflections = true;

    // preDelayMs -> the tail's own pre-delay line. The schema's 0..500 ms sits inside the class's
    // 0..2000 ms range, so nothing is lost here.
    Settings.LateReflections.LateDelayMsec = static_cast<float>(PreDelayMs);

    // Wet level is the blend's job, so the tail runs at unity.
    Settings.LateReflections.LateGainDB = 0.0f;

    // Bandwidth is pinned wide open rather than left at its 0.5 default: the plate multiplies its
    // input by Bandwidth AND sets the input low-pass to 1 - Bandwidth (LateReflectionsFast.cpp:
    // 448,536), so the default would quietly cost 6 dB of wet level and roll the input off before
    // the caller's dampingHz has any say. High-frequency loss belongs to dampingHz alone.
    Settings.LateReflections.Bandwidth = Audio::FLateReflectionsFast::MaxBandwidth;

    Settings.LateReflections.Dampening = static_cast<float>(Dampening);
    Settings.LateReflections.Decay = static_cast<float>(Decay);
    // Diffusion and Density stay at the struct's defaults: they describe the plate's internal
    // echo texture, the schema exposes no control for either, and the algorithm needs some value.

    // Clamp explicitly even though FLateReflectionsFast::SetSettings clamps its own copy, so the
    // settings this function holds are the settings that actually run.
    Audio::FPlateReverbFast::ClampSettings(Settings);

    const int32 NumChannels = InOut.IsStereo() ? 2 : 1;
    Audio::FPlateReverbFast Reverb(static_cast<float>(SampleRate), /*InMaxInternalBufferSamples=*/512, Settings);
    // No FlushAudio() here. Audio::FPlateReverbFast::FlushAudio is DECLARED with
    // SIGNALPROCESSING_API in DSP/ReverbFast.h but has NO definition anywhere in SignalProcessing
    // (verified on 5.8: ReverbFast.cpp defines only the ctor, dtor, SetSettings, GetSettings,
    // ProcessAudio, ClampSettings, InterleaveAndMixOutput and ApplySettings, while every sibling
    // class - FLateReflectionsPlate, FEarlyReflectionsFast, FBufferOnePoleLPF - does define its
    // own). Calling it is an unconditional LNK2019. Nothing is lost by dropping it: Reverb is
    // constructed fresh on every invocation and construction already zeroes the state the flush
    // would have cleared (FIntegerDelay seeds its line with AddZeros, the plate taps go through
    // ResizeAndZero), so the tail starts silent and no render inherits a previous one. Do not
    // reintroduce the call.

    const float DryScale = static_cast<float>(1.0 - Mix);
    const float WetScale = static_cast<float>(Mix);

    // The plate is genuinely stereo - two decorrelated cross-fed plates, always two output
    // channels, and a stereo input is summed to mono on the way in. On a MONO span the two outputs
    // are averaged rather than one being taken or both being summed: taking one throws away half
    // the tail's density, and summing two decorrelated signals is about 3 dB hotter, which would
    // make the same reverb louder in a layer chain than on the master bus.
    Audio::FAlignedFloatBuffer InBlock;
    Audio::FAlignedFloatBuffer OutBlock;

    for (int32 Start = 0; Start < InOut.NumFrames; Start += ReverbBlockFrames)
    {
        const int32 Count = FMath::Min(ReverbBlockFrames, InOut.NumFrames - Start);

        // Blocked rather than one call over the whole span: a 60 s stereo render at 192 kHz would
        // otherwise need two more full-size interleaved mirrors of the buffer. Every stage inside
        // the plate is a per-sample recursion over persistent state, so the block size does not
        // change the output.
        InBlock.SetNumUninitialized(Count * NumChannels);
        for (int32 Index = 0; Index < Count; ++Index)
        {
            InBlock[Index * NumChannels] = InOut.Left[Start + Index];
            if (NumChannels == 2)
            {
                InBlock[Index * NumChannels + 1] = InOut.Right[Start + Index];
            }
        }

        Reverb.ProcessAudio(InBlock, NumChannels, OutBlock, /*OutNumChannels=*/2);

        for (int32 Index = 0; Index < Count; ++Index)
        {
            const float WetLeft = OutBlock[Index * 2];
            const float WetRight = OutBlock[Index * 2 + 1];

            float& Left = InOut.Left[Start + Index];
            if (NumChannels == 2)
            {
                Left = DryScale * Left + WetScale * WetLeft;
                float& Right = InOut.Right[Start + Index];
                Right = DryScale * Right + WetScale * WetRight;
            }
            else
            {
                Left = DryScale * Left + WetScale * (0.5f * (WetLeft + WetRight));
            }
        }
    }

    return true;
}
