// Copyright (c) 2026 Alexander Penkin. MIT License.

// The two source-reading generators of the audio.synth chain - `sample` and `granular` - plus
// PwResolveSourceBuffer, the shared asset->PCM seam they and `convolve` all go through.
//
// WHY THESE TWO SHARE A FILE. They are the same machine at two time scales: resolve a
// USoundWave, collapse it to mono, and drag a fractional read head across it. `sample` runs one
// read head for the whole layer; `granular` runs a few thousand short windowed ones. The
// interpolator, the read-rate maths, the source/render sample-rate conversion and the pitch
// envelope reader are identical, and splitting them would duplicate every one of those.
//
// HAND-ROLLED GRAIN SCHEDULING, NOT Audio::FGranularSynth (DSP/Granulator.h). FGranularSynth is
// a real-time class and three of its properties are disqualifying here, all verified against
// UE 5.8 `Runtime/SignalProcessing`:
//   1. Grains cannot be scheduled by the caller. FGranularSynth::SpawnGrain() is protected, takes
//      no arguments, and is driven from inside Generate() off an internal frame clock
//      (Granulator.cpp:794-809). There is no seam to hand it a grain index, a read position or a
//      rate.
//   2. Its randomness is unseedable. The spawn dice roll is FMath::FRand() (Granulator.cpp:804)
//      and every per-grain pan/volume/pitch/duration spread is FMath::FRandRange
//      (Granulator.h:276-279). Neither honours FRandomStream, so a render through it could not be
//      reproduced from the recipe's seed - the one property this whole subsystem is built on.
//   3. Init() unconditionally installs an output FDynamicsProcessor (-15 dB, 5:1, analog-mode
//      compressor) that the public API cannot disable, so "the grains" would arrive already
//      tone-shaped by an effect the recipe never asked for.
// Audio::FGrainEnvelope is likewise not reused for the window: GenerateEnvelope() is a silent
// no-op when asked for the type it already holds (Granulator.cpp:16, `if (CurrentType !=
// EnvelopeType)`), so it cannot be re-generated at a different length, and it asserts
// check(NumFrames > 1) - an editor-killer on a degenerate one-sample grain. A symmetric Hann is
// four lines, exact zero at both ends, and has neither landmine.
//
// DETERMINISM. Neither generator ever draws from the `Rng` it is handed; it only calls
// FPwSeededRandom::Derive, which is a pure hash of the parent's INITIAL seed. Consequences that
// are the point of the design:
//   - grain N's jitter is a function of N alone, so raising densityHz inserts grains between the
//     existing ones instead of re-rolling all of them;
//   - the parent stream's position on entry is irrelevant, so layer order cannot perturb a render;
//   - every per-grain draw is made UNCONDITIONALLY, in a fixed order, even when the parameter
//     that consumes it is zero - otherwise switching reverseChance off would shift the position
//     and pitch draws of every grain.
// The only other stochastic input, a `noise` modulator, draws from its own NEGATIVE substream
// index, which grain indices (always >= 0) cannot collide with.
//
// ENGINE VERSION. The only engine DSP symbol used is Audio::GetFrequencyMultiplier
// (DSP/Dsp.h:409 on 5.8, :375 on 5.3), an inline 2^(semitones/12); it is present across the whole
// intended 5.3-5.8 range, so this file owes no row in docs/engine-version-support.md.

#include "AudioGen/PwSynthDsp.h"

#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwAudioDecode.h"
#include "AudioGen/PwSeededRandom.h"
#include "AudioGen/PwSynthRecipe.h"
#include "Handlers/ErrorCodes.h"

#include "DSP/Dsp.h"
#include "Math/UnrealMathUtility.h"
#include "Sound/SoundWave.h"
#include "UObject/UObjectGlobals.h"

