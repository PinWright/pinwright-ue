// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwSynthDsp.cpp - kind dispatch + the recipe renderer.
//
// The generator and effect implementations live in sibling translation units; this file owns
// only the two switches that route a kind to one of them, and PwRenderRecipe, which walks the
// recipe topology (layers -> mix bus -> master chain -> normalize -> fades -> clamp).
//
// Two properties this file exists to hold:
//
//   DETERMINISM. Single-threaded, no wall clock, and every layer's randomness comes from
//   Root.Derive(LayerIndex) - a pure hash of the recipe seed and the layer's own index. Layer
//   3 therefore renders identically whether or not layers 0-2 ran first, and editing layer 2
//   cannot move layer 1's noise. Threading the one root stream through all layers in sequence
//   would look equivalent and quietly break that; see the comment on FPwSeededRandom::Derive.
//
//   MEASUREMENT, NOT ASSUMPTION (rpc-design.md §1). Every number in FPwRenderReport is read
//   back off the samples: FramesMixed is FPwAudioBuffer::MixInto's own return, PeakLinear scans
//   each layer's post-envelope/FX mono signal, PeakLinearPostGain scans its post-gain/pan stereo
//   layer buffer, PeakDbBeforeNormalize scans the bus, NormalizeGainDb is the gain that was
//   applied rather than the gain that was requested, and ClampedSamples is counted inside the
//   clamp loop. Every bMeasured defaults false, so a path that never measured cannot report a
//   clean zero.

#include "AudioGen/PwSynthDsp.h"

#include "AudioGen/PwAudioFeatures.h"
#include "Handlers/ErrorCodes.h"
#include "PinWrightSubsystem.h"

// Named (not anonymous) namespace: the module builds with bUseUnity = true.
namespace PwSynthDspInternal
{
    // Substream index handed to the master fx chain. Negative, so it can never collide with a
    // layer index however many layers a recipe grows.
    constexpr int32 MasterStreamIndex = -1;

    constexpr double Ln10 = 2.30258509299404568402;

    // -----------------------------------------------------------------------------------
    // Dispatch
    // -----------------------------------------------------------------------------------

    // Neither switch carries a `default:`, so a new enumerator has no landing place and falls
    // out of the switch into the error below rather than into a silent "default oscillator" or
    // pass-through effect.
    //
    // Do NOT read that as compiler enforcement. Clang's -Wswitch would flag the missing case,
    // but this plugin ships on Win64/MSVC, where the unhandled-enumerator warnings C4061/C4062
    // are off by default and not enabled here. The real backstop is what is written below: the
    // explicit Unspecified / Count cases plus a runtime error naming the kind. Adding a
    // generator or effect kind therefore still requires remembering to add its row - claiming
    // otherwise would be worse than claiming nothing, because the next reader would trust it.

    bool DispatchGenerator(EPwSynthGeneratorKind Kind, const FPwSynthParams& Params,
        const TArray<FPwSynthPitchPoint>& PitchEnvelope, const FPwSynthModulation& Modulation,
        int32 SampleRate, FPwSeededRandom& Rng, TArrayView<float> OutMono,
        FString& OutErrorCode, FString& OutError)
    {
        switch (Kind)
        {
        case EPwSynthGeneratorKind::Osc:
            return PwGenOsc(Params, PitchEnvelope, Modulation, SampleRate, Rng, OutMono, OutErrorCode, OutError);
        case EPwSynthGeneratorKind::Noise:
            return PwGenNoise(Params, PitchEnvelope, Modulation, SampleRate, Rng, OutMono, OutErrorCode, OutError);
        case EPwSynthGeneratorKind::Modal:
            return PwGenModal(Params, PitchEnvelope, Modulation, SampleRate, Rng, OutMono, OutErrorCode, OutError);
        case EPwSynthGeneratorKind::Formant:
            return PwGenFormant(Params, PitchEnvelope, Modulation, SampleRate, Rng, OutMono, OutErrorCode, OutError);
        case EPwSynthGeneratorKind::Granular:
            return PwGenGranular(Params, PitchEnvelope, Modulation, SampleRate, Rng, OutMono, OutErrorCode, OutError);
        case EPwSynthGeneratorKind::Sample:
            return PwGenSample(Params, PitchEnvelope, Modulation, SampleRate, Rng, OutMono, OutErrorCode, OutError);

        case EPwSynthGeneratorKind::Unspecified:
        case EPwSynthGeneratorKind::Count:
            break;
        }

        OutErrorCode = ErrorCodes::ERR_UNKNOWN_GENERATOR;
        OutError = FString::Printf(
            TEXT("generator kind %d has no renderer. An unrecognized generator would render ")
            TEXT("silence, which reads as a working pipeline that produced a quiet sound."),
            static_cast<int32>(Kind));
        return false;
    }

    bool DispatchFx(EPwSynthFxKind Kind, const FPwSynthParams& Params, int32 SampleRate,
        FPwSeededRandom& Rng, const FPwDspSpan& InOut, FString& OutErrorCode, FString& OutError)
    {
        switch (Kind)
        {
        case EPwSynthFxKind::Filter:     return PwFxFilter(Params, SampleRate, Rng, InOut, OutErrorCode, OutError);
        case EPwSynthFxKind::Distort:    return PwFxDistort(Params, SampleRate, Rng, InOut, OutErrorCode, OutError);
        case EPwSynthFxKind::Delay:      return PwFxDelay(Params, SampleRate, Rng, InOut, OutErrorCode, OutError);
        case EPwSynthFxKind::Reverb:     return PwFxReverb(Params, SampleRate, Rng, InOut, OutErrorCode, OutError);
        case EPwSynthFxKind::Chorus:     return PwFxChorus(Params, SampleRate, Rng, InOut, OutErrorCode, OutError);
        case EPwSynthFxKind::Flanger:    return PwFxFlanger(Params, SampleRate, Rng, InOut, OutErrorCode, OutError);
        case EPwSynthFxKind::Phaser:     return PwFxPhaser(Params, SampleRate, Rng, InOut, OutErrorCode, OutError);
        case EPwSynthFxKind::RingMod:    return PwFxRingmod(Params, SampleRate, Rng, InOut, OutErrorCode, OutError);
        case EPwSynthFxKind::PitchShift: return PwFxPitchshift(Params, SampleRate, Rng, InOut, OutErrorCode, OutError);
        case EPwSynthFxKind::Compressor: return PwFxCompressor(Params, SampleRate, Rng, InOut, OutErrorCode, OutError);
        case EPwSynthFxKind::Eq:         return PwFxEq(Params, SampleRate, Rng, InOut, OutErrorCode, OutError);
        case EPwSynthFxKind::Gain:       return PwFxGain(Params, SampleRate, Rng, InOut, OutErrorCode, OutError);
        case EPwSynthFxKind::Width:      return PwFxWidth(Params, SampleRate, Rng, InOut, OutErrorCode, OutError);
        case EPwSynthFxKind::Reverse:    return PwFxReverse(Params, SampleRate, Rng, InOut, OutErrorCode, OutError);
        case EPwSynthFxKind::Convolve:   return PwFxConvolve(Params, SampleRate, Rng, InOut, OutErrorCode, OutError);

        case EPwSynthFxKind::Unspecified:
        case EPwSynthFxKind::Count:
            break;
        }

        OutErrorCode = ErrorCodes::ERR_UNKNOWN_EFFECT;
        OutError = FString::Printf(
            TEXT("effect kind %d has no implementation. A skipped effect passes the dry signal ")
            TEXT("through, which is indistinguishable from an effect that had no audible result."),
            static_cast<int32>(Kind));
        return false;
    }

    // -----------------------------------------------------------------------------------
    // Scalar helpers
    // -----------------------------------------------------------------------------------

    // Rounds rather than truncates, so 10 ms at 48 kHz is 480 frames and not 479, and clamps
    // through the int32 range so an absurd millisecond value cannot wrap into a small count.
    int32 MsToFrames(double Ms, int32 SampleRate)
    {
        const double Frames = FMath::RoundToDouble(Ms * static_cast<double>(SampleRate) / 1000.0);
        return static_cast<int32>(FMath::Clamp(Frames,
            static_cast<double>(MIN_int32), static_cast<double>(MAX_int32)));
    }

    double DbToLinear(double Db)
    {
        return FMath::Pow(10.0, Db / 20.0);
    }