// Named (not anonymous) namespace: the module builds with bUseUnity = true and anonymous
// namespaces in merged TUs are the ODR-collision source Build.cs warns about.
namespace PwGenSourceInternal
{
    /**
     * Substream index for a `noise` modulator. Grain substreams are the grain's own non-negative
     * index, so keeping the modulator on the negative side makes a collision - which would tie a
     * grain's jitter to the modulator's draws - unrepresentable.
     */
    constexpr int32 NoiseStreamBase = -1;

    bool Fail(const TCHAR* Code, const FString& Message, FString& OutErrorCode, FString& OutError)
    {
        OutErrorCode = Code;
        OutError = Message;
        return false;
    }

    // -----------------------------------------------------------------------
    // Parameter access
    // -----------------------------------------------------------------------

    const FPwSynthParamSpec* FindSpecRow(const FPwSynthKindSpec& Kind, FName Name)
    {
        if (!Kind.Params)
        {
            return nullptr;
        }
        for (const FPwSynthParamSpec& Row : *Kind.Params)
        {
            if (Row.Name == Name)
            {
                return &Row;
            }
        }
        return nullptr;
    }

    /**
     * Reads a numeric parameter, falling back to the KIND SPEC's own documented default rather
     * than to a constant copied into this file. ParseSynthRecipe already materializes every
     * default into the bag, so this fallback only fires for a hand-assembled bag (tests, a future
     * programmatic caller) - and sourcing it from the table is what stops the two paths drifting.
     * A required row's DefaultNumber is 0, which is exactly the value the validation below rejects.
     */
    double NumberParam(const FPwSynthParams& Params, const FPwSynthKindSpec& Kind, const TCHAR* Name)
    {
        const FName Key(Name);
        if (Params.Has(Key))
        {
            return Params.GetNumber(Key);
        }
        const FPwSynthParamSpec* Row = FindSpecRow(Kind, Key);
        return Row ? Row->DefaultNumber : 0.0;
    }

    bool BoolParam(const FPwSynthParams& Params, const FPwSynthKindSpec& Kind, const TCHAR* Name)
    {
        const FName Key(Name);
        if (Params.Has(Key))
        {
            return Params.GetBool(Key);
        }
        const FPwSynthParamSpec* Row = FindSpecRow(Kind, Key);
        return Row ? (Row->DefaultNumber != 0.0) : false;
    }

    // -----------------------------------------------------------------------
    // Source reading
    // -----------------------------------------------------------------------

    /**
     * Collapses the decoded stereo buffer to the mono signal the generators work in.
     *
     * FPwAudioBuffer duplicates a mono source into both channels, so for the common case the
     * 0.5/0.5 average returns the original samples bit-for-bit. For a genuinely stereo source it
     * is a real downmix and a real information loss, including cancellation of out-of-phase
     * content - documented rather than hidden, because every generator in this chain is mono by
     * contract and the layer's `pan` is the one place stereo placement happens.
     */
    void CollapseToMono(const FPwAudioBuffer& In, TArray<float>& Out)
    {
        const int32 NumFrames = In.NumFrames();
        Out.Reset();
        Out.SetNumUninitialized(NumFrames);

        const bool bHasRight = In.Right.Num() == NumFrames;
        for (int32 Frame = 0; Frame < NumFrames; ++Frame)
        {
            Out[Frame] = bHasRight ? 0.5f * (In.Left[Frame] + In.Right[Frame]) : In.Left[Frame];
        }
    }

    FORCEINLINE float FetchSample(const TArray<float>& Source, int64 Index, bool bWrap)
    {
        const int64 Num = Source.Num();
        if (Num <= 0)
        {
            return 0.f;
        }
        if (bWrap)
        {
            int64 Wrapped = Index % Num;
            if (Wrapped < 0)
            {
                Wrapped += Num;
            }
            return Source[static_cast<int32>(Wrapped)];
        }
        return (Index < 0 || Index >= Num) ? 0.f : Source[static_cast<int32>(Index)];
    }

    /**
     * 4-point Catmull-Rom at a fractional index. Hand-rolled rather than FMath::CubicInterp
     * (which wants tangents) or Audio::FSampleBufferReader (which is int16-only, 2-tap linear, and
     * carries the SetBuffer-resets-Init ordering trap): a sample player transposed two octaves up
     * aliases audibly on linear interpolation, and cubic costs three extra multiplies.
     *
     * Out-of-range taps read as zero (bWrap false) or wrap (bWrap true), so the caller never has
     * to special-case the first or last three samples of the source.
     */
    double CubicAt(const TArray<float>& Source, double Position, bool bWrap)
    {
        const double Base = FMath::FloorToDouble(Position);
        const int64 Index = static_cast<int64>(Base);
        const double Frac = Position - Base;

        const double P0 = FetchSample(Source, Index - 1, bWrap);
        const double P1 = FetchSample(Source, Index, bWrap);
        const double P2 = FetchSample(Source, Index + 1, bWrap);
        const double P3 = FetchSample(Source, Index + 2, bWrap);

        const double A = -0.5 * P0 + 1.5 * P1 - 1.5 * P2 + 0.5 * P3;
        const double B = P0 - 2.5 * P1 + 2.0 * P2 - 0.5 * P3;
        const double C = -0.5 * P0 + 0.5 * P2;
        return ((A * Frac + B) * Frac + C) * Frac + P1;
    }

    double WrapPosition(double Position, int32 Num)
    {
        if (Num <= 0)
        {
            return 0.0;
        }
        const double Span = static_cast<double>(Num);
        double Wrapped = FMath::Fmod(Position, Span);
        if (Wrapped < 0.0)
        {
            Wrapped += Span;
        }
        return Wrapped;
    }

    // -----------------------------------------------------------------------
    // Pitch envelope
    // -----------------------------------------------------------------------

    /**
     * Piecewise-linear pitch envelope with a forward-only cursor. The linear-scan form is O(P) per
     * lookup and the sample player asks once per output sample - up to 2.9M times for a 60 s
     * render at 48 kHz against as many as 64 points - so the cursor is a real saving, not a
     * micro-optimisation. Times outside the envelope hold the first / last value rather than
     * extrapolating.
     */
    struct FPitchEnvelopeReader
    {
        explicit FPitchEnvelopeReader(const TArray<FPwSynthPitchPoint>& InPoints)
            : Points(&InPoints)
        {
        }

        double SemitonesAt(double TimeMs)
        {
            const TArray<FPwSynthPitchPoint>& P = *Points;
            if (P.Num() == 0)
            {
                return 0.0;
            }
            if (TimeMs <= P[0].TimeMs)
            {
                Cursor = 0;
                return P[0].Semitones;
            }
            while (Cursor + 1 < P.Num() && P[Cursor + 1].TimeMs < TimeMs)
            {
                ++Cursor;
            }
            if (Cursor + 1 >= P.Num())
            {
                return P.Last().Semitones;
            }
            const FPwSynthPitchPoint& Left = P[Cursor];
            const FPwSynthPitchPoint& Right = P[Cursor + 1];
            const double Span = Right.TimeMs - Left.TimeMs;
            const double Alpha = (Span > 0.0)
                ? FMath::Clamp((TimeMs - Left.TimeMs) / Span, 0.0, 1.0)
                : 1.0;
            return FMath::Lerp(Left.Semitones, Right.Semitones, Alpha);
        }

        const TArray<FPwSynthPitchPoint>* Points = nullptr;
        int32 Cursor = 0;
    };

    /** Shared preamble: both generators need a usable render rate and a decoded mono source. */
    bool PrepareSource(const FString& SourcePath, int32 SampleRate, TArray<float>& OutMonoSource,
        int32& OutSourceRate, FString& OutErrorCode, FString& OutError)
    {
        if (SampleRate <= 0)
        {
            return Fail(ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("Render sample rate %d is not positive."), SampleRate),
                OutErrorCode, OutError);
        }