    // Zero and negative amplitudes report the documented floor instead of -inf, so the value
    // survives a JSON round trip and plots at the bottom of a scale.
    double LinearToDb(double Linear)
    {
        return (Linear > 0.0)
            ? 20.0 * FMath::Loge(Linear) / Ln10
            : PwSynthRender::SilenceFloorDb;
    }

    // Equal-power pan law, documented so a recipe's `pan` is reproducible outside this code:
    //   Angle = (pan + 1) * pi/4, left = cos(Angle), right = sin(Angle).
    // pan -1 -> (1, 0), pan 0 -> (0.7071, 0.7071), pan +1 -> (0, 1). left^2 + right^2 == 1 at
    // every position, so sweeping a layer across the image holds its power constant instead of
    // dipping 3 dB in the middle the way a linear law does. The centre position is therefore
    // -3 dB per channel, NOT unity - a mono layer at pan 0 sums to the same power, not the
    // same per-channel amplitude, as one hard-panned.
    void EqualPowerPanGains(double Pan, float& OutLeft, float& OutRight)
    {
        const double Clamped = FMath::Clamp(Pan, -1.0, 1.0);

        // The two endpoints are set exactly rather than trusting the trig: FMath::Cos(pi/2)
        // returns 6.12e-17, not 0, and that leak makes a hard-panned layer audible in the
        // opposite channel at -324 dBFS. Inaudible, but it is not silence, and "hard left
        // leaves the right channel untouched" is a property worth being exactly true.
        if (Clamped <= -1.0)
        {
            OutLeft = 1.f;
            OutRight = 0.f;
            return;
        }
        if (Clamped >= 1.0)
        {
            OutLeft = 0.f;
            OutRight = 1.f;
            return;
        }

        const double Angle = (Clamped + 1.0) * 0.25 * UE_DOUBLE_PI;
        OutLeft = static_cast<float>(FMath::Cos(Angle));
        OutRight = static_cast<float>(FMath::Sin(Angle));
    }

    // -----------------------------------------------------------------------------------
    // Amplitude envelope
    // -----------------------------------------------------------------------------------

    // Shaped position within one segment. The vocabulary is fixed by PwSynthRecipe's curve
    // table and its published meanings, and the exponent is PwSynthRender::CurveExponent:
    //   linear  straight line
    //   exp     fast start, slow finish   1 - (1-a)^2      (the natural shape of a decay)
    //   log     slow start, fast finish   a^2              (the natural shape of a swell)
    //   step    holds the segment's start value until the next point
    //   scurve  eased at both ends        a^2 * (3 - 2a)   (click-free crossfades)
    double ShapeAlpha(EPwSynthCurve Curve, double Alpha)
    {
        const double A = FMath::Clamp(Alpha, 0.0, 1.0);
        switch (Curve)
        {
        case EPwSynthCurve::Linear: return A;
        case EPwSynthCurve::Exp:    return 1.0 - FMath::Pow(1.0 - A, PwSynthRender::CurveExponent);
        case EPwSynthCurve::Log:    return FMath::Pow(A, PwSynthRender::CurveExponent);
        case EPwSynthCurve::Step:   return 0.0;
        case EPwSynthCurve::SCurve: return A * A * (3.0 - 2.0 * A);
        case EPwSynthCurve::Count:  break;
        }
        // Unreachable: ApplyAmpEnvelope rejects Count before it gets here.
        return A;
    }