        FPwAudioBuffer Source;
        if (!PwResolveSourceBuffer(SourcePath, Source, OutErrorCode, OutError))
        {
            return false;
        }

        // PwDecodeSoundWave already refuses a zero-frame or rate-less payload, so this is the
        // second reader of the same fact rather than a speculative branch - but the read head
        // maths below divides by both, so the guard earns its four lines.
        if (Source.NumFrames() <= 0 || Source.SampleRate <= 0)
        {
            return Fail(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
                FString::Printf(TEXT("Source '%s' decoded to %d frames at %d Hz; there is nothing to read."),
                    *SourcePath, Source.NumFrames(), Source.SampleRate),
                OutErrorCode, OutError);
        }

        OutSourceRate = Source.SampleRate;
        CollapseToMono(Source, OutMonoSource);
        return true;
    }
}

// ===========================================================================
// PwResolveSourceBuffer
// ===========================================================================

bool PwResolveSourceBuffer(const FString& AssetPath, FPwAudioBuffer& Out,
    FString& OutErrorCode, FString& OutError)
{
    using namespace PwGenSourceInternal;

    // Failure is the default (rpc-design.md §1): the caller's buffer is cleared on entry and only
    // a fully successful decode leaves anything in it.
    Out = FPwAudioBuffer();
    OutErrorCode.Reset();
    OutError.Reset();

    // ASSETS ONLY, DELIBERATELY. There is no file-reading path anywhere in this subsystem, and
    // this function is the seam where that decision lives. A user with a loose .wav imports it as
    // a SoundWave first and then references the asset path; the alternative - letting a recipe
    // name an arbitrary path on disk - would hand a remote MCP caller a filesystem read primitive,
    // and would let a "working" recipe reference audio that no other machine has. PwAudioDecode.h
    // states the same rule for the decode seam; this is its recipe-facing half.
    //
    // NATIVE RATE OUT; EVERY CONSUMER RESAMPLES. `Out.SampleRate` is the ASSET's rate, taken from
    // the wave header by the decoder and never substituted with a plausible-looking 48000. This
    // function cannot know the render rate, so converting here is impossible and the duty is the
    // caller's - including the caller that "just wants the samples".
    //
    // Forwarding a native-rate buffer into the render without resampling is a SILENT failure, not
    // a wrong-pitch one: FPwAudioBuffer::MixInto refuses a rate mismatch outright, mixes nothing,
    // and returns 0, so the layer becomes inaudible and no error is raised anywhere (rpc-design.md
    // §1). Both generators in this file therefore carry an explicit SourceRate / RenderRate term in
    // their read-head step - PwGenSample per output sample, PwGenGranular per grain - and
    // TestPwGenSampleGranular.cpp pins each of them with a 44.1 kHz fixture rendered at 48 kHz.

    if (!IsInGameThread())
    {
        // StaticLoadObject and the wave's editor bulk-data payload are both game-thread only;
        // PwDecodeSoundWaveWithCode reports this too, but it is reached after the load.
        return Fail(ErrorCodes::ERR_INVALID_STATE,
            FString::Printf(TEXT("Source '%s' can only be resolved on the game thread."), *AssetPath),
            OutErrorCode, OutError);
    }

    FString Normalized = AssetPath.TrimStartAndEnd();
    Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
    while (Normalized.EndsWith(TEXT("/")))
    {
        Normalized.LeftChopInline(1);
    }

    if (Normalized.IsEmpty())
    {
        // Distinct from ASSET_NOT_FOUND on purpose (rpc-design.md §7): an empty path is a caller
        // bug, a non-empty one that resolves nowhere is a content problem, and the two get
        // different fixes.
        return Fail(ErrorCodes::ERR_INVALID_PARAMS,
            TEXT("sourcePath is empty; it must name an imported USoundWave asset."),
            OutErrorCode, OutError);
    }

    // FindObject first: an already-loaded wave (and a transient one, which has no package on disk)
    // resolves without touching the asset registry or the loader.
    USoundWave* Wave = FindObject<USoundWave>(nullptr, *Normalized);
    if (!Wave)
    {
        // LOAD_NoWarn | LOAD_Quiet: a missing asset is reported below with the path and a recovery
        // hint, so the loader's own generic warning would only add noise to the log.
        Wave = Cast<USoundWave>(StaticLoadObject(USoundWave::StaticClass(), nullptr, *Normalized,
            nullptr, LOAD_NoWarn | LOAD_Quiet));
    }

    if (!Wave)
    {
        return Fail(ErrorCodes::ERR_ASSET_NOT_FOUND,
            FString::Printf(
                TEXT("No USoundWave at '%s'. Only imported SoundWave assets can be used as a synth source; ")
                TEXT("import a loose audio file into the project first, then reference its asset path."),
                *Normalized),
            OutErrorCode, OutError);
    }

    // Propagated, never flattened: PwDecodeSoundWaveWithCode distinguishes procedural,
    // multichannel, empty and generic decode failures with separate codes, and collapsing them
    // into one would force the caller back to parsing messages (rpc-design.md §7).
    return PwDecodeSoundWaveWithCode(Wave, Out, OutErrorCode, OutError);
}