    // Multiplies Mono by the piecewise envelope. Point times are LAYER-RELATIVE milliseconds
    // (0 == the layer's own first sample, not the render's), which is what PwSynthRecipe's
    // parser validates them against.
    //
    // Outside the point range the envelope HOLDS: before the first point it is the first
    // point's value, after the last it is the last point's. It does not ramp to zero on its
    // own - an envelope that should end silent has to say so with a final 0 point.
    //
    // Assumes ascending TimeMs, which the parser enforces; the segment cursor only walks
    // forward, so an out-of-order array degrades to holding the last reachable value rather
    // than reading out of bounds.
    bool ApplyAmpEnvelope(TArrayView<float> Mono, int32 SampleRate,
        const TArray<FPwSynthEnvelopePoint>& Points, FString& OutErrorCode, FString& OutError)
    {
        for (int32 Index = 0; Index < Points.Num(); ++Index)
        {
            if (Points[Index].Curve >= EPwSynthCurve::Count)
            {
                OutErrorCode = ErrorCodes::ERR_INVALID_RECIPE;
                OutError = FString::Printf(
                    TEXT("point %d carries curve value %d, which is outside the closed curve ")
                    TEXT("vocabulary; rendering it as linear would silently change the shape."),
                    Index, static_cast<int32>(Points[Index].Curve));
                return false;
            }
        }

        if (Points.Num() == 0 || SampleRate <= 0)
        {
            return true;
        }

        const double MsPerFrame = 1000.0 / static_cast<double>(SampleRate);
        const int32 LastPoint = Points.Num() - 1;
        int32 Segment = 0;

        for (int32 Frame = 0; Frame < Mono.Num(); ++Frame)
        {
            const double TimeMs = static_cast<double>(Frame) * MsPerFrame;

            while (Segment < LastPoint && TimeMs >= Points[Segment + 1].TimeMs)
            {
                ++Segment;
            }

            double Value;
            if (TimeMs <= Points[0].TimeMs)
            {
                Value = Points[0].Value;
            }
            else if (Segment >= LastPoint)
            {
                Value = Points[LastPoint].Value;
            }
            else
            {
                const FPwSynthEnvelopePoint& A = Points[Segment];
                const FPwSynthEnvelopePoint& B = Points[Segment + 1];
                const double Span = B.TimeMs - A.TimeMs;
                Value = (Span > 0.0)
                    ? FMath::Lerp(A.Value, B.Value, ShapeAlpha(A.Curve, (TimeMs - A.TimeMs) / Span))
                    : B.Value;
            }

            Mono[Frame] *= static_cast<float>(Value);
        }

        return true;
    }

    // -----------------------------------------------------------------------------------
    // Whole-bus measurement and shaping
    // -----------------------------------------------------------------------------------

    double MeasurePeakLinear(const FPwAudioBuffer& Bus)
    {
        double Peak = 0.0;
        for (const float Sample : Bus.Left)
        {
            Peak = FMath::Max(Peak, static_cast<double>(FMath::Abs(Sample)));
        }
        for (const float Sample : Bus.Right)
        {
            Peak = FMath::Max(Peak, static_cast<double>(FMath::Abs(Sample)));
        }
        return Peak;
    }

    // Gated integrated loudness of the stereo bus, in LUFS.
    //
    // ONE IMPLEMENTATION. This is a thin adapter over PwComputeLoudness (AudioGen/PwAudioFeatures.h),
    // which is the single place in the plugin that drives Audio::FLKFSAnalyzer - it owns the
    // interleave requirement, the chunked feed, the gated/ungated fallback and the sentinel
    // filtering. The renderer previously had its own copy of all of that; two independent
    // BS.1770 paths could report two loudnesses for the same bus, and only one of them would
    // have been the one an analysis verb published back.
    //
    // Returns false, with the analyzer's own sentence in OutReason, when no usable measurement
    // was produced (typically a render shorter than the analysis window). The caller must NOT
    // degrade that into peak normalization: -16 LUFS and -16 dBFS are different requests and
    // quietly swapping them is exactly the unsafe default rpc-design.md §3 forbids.
    bool MeasureIntegratedLufs(const FPwAudioBuffer& Bus, double& OutLufs, FString& OutReason)
    {
        FPwLoudnessResult Loudness;
        FString LoudnessCode;
        if (!PwComputeLoudness(Bus, Loudness, LoudnessCode, OutReason) || !Loudness.bMeasured)
        {
            return false;
        }

        OutLufs = Loudness.IntegratedLufs;
        return true;
    }

    void ApplyGain(FPwAudioBuffer& Bus, float GainLinear)
    {
        for (float& Sample : Bus.Left)
        {
            Sample *= GainLinear;
        }
        for (float& Sample : Bus.Right)
        {
            Sample *= GainLinear;
        }
    }

    // Linear ramps at the two edges of the render. The first sample of a fade-in is exactly
    // zero and so is the last sample of a fade-out, which is the whole point: they are the
    // guarantee that the buffer starts and ends without a step discontinuity, whatever the
    // layers left there. Fades run AFTER normalization, so an edge sits below the normalize
    // target by construction. Overlapping fades multiply.
    void ApplyFadeIn(FPwAudioBuffer& Bus, int32 NumFadeFrames)
    {
        const int32 Count = FMath::Clamp(NumFadeFrames, 0, Bus.NumFrames());
        for (int32 Frame = 0; Frame < Count; ++Frame)
        {
            const float Gain = static_cast<float>(Frame) / static_cast<float>(Count);
            Bus.Left[Frame] *= Gain;
            Bus.Right[Frame] *= Gain;
        }
    }

    void ApplyFadeOut(FPwAudioBuffer& Bus, int32 NumFadeFrames)
    {
        const int32 Total = Bus.NumFrames();
        const int32 Count = FMath::Clamp(NumFadeFrames, 0, Total);
        for (int32 Step = 0; Step < Count; ++Step)
        {
            const float Gain = static_cast<float>(Step) / static_cast<float>(Count);
            const int32 Frame = Total - 1 - Step;
            Bus.Left[Frame] *= Gain;
            Bus.Right[Frame] *= Gain;
        }
    }

    // Clamps to [-1, 1] and returns the number of samples the clamp actually MOVED, counted
    // across both channels. NaN is deliberately left alone: it is neither > 1 nor < -1, so a
    // generator bug surfaces in the exported file instead of being quietly rewritten to a
    // plausible value.
    int32 ClampToUnitRange(FPwAudioBuffer& Bus)
    {
        int32 Clamped = 0;
        const auto ClampChannel = [&Clamped](TArray<float>& Channel)
        {
            for (float& Sample : Channel)
            {
                if (Sample > 1.f)
                {
                    Sample = 1.f;
                    ++Clamped;
                }
                else if (Sample < -1.f)
                {
                    Sample = -1.f;
                    ++Clamped;
                }
            }
        };
        ClampChannel(Bus.Left);
        ClampChannel(Bus.Right);
        return Clamped;
    }
}