// NO DECODE CACHE, AND WHY. PwDecodeSoundWave blocks on the wave's editor bulk-data payload
// future, so a recipe with four granular layers on one asset pays that block four times. The two
// cache shapes available without changing this signature are both worse than the block:
//   - a file-static map keyed by asset path is a global whose lifetime outlives the render. It
//     retains decoded PCM indefinitely (a 60 s stereo source is ~23 MB) and, worse, serves stale
//     samples after the user re-imports the asset - a silently wrong render, which costs more than
//     a slow one;
//   - a single-entry memo has the same staleness hazard for a fraction of the benefit.
// The clean fix belongs one level up, where the render call can own the lifetime: thread an
// `FPwSourceBufferCache&` through PwRenderRecipe into an overload of this function, so the map
// dies with the render. That is a one-line change to PwSynthDsp.h and is deliberately left to
// whoever owns that header rather than smuggled in here as a global.

// ===========================================================================
// PwGenSample
// ===========================================================================

bool PwGenSample(const FPwSynthParams& Params, const TArray<FPwSynthPitchPoint>& PitchEnvelope,
    const FPwSynthModulation& Modulation, int32 SampleRate, FPwSeededRandom& Rng,
    TArrayView<float> OutMono, FString& OutErrorCode, FString& OutError)
{
    using namespace PwGenSourceInternal;

    OutErrorCode.Reset();
    OutError.Reset();

    const FPwSynthKindSpec& Spec = PwSynthGeneratorSpec(EPwSynthGeneratorKind::Sample);
    const FString SourcePath = Params.GetString(FName(TEXT("sourcePath")));
    const double SourceStartMs = NumberParam(Params, Spec, TEXT("sourceStartMs"));
    const double PlaybackRate = NumberParam(Params, Spec, TEXT("playbackRate"));
    const bool bLoop = BoolParam(Params, Spec, TEXT("loop"));

    // Rejected, not clamped (rpc-design.md §3). A zero rate parks the read head and emits DC; a
    // negative one reads backwards out of the buffer. The schema's own minimum is 0.01, so this
    // can only fire for a bag that never went through ParseSynthRecipe.
    if (!(PlaybackRate > 0.0))
    {
        return Fail(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("playbackRate is %g; it must be greater than 0."), PlaybackRate),
            OutErrorCode, OutError);
    }

    // bAcceptsFm is false: the FM index is defined against a carrier frequency and a recorded
    // sample has a read RATE, not one. `fm` is rejected rather than ignored; the rejection names
    // am / ring and the layer's pitchEnvelope as the recoveries. The `noise` modulator keeps to
    // its own NEGATIVE substream, which grain indices (always >= 0) cannot collide with.
    FPwSynthModulator Mod;
    if (!PwPrepareModulation(Modulation, /*bAcceptsFm*/ false, TEXT("sample"), SampleRate,
            Rng.Derive(NoiseStreamBase), Mod, OutErrorCode, OutError))
    {
        return false;
    }

    TArray<float> Mono;
    int32 SourceRate = 0;
    if (!PrepareSource(SourcePath, SampleRate, Mono, SourceRate, OutErrorCode, OutError))
    {
        return false;
    }

    // Validation runs before this early-out on purpose: a zero-length layer must still surface a
    // missing asset, otherwise a recipe bug hides behind a timing accident.
    const int32 NumOut = OutMono.Num();
    if (NumOut <= 0)
    {
        return true;
    }

    // THE SAMPLE-RATE MISMATCH TERM. Without it a 44.1 kHz asset rendered at 48 kHz plays 8.8%
    // fast and a whole semitone-and-a-half sharp - the most likely real-world defect in this
    // generator, and the one that looks like "the sample is just a bit off" rather than a bug.
    const double RateConversion = static_cast<double>(SourceRate) / static_cast<double>(SampleRate);

    FPitchEnvelopeReader Pitch(PitchEnvelope);

    // sourceStartMs is a one-time read offset, NOT a loop point. The schema has no loop-start /
    // loop-end pair, so `loop` repeats the whole source; making the offset double as a loop start
    // would give one key two meanings and would not round-trip as anything the caller wrote.
    double ReadPos = SourceStartMs * 0.001 * static_cast<double>(SourceRate);
    if (bLoop)
    {
        ReadPos = WrapPosition(ReadPos, Mono.Num());
    }

    const double SecondsPerSample = 1.0 / static_cast<double>(SampleRate);

    for (int32 Index = 0; Index < NumOut; ++Index)
    {
        const double TimeSeconds = Index * SecondsPerSample;
        const double Semitones = Pitch.SemitonesAt(TimeSeconds * 1000.0);
        const double ModValue = Mod.Generate();

        const double Step = RateConversion * PlaybackRate
            * static_cast<double>(Audio::GetFrequencyMultiplier(static_cast<float>(Semitones)));

        double Value;
        if (bLoop)
        {
            Value = CubicAt(Mono, ReadPos, /*bWrap=*/true);
        }
        else if (ReadPos < 0.0 || ReadPos >= static_cast<double>(Mono.Num()))
        {
            // Past the end with loop off: silence. Not a repeat (that is what `loop` is for) and
            // not an error (a source shorter than its layer is a normal, deliberate recipe).
            Value = 0.0;
        }
        else
        {
            Value = CubicAt(Mono, ReadPos, /*bWrap=*/false);
        }

        OutMono[Index] = static_cast<float>(Value * Mod.AmplitudeScale(ModValue));

        ReadPos += Step;
        if (bLoop)
        {
            // Wrapped every sample rather than at the end: over a 60 s render an unwrapped
            // accumulator would drift into the range where a double's ULP exceeds the step.
            ReadPos = WrapPosition(ReadPos, Mono.Num());
        }
    }

    return true;
}