bool PwRenderRecipe(const FPwSynthRecipe& Recipe, FPwAudioBuffer& Out, FPwRenderReport& OutReport,
                    FString& OutErrorCode, FString& OutError)
{
    using namespace PwSynthDspInternal;

    // Failure is the default: both out-parameters are cleared up front and written only on the
    // single success path at the bottom.
    Out = FPwAudioBuffer();
    OutReport = FPwRenderReport();
    OutErrorCode.Reset();
    OutError.Reset();

    // Prefixes the recipe field path onto whichever error the failing stage already wrote, and
    // guarantees neither a half-mixed buffer nor a partial report survives the return.
    const auto Fail = [&Out, &OutReport, &OutError](const FString& FieldPath) -> bool
    {
        Out = FPwAudioBuffer();
        OutReport = FPwRenderReport();
        OutError = FieldPath + TEXT(": ") + OutError;
        return false;
    };

    if (Recipe.SampleRate <= 0)
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_RECIPE;
        OutError = FString::Printf(TEXT("sampleRate is %d; a render needs a positive rate."), Recipe.SampleRate);
        return false;
    }

    const int32 TotalFrames = MsToFrames(Recipe.DurationMs, Recipe.SampleRate);
    if (TotalFrames <= 0)
    {
        OutErrorCode = ErrorCodes::ERR_AUDIO_EMPTY_BUFFER;
        OutError = FString::Printf(
            TEXT("durationMs %.4f at %d Hz rounds to %d frames; an empty render and a silent one ")
            TEXT("are different outcomes and this one has no samples to produce."),
            Recipe.DurationMs, Recipe.SampleRate, TotalFrames);
        return false;
    }

    if (Recipe.Layers.Num() == 0)
    {
        OutErrorCode = ErrorCodes::ERR_AUDIO_EMPTY_BUFFER;
        OutError = TEXT("recipe carries no layers, so nothing can reach the mix bus. ")
                   TEXT("Reported as an error rather than a buffer of digital silence.");
        return false;
    }

    FPwAudioBuffer Bus;
    Bus.SampleRate = Recipe.SampleRate;
    Bus.SetNumFrames(TotalFrames);

    // Root of every substream in this render. Derive() hashes this seed with an index and
    // advances nothing, so no stage's randomness depends on what another stage drew.
    const FPwSeededRandom Root(Recipe.Seed);

    TArray<FPwLayerReport> LayerReports;
    LayerReports.Reserve(Recipe.Layers.Num());

    for (int32 LayerIndex = 0; LayerIndex < Recipe.Layers.Num(); ++LayerIndex)
    {
        const FPwSynthLayer& Layer = Recipe.Layers[LayerIndex];
        const FString LayerPath = FString::Printf(TEXT("layers[%d]"), LayerIndex);

        FPwLayerReport LayerReport;
        LayerReport.LayerIndex = LayerIndex;

        if (Layer.StartMs < 0.0)
        {
            OutErrorCode = ErrorCodes::ERR_INVALID_RECIPE;
            OutError = FString::Printf(
                TEXT("startMs is %.4f; a layer cannot begin before the render. Rendering the ")
                TEXT("pre-roll and discarding it would allocate without bound for no audible effect."),
                Layer.StartMs);
            return Fail(LayerPath);
        }

        const int32 StartFrame = MsToFrames(Layer.StartMs, Recipe.SampleRate);

        // A layer occupies the render from its own start to the end of the timeline.
        const int32 LayerFrames = TotalFrames - StartFrame;
        if (LayerFrames <= 0)
        {
            // Starts at or past the end of the render. This is a MEASURED nothing, not a skip:
            // the row is emitted with bMeasured true and FramesMixed 0, so a caller can tell it
            // apart from a layer the renderer never reached (rpc-design.md §1).
            LayerReport.bMeasured = true;
            LayerReport.FramesMixed = 0;
            LayerReport.PeakLinear = 0.0;
            LayerReport.PeakLinearPostGain = 0.0;
            LayerReports.Add(LayerReport);

            UE_LOG(LogPinWrightSubsystem, Warning,
                TEXT("PwRenderRecipe: layers[%d] starts at %.3f ms, at or past the %.3f ms render; ")
                TEXT("it contributes 0 frames."),
                LayerIndex, Layer.StartMs, Recipe.DurationMs);
            continue;
        }

        TArray<float> Scratch;
        Scratch.SetNumZeroed(LayerFrames);

        // Order-independent per-layer substream. Do NOT replace this with a single advancing
        // stream shared by all layers - that ties every layer's noise to evaluation order.
        FPwSeededRandom LayerRng = Root.Derive(LayerIndex);

        if (!DispatchGenerator(Layer.Generator.Kind, Layer.Generator.Params, Layer.PitchEnvelope,
                Layer.Modulation, Recipe.SampleRate, LayerRng, Scratch, OutErrorCode, OutError))
        {
            return Fail(LayerPath + TEXT(".generator"));
        }

        // No envelope means NO SHAPING: the layer runs at whatever level the generator produced
        // for its whole span, so it can step from silence to full amplitude at both edges and
        // click. That is the documented behaviour of an empty ampEnvelope (PwSynthRecipe.h) and
        // is deliberately not repaired here - inventing a default attack/release would make two
        // recipes that differ only in an omitted block render identically, and the recipe would
        // no longer describe the sound. The master fadeInMs/fadeOutMs cover the render's own
        // edges; a mid-render layer edge is the recipe author's to shape.
        if (Layer.AmpEnvelope.Num() > 0)
        {
            if (!ApplyAmpEnvelope(Scratch, Recipe.SampleRate, Layer.AmpEnvelope, OutErrorCode, OutError))
            {
                return Fail(LayerPath + TEXT(".ampEnvelope"));
            }
        }

        FPwDspSpan LayerSpan;
        LayerSpan.Left = Scratch.GetData();
        LayerSpan.Right = nullptr;      // mono layer chain
        LayerSpan.NumFrames = LayerFrames;

        for (int32 FxIndex = 0; FxIndex < Layer.Fx.Num(); ++FxIndex)
        {
            const FPwSynthFx& Fx = Layer.Fx[FxIndex];

            // The parser already rejects a master-only effect in a layer chain; re-checked here
            // because PwRenderRecipe is also reachable with a hand-built recipe, and the failure
            // it prevents is invisible (a stereo effect on a mono span does nothing at all).
            if (PwSynthFxSpec(Fx.Kind).bMasterOnly)
            {
                OutErrorCode = ErrorCodes::ERR_INVALID_RECIPE;
                OutError = FString::Printf(
                    TEXT("effect '%s' operates on the stereo mix bus, and a layer chain is mono, ")
                    TEXT("so it would be a silent no-op here. Move it to master.fx."),
                    PwSynthFxKindToString(Fx.Kind));
                return Fail(FString::Printf(TEXT("%s.fx[%d]"), *LayerPath, FxIndex));
            }

            if (!DispatchFx(Fx.Kind, Fx.Params, Recipe.SampleRate, LayerRng, LayerSpan,
                    OutErrorCode, OutError))
            {
                return Fail(FString::Printf(TEXT("%s.fx[%d]"), *LayerPath, FxIndex));
            }
        }

        // Keep the pre-gain generator diagnostic, then measure the post-gain contribution below.
        double LayerPeak = 0.0;
        for (const float Sample : Scratch)
        {
            LayerPeak = FMath::Max(LayerPeak, static_cast<double>(FMath::Abs(Sample)));
        }
        LayerReport.PeakLinear = LayerPeak;

        // Gain and the equal-power pan law fold into the two channel gains, then MixInto does
        // the windowing and reports what it actually wrote.
        float PanLeft = 0.f;
        float PanRight = 0.f;
        EqualPowerPanGains(Layer.Pan, PanLeft, PanRight);
        const float LeftGain = static_cast<float>(DbToLinear(Layer.GainDb)) * PanLeft;
        const float RightGain = static_cast<float>(DbToLinear(Layer.GainDb)) * PanRight;

        FPwAudioBuffer LayerBuffer;
        LayerBuffer.SampleRate = Recipe.SampleRate;
        LayerBuffer.SetNumFrames(LayerFrames, /*bZeroed=*/false);
        for (int32 Frame = 0; Frame < LayerFrames; ++Frame)
        {
            LayerBuffer.Left[Frame] = Scratch[Frame] * LeftGain;
            LayerBuffer.Right[Frame] = Scratch[Frame] * RightGain;
        }

        double LayerPeakPostGain = 0.0;
        for (int32 Frame = 0; Frame < LayerFrames; ++Frame)
        {
            LayerPeakPostGain = FMath::Max(LayerPeakPostGain,
                static_cast<double>(FMath::Abs(LayerBuffer.Left[Frame])));
            LayerPeakPostGain = FMath::Max(LayerPeakPostGain,
                static_cast<double>(FMath::Abs(LayerBuffer.Right[Frame])));
        }
        LayerReport.PeakLinearPostGain = LayerPeakPostGain;

        // The return, not the request: a layer truncated by the render's end reports the
        // clipped count, and a layer that landed nowhere reports 0.
        LayerReport.FramesMixed = LayerBuffer.MixInto(Bus, 1.f, StartFrame);
        LayerReport.bMeasured = true;
        LayerReports.Add(LayerReport);
    }

    // ---- master chain ----------------------------------------------------------------
    FPwSeededRandom MasterRng = Root.Derive(MasterStreamIndex);

    FPwDspSpan MasterSpan;
    MasterSpan.Left = Bus.Left.GetData();
    MasterSpan.Right = Bus.Right.GetData();
    MasterSpan.NumFrames = TotalFrames;

    for (int32 FxIndex = 0; FxIndex < Recipe.Master.Fx.Num(); ++FxIndex)
    {
        const FPwSynthFx& Fx = Recipe.Master.Fx[FxIndex];
        if (!DispatchFx(Fx.Kind, Fx.Params, Recipe.SampleRate, MasterRng, MasterSpan,
                OutErrorCode, OutError))
        {
            return Fail(FString::Printf(TEXT("master.fx[%d]"), FxIndex));
        }
    }

    // ---- normalize -------------------------------------------------------------------
    const double PeakLinearBeforeNormalize = MeasurePeakLinear(Bus);
    const double PeakDbBeforeNormalize = LinearToDb(PeakLinearBeforeNormalize);
    double NormalizeGainDb = 0.0;

    // The value the normalizer keyed on, published so the report can say what it measured
    // rather than leaving the caller to back-derive Target - NormalizeGainDb, which restates
    // the gain and proves nothing (rpc-design.md §1). Stays unmeasured unless a measurement
    // genuinely happened.
    double NormalizeInputDb = 0.0;
    bool bNormalizeMeasured = false;
    bool bNormalizeFailed = false;

    switch (Recipe.Master.Normalize.Mode)
    {
    case EPwSynthNormalizeMode::None:
        break;

    case EPwSynthNormalizeMode::Peak:
        if (PeakLinearBeforeNormalize > 0.0)
        {
            // Peak mode keys on the peak, so the two fields agree by construction here. They
            // are still both published: the reader should not have to know that Peak mode is
            // the case where they coincide.
            NormalizeInputDb = PeakDbBeforeNormalize;
            bNormalizeMeasured = true;
            NormalizeGainDb = Recipe.Master.Normalize.Target - PeakDbBeforeNormalize;
        }
        else
        {
            // No finite gain moves silence to a target. Reported as gain 0 beside a floored
            // peak rather than as a successful normalization that did nothing.
            UE_LOG(LogPinWrightSubsystem, Warning,
                TEXT("PwRenderRecipe: peak normalize to %.2f dBFS skipped - the bus is silent, ")
                TEXT("so no finite gain reaches the target."),
                Recipe.Master.Normalize.Target);
        }
        break;

    case EPwSynthNormalizeMode::Lufs:
    {
        double MeasuredLufs = 0.0;
        FString AnalyzerReason;
        if (Recipe.DurationMs < PwSynthRender::MinLufsDurationMs)
        {
            OutErrorCode = ErrorCodes::ERR_INVALID_RECIPE;
            OutError = FString::Printf(
                TEXT("durationMs %.4f is shorter than the %.0f ms LUFS analysis window, so the ")
                TEXT("loudness cannot be measured. Lengthen the render or use peak normalization ")
                TEXT("- falling back to peak silently would treat %.2f LUFS as %.2f dBFS."),
                Recipe.DurationMs, PwSynthRender::MinLufsDurationMs,
                Recipe.Master.Normalize.Target, Recipe.Master.Normalize.Target);
            bNormalizeFailed = true;
        }
        else if (!MeasureIntegratedLufs(Bus, MeasuredLufs, AnalyzerReason))
        {
            OutErrorCode = ErrorCodes::ERR_INVALID_RECIPE;
            OutError = FString::Printf(
                TEXT("LUFS normalization to %.2f LUFS could not be applied: the loudness analyzer ")
                TEXT("produced no usable measurement for this render (peak was %.2f dBFS).%s%s"),
                Recipe.Master.Normalize.Target, PeakDbBeforeNormalize,
                AnalyzerReason.IsEmpty() ? TEXT("") : TEXT(" "), *AnalyzerReason);
            bNormalizeFailed = true;
        }
        else
        {
            // The analyzer's own gated loudness. This is the only place the render's measured
            // LUFS is observable - no other field in the report can express it.
            NormalizeInputDb = MeasuredLufs;
            bNormalizeMeasured = true;
            NormalizeGainDb = Recipe.Master.Normalize.Target - MeasuredLufs;
        }
        break;
    }

    case EPwSynthNormalizeMode::Count:
        OutErrorCode = ErrorCodes::ERR_INVALID_RECIPE;
        OutError = TEXT("normalize.mode is outside the closed vocabulary; treating it as 'no normalize' ")
                   TEXT("would publish a level the recipe never asked for.");
        bNormalizeFailed = true;
        break;
    }

    if (bNormalizeFailed)
    {
        return Fail(TEXT("master.normalize"));
    }

    if (NormalizeGainDb != 0.0)
    {
        ApplyGain(Bus, static_cast<float>(DbToLinear(NormalizeGainDb)));
    }

    // ---- fades, then the final clamp -------------------------------------------------
    ApplyFadeIn(Bus, MsToFrames(Recipe.Master.FadeInMs, Recipe.SampleRate));
    ApplyFadeOut(Bus, MsToFrames(Recipe.Master.FadeOutMs, Recipe.SampleRate));

    const int32 ClampedSamples = ClampToUnitRange(Bus);

    OutReport.Layers = MoveTemp(LayerReports);
    OutReport.ClampedSamples = ClampedSamples;
    OutReport.PeakDbBeforeNormalize = PeakDbBeforeNormalize;
    OutReport.NormalizeInputDb = NormalizeInputDb;
    OutReport.bNormalizeMeasured = bNormalizeMeasured;
    OutReport.NormalizeGainDb = NormalizeGainDb;
    OutReport.bMeasured = true;

    Out = MoveTemp(Bus);
    return true;
}