// ===========================================================================
// PwGenGranular
// ===========================================================================

bool PwGenGranular(const FPwSynthParams& Params, const TArray<FPwSynthPitchPoint>& PitchEnvelope,
    const FPwSynthModulation& Modulation, int32 SampleRate, FPwSeededRandom& Rng,
    TArrayView<float> OutMono, FString& OutErrorCode, FString& OutError)
{
    using namespace PwGenSourceInternal;

    OutErrorCode.Reset();
    OutError.Reset();

    const FPwSynthKindSpec& Spec = PwSynthGeneratorSpec(EPwSynthGeneratorKind::Granular);
    const FString SourcePath = Params.GetString(FName(TEXT("sourcePath")));
    const double GrainMs = NumberParam(Params, Spec, TEXT("grainMs"));
    const double DensityHz = NumberParam(Params, Spec, TEXT("densityHz"));
    const double PositionStart = NumberParam(Params, Spec, TEXT("positionStart"));
    const double PositionEnd = NumberParam(Params, Spec, TEXT("positionEnd"));
    const double PositionJitter = NumberParam(Params, Spec, TEXT("positionJitter"));
    const double PitchJitterCents = NumberParam(Params, Spec, TEXT("pitchJitterCents"));
    const double ReverseChance = NumberParam(Params, Spec, TEXT("reverseChance"));

    // Errors, never silent corrections (rpc-design.md §1/§3). A zero grain length or density is
    // not "no grains", it is an unanswerable request, and a backwards sweep is a swapped pair of
    // keys the caller needs told about rather than quietly reordered.
    if (!(GrainMs > 0.0))
    {
        return Fail(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("grainMs is %g; it must be greater than 0."), GrainMs),
            OutErrorCode, OutError);
    }
    if (!(DensityHz > 0.0))
    {
        return Fail(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("densityHz is %g; it must be greater than 0."), DensityHz),
            OutErrorCode, OutError);
    }
    if (PositionStart > PositionEnd)
    {
        return Fail(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(
                TEXT("positionStart (%g) is past positionEnd (%g); the read head would sweep backwards. ")
                TEXT("Swap them to sweep the other way."),
                PositionStart, PositionEnd),
            OutErrorCode, OutError);
    }

    // bAcceptsFm is false: the FM index is defined against a carrier frequency and a recorded
    // sample has a read RATE, not one. `fm` is rejected rather than ignored; the rejection names
    // am / ring and the layer's pitchEnvelope as the recoveries. The `noise` modulator keeps to
    // its own NEGATIVE substream, which grain indices (always >= 0) cannot collide with.
    FPwSynthModulator Mod;
    if (!PwPrepareModulation(Modulation, /*bAcceptsFm*/ false, TEXT("granular"), SampleRate,
            Rng.Derive(NoiseStreamBase), Mod, OutErrorCode, OutError))
    {
        return false;
    }

    TArray<float> Mono;
    int32 SourceRate = 0;
    if (!PrepareSource(SourcePath, SampleRate, Mono, SourceRate, OutErrorCode, OutError))
    {
        return false;
    }

    const int32 NumOut = OutMono.Num();
    if (NumOut <= 0)
    {
        return true;
    }

    const double RateConversion = static_cast<double>(SourceRate) / static_cast<double>(SampleRate);
    const int32 GrainSamples = FMath::Max(1,
        FMath::RoundToInt32(GrainMs * 0.001 * static_cast<double>(SampleRate)));
    const double IntervalSamples = static_cast<double>(SampleRate) / DensityHz;

    // OVERLAP NORMALIZATION. densityHz * grainMs / 1000 is how many grains are sounding at once on
    // average, so dividing by it stops density doubling as a volume knob. Floored at 1: below unity
    // overlap the grains do not actually overlap, and scaling UP would push isolated grains above
    // the level they have in the source.
    const double Overlap = DensityHz * GrainMs / 1000.0;
    const double NormGain = 1.0 / FMath::Max(1.0, Overlap);

    // Symmetric Hann, exactly zero at both ends. Without a window every grain boundary is a step
    // discontinuity - the classic granular click, and the loudest thing in the output once density
    // is high. Computed once because every grain is the same length.
    TArray<float> Window;
    Window.SetNumUninitialized(GrainSamples);
    {
        const double Denominator = static_cast<double>(FMath::Max(1, GrainSamples - 1));
        for (int32 K = 0; K < GrainSamples; ++K)
        {
            Window[K] = static_cast<float>(
                0.5 - 0.5 * FMath::Cos(2.0 * UE_DOUBLE_PI * static_cast<double>(K) / Denominator));
        }
    }

    for (int32 Index = 0; Index < NumOut; ++Index)
    {
        OutMono[Index] = 0.f;
    }

    FPitchEnvelopeReader Pitch(PitchEnvelope);
    const double SecondsPerSample = 1.0 / static_cast<double>(SampleRate);
    const double ProgressDenominator = static_cast<double>(FMath::Max(1, NumOut - 1));

    for (int32 GrainIndex = 0; ; ++GrainIndex)
    {
        const int32 Onset = FMath::FloorToInt32(GrainIndex * IntervalSamples);
        if (Onset >= NumOut)
        {
            break;
        }

        // Per-grain substream. Derive is a pure hash of the parent's INITIAL seed and this index,
        // so grain 412 draws the same numbers whatever densityHz is and whatever grains 0-411 did.
        FPwSeededRandom GrainRng = Rng.Derive(GrainIndex);

        // All three draws happen unconditionally and in this fixed order even when the parameter
        // scaling them is zero. Drawing them conditionally would mean that switching reverseChance
        // off silently re-rolled every grain's position and pitch.
        const double PositionRoll = GrainRng.FloatInRange(-1.f, 1.f);
        const double CentsRoll = GrainRng.FloatInRange(-1.f, 1.f);
        const double ReverseRoll = GrainRng.FloatInRange(0.f, 1.f);

        const double Progress = FMath::Clamp(static_cast<double>(Onset) / ProgressDenominator, 0.0, 1.0);
        const double Position = FMath::Clamp(
            FMath::Lerp(PositionStart, PositionEnd, Progress) + PositionRoll * PositionJitter,
            0.0, 1.0);
        const bool bReverse = (ReverseChance >= 1.0) || (ReverseRoll < ReverseChance);

        // Rate held constant for the whole grain. Grains are milliseconds long, and sweeping the
        // rate inside one would fight the window for control of the grain's shape; sampling the
        // pitch envelope at the onset is the standard granular reading.
        const double OnsetSeconds = Onset * SecondsPerSample;
        const double Semitones = Pitch.SemitonesAt(OnsetSeconds * 1000.0);
        const double Rate = RateConversion
            * static_cast<double>(Audio::GetFrequencyMultiplier(static_cast<float>(Semitones)))
            * FMath::Pow(2.0, CentsRoll * PitchJitterCents / 1200.0);

        // The read head is normalized over the span from which a WHOLE grain still fits, so
        // positionEnd = 1.0 reads the last grain-length of the source instead of running off the
        // end into silence. A grain longer than the source starts at 0 and reads what there is.
        const double GrainSourceLength = GrainSamples * Rate;
        const double MaxStart = FMath::Max(0.0, static_cast<double>(Mono.Num()) - GrainSourceLength);
        const double ReadStart = Position * MaxStart;

        const int32 Count = FMath::Min(GrainSamples, NumOut - Onset);
        for (int32 K = 0; K < Count; ++K)
        {
            const double ReadPos = bReverse
                ? (ReadStart + GrainSourceLength - K * Rate)
                : (ReadStart + K * Rate);
            OutMono[Onset + K] += static_cast<float>(CubicAt(Mono, ReadPos, /*bWrap=*/false) * Window[K]);
        }
    }

    // Normalization and the amplitude routing run once over the summed cloud rather than per
    // grain: am / ring shape the cloud, not the individual grains, and applying them inside the
    // grain loop would multiply the overlap region by the modulator twice.
    for (int32 Index = 0; Index < NumOut; ++Index)
    {
        double Value = static_cast<double>(OutMono[Index]) * NormGain;
        if (Mod.IsSet())
        {
            Value *= Mod.AmplitudeScale(Mod.Generate());
        }
        OutMono[Index] = static_cast<float>(Value);
    }

    return true;
}
