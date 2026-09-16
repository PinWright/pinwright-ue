// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "AudioGen/PwAudioFeatures.h"

#include "Compat/EngineVersionCompat.h"
#include "DSP/AlignedBuffer.h"
#include "DSP/AudioFFT.h"
#include "DSP/BlockCorrelator.h"
#include "Handlers/ErrorCodes.h"
#include "Math/UnrealMathUtility.h"

// Audio::FLKFSAnalyzer is the engine's ITU-R BS.1770 loudness meter and it arrived with UE 5.8
// (Runtime/SignalProcessing/Public/DSP/LKFSAnalyzer.h). Nothing on 5.3-5.7 measures gated LUFS:
// the AudioSynesthesia plugin's FLoudnessAnalyzer is a perceptual equal-loudness-curve meter, not
// BS.1770, and it lives in a plugin that is off by default. PwComputeLoudness therefore refuses
// on older engines rather than publishing a different quantity under the LUFS name.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
#include "DSP/LKFSAnalyzer.h"
#endif

// Named (not anonymous) namespace: this module builds with bUseUnity = true, and anonymous
// namespaces in merged TUs are the ODR-collision source the plugin's Build.cs comment warns about.
namespace PwAudioFeaturesInternal
{
    /**
     * Peak absolute sample below which an input is reported as silence instead of analysed.
     * 1e-9 is about -180 dBFS - roughly 36 dB below a 24-bit LSB - so no real recording lands
     * here while a zero-filled or never-rendered buffer always does. Same value and same reason
     * as PwStft's SilencePeak; the two are deliberately identical so a buffer that PwComputeStft
     * calls silent is not analysed as signal here.
     */
    constexpr float SilencePeak = 1e-9f;

    /**
     * Loudness values at or below this are treated as "the analyzer had nothing".
     * FLKFSAnalyzer initialises its aggregate stats to TNumericLimits<float>::Lowest(), which is
     * finite, so FMath::IsFinite alone does not catch an unfilled slot; and a genuinely silent
     * window converts to -inf. -600 LUFS is far below the smallest representable float32 signal
     * (a denormal sample measures around -450 LUFS) and far above Lowest(), so it separates both
     * sentinels from every real measurement.
     */
    constexpr double LufsSentinelFloor = -600.0;

    /** Floor for PeakDb / RmsDb so a pathological input plots at the bottom instead of at -inf. */
    constexpr double LevelDbFloor = -200.0;

    /** Frames per interleave chunk fed to FLKFSAnalyzer. Bounds the scratch buffer at 512 KB. */
    constexpr int32 LoudnessChunkFrames = 65536;

    // ---- onset detection constants ----------------------------------------------------------
    //
    // Every one of these is a time in milliseconds rather than a frame count, so the detector
    // behaves the same whichever FFT size and hop the caller handed it; they are converted to
    // frames against the spectrogram's own hop.

    /**
     * Full width of the sliding window whose median forms the adaptive threshold (so +/-50 ms).
     * Long enough that a single transient cannot drag the median up under itself - one onset
     * occupies two or three frames out of the ~75 in the window at hop 64 - and short enough to
     * follow a level change across a clip, which a global threshold cannot do. Varying level is
     * the normal case for game SFX, which is why this is a local median at all.
     */
    constexpr double OnsetMedianWindowMs = 100.0;

    /**
     * Margin added to the local median, in normalized-flux units (the curve is scaled so its
     * largest value is 1.0). 0.07 is librosa's peak_pick default under the same normalization,
     * kept rather than re-derived so the behaviour is comparable to a well-trodden reference.
     */
    constexpr double OnsetMedianMargin = 0.07;

    /** Full width of the local-maximum test (so +/-15 ms). Rejects the shoulders of one peak. */
    constexpr double OnsetLocalMaxWindowMs = 30.0;

    /**
     * Minimum spacing between reported onsets. 30 ms is the usual floor for separately perceived
     * transients, and it is what stops a single attack that smears across several hops from being
     * reported two or three times. Consequence to know: a genuine drum flam tighter than 30 ms is
     * reported as one onset.
     */
    constexpr double OnsetMinIntervalMs = 30.0;

    /**
     * Absolute gate: a frame must gain at least this fraction of the clip's mean total spectral
     * magnitude within one hop to be an onset at all.
     *
     * This is load-bearing, not belt-and-braces. Normalizing the flux curve to unit maximum makes
     * the median test scale-free, but it also means a clip with NO onsets still has a frame at
     * 1.0 - the largest of its numerical ripple - which the median test alone will happily
     * report. The gate is what makes a smooth sustained tone return zero onsets. Measured margins
     * at 5%: a 200 ms raised-cosine fade-in peaks at ~1% of mean frame magnitude per hop (4x
     * below the gate, rejected), while an impulse in an otherwise quiet clip runs tens of times
     * above it. The cost is the other direction: an onset that adds less than 5% of the clip's
     * average spectral magnitude in one hop is missed, so this is the knob to loosen if a caller
     * reports misses on dense material.
     */
    constexpr double OnsetMinFluxFraction = 0.05;

    // ---- pitch estimation constants ---------------------------------------------------------

    /** Lowest fundamental searched. Sets the longest lag, hence the analysis block size. */
    constexpr double PitchMinF0Hz = 50.0;

    /** Highest fundamental searched. See the PwEstimatePitch header comment for why not higher. */
    constexpr double PitchMaxF0Hz = 2000.0;

    /** Analysis blocks advance by BlockSize/4 - 75% overlap, ~10.7 ms per track point at 48 kHz. */
    constexpr int32 PitchHopDivisor = 4;

    /**
     * YIN absolute threshold on the cumulative-mean-normalized difference function. The paper's
     * value is 0.1; 0.15 is the common practical loosening and is used here because a game SFX
     * source is rarely as clean as the paper's speech corpus. Raising it further starts admitting
     * the first shallow dip of a noisy signal as a period.
     */
    constexpr double YinThreshold = 0.15;

    /** Below one semitone of total change across the confident span, motion is "stable". */
    constexpr double PitchStableOctaves = 1.0 / 12.0;

    /** Fewer confident windows than this and no line is fitted; motion stays "unknown". */
    constexpr int32 PitchMinPointsForMotion = 4;

    bool Fail(FString& OutErrorCode, FString& OutError, const TCHAR* Code, FString&& Message)
    {
        OutErrorCode = Code;
        OutError = MoveTemp(Message);
        return false;
    }

    /** True when the analyzer actually produced this number rather than leaving a sentinel. */
    bool IsUsableLufs(float Value)
    {
        return FMath::IsFinite(Value) && static_cast<double>(Value) > LufsSentinelFloor;
    }

    double AmplitudeToDb(double Amplitude)
    {
        if (!(Amplitude > 0.0))
        {
            return LevelDbFloor;
        }
        return FMath::Max(20.0 * FMath::LogX(10.0, Amplitude), LevelDbFloor);
    }

    /** Median of Values, which is sorted in place. Even counts average the two central items. */
    double MedianInPlace(TArray<double>& Values)
    {
        if (Values.Num() == 0)
        {
            return 0.0;
        }
        Values.Sort();
        const int32 Mid = Values.Num() / 2;
        if ((Values.Num() % 2) == 1)
        {
            return Values[Mid];
        }
        return 0.5 * (Values[Mid - 1] + Values[Mid]);
    }
}

// ==============================================================================================
// Loudness
// ==============================================================================================

bool PwComputeLoudness(const FPwAudioBuffer& In, FPwLoudnessResult& Out,
                       FString& OutErrorCode, FString& OutError)
{
    using namespace PwAudioFeaturesInternal;

    // Failure is the default: clear every out-parameter up front so no early return can leave a
    // partially populated result behind for a caller that ignored the bool.
    Out = FPwLoudnessResult();
    OutErrorCode.Reset();
    OutError.Reset();

    // ---- existence checks FIRST (rpc-design.md §7) ------------------------------------------
    // A silent buffer measures -inf LUFS and a peak of -inf dBFS. Both are numbers, and both read
    // as "extremely quiet audio" rather than as "no audio", so they are named here and answered
    // here rather than allowed to fall through into the analyzer.
    if (In.Left.Num() <= 0 && In.Right.Num() <= 0)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
            TEXT("Loudness: the buffer holds no samples in either channel."));
    }

    double PeakAbs = 0.0;
    double SumSquares = 0.0;
    int64 NumSamples = 0;
    for (const TArray<float>* Channel : { &In.Left, &In.Right })
    {
        for (const float Sample : *Channel)
        {
            const double Value = static_cast<double>(Sample);
            PeakAbs = FMath::Max(PeakAbs, FMath::Abs(Value));
            SumSquares += Value * Value;
            ++NumSamples;
        }
    }

    if (!(PeakAbs > static_cast<double>(SilencePeak)))
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
            FString::Printf(TEXT("Loudness: the buffer is digitally silent - the largest absolute ")
                TEXT("sample over %lld samples is %.3g, below the %.3g silence floor."),
                NumSamples, PeakAbs, static_cast<double>(SilencePeak)));
    }

    // ---- structural checks ------------------------------------------------------------------
    if (In.Left.Num() != In.Right.Num())
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Loudness: the two channels disagree on length (left %d frames, ")
                TEXT("right %d). FPwAudioBuffer's invariant is Left.Num() == Right.Num(); use ")
                TEXT("SetNumFrames rather than filling the channel arrays directly."),
                In.Left.Num(), In.Right.Num()));
    }
    if (In.SampleRate <= 0)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Loudness: sample rate is %d; it must be positive."), In.SampleRate));
    }

#if !UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
    // No BS.1770 meter on this engine (see the include block at the top of this file). Refused
    // rather than approximated: a caller normalizing to -16 LUFS must not silently be handed a
    // peak or a perceptual-loudness number under that name.
    return Fail(OutErrorCode, OutError, ErrorCodes::ERR_UNSUPPORTED_ENGINE_VERSION,
        FString::Printf(TEXT("Loudness: ITU-R BS.1770 measurement needs Audio::FLKFSAnalyzer, ")
            TEXT("which the engine only ships from UE 5.8; this editor is %d.%d. Peak and RMS ")
            TEXT("levels remain available through the analysis verbs."),
            ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION));
#else
    const int32 NumFrames = In.Left.Num();

    // ---- BS.1770 ----------------------------------------------------------------------------
    // FLKFSAnalyzer consumes INTERLEAVED frames (it builds its sliding window as
    // NumSlidingWindowFrames * NumChannels samples and deinterleaves inside
    // FMultichannelLoudnessAnalyzer). FPwAudioBuffer is deinterleaved, so handing it the two
    // planar arrays back to back would measure the left channel over the first half of a doubled
    // timeline and the right over the second - a wrong answer that looks like a right one.
    Audio::FLKFSAnalyzerSettings Settings;
    Settings.bCalculateOverallLoudnessRange = true;   // needed for LoudnessRangeLu
    Audio::FLKFSAnalyzer Analyzer(2, static_cast<float>(In.SampleRate), Settings);
    Audio::FLKFSAnalyzerResults Results;

    // Fed in chunks: the analyzer keeps its own sliding-buffer state across calls, so chunking
    // costs nothing and keeps a ten-minute stereo file from asking for a 230 MB interleave.
    TArray<float> Interleaved;
    Interleaved.SetNumUninitialized(FMath::Min(LoudnessChunkFrames, NumFrames) * 2);
    for (int32 Start = 0; Start < NumFrames; Start += LoudnessChunkFrames)
    {
        const int32 Count = FMath::Min(LoudnessChunkFrames, NumFrames - Start);
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Interleaved[Index * 2 + 0] = In.Left[Start + Index];
            Interleaved[Index * 2 + 1] = In.Right[Start + Index];
        }
        Analyzer.Analyze(TArrayView<const float>(Interleaved.GetData(), Count * 2), Results);
    }

    // Channel -1: the BS.1770 sum over channels, L and R each weighted 1.0. A dual-mono signal
    // therefore measures 3 LU above the same signal as one channel, which is the standard's
    // behaviour and not a doubling bug.
    const TArray<Audio::FLKFSResult>& Overall = Results.GetLoudnessResults();
    if (Overall.Num() == 0)
    {
        // The analyzer emits its first result only once it has accumulated a whole analysis
        // window: 0.4 s of audio plus the 4096-sample FFT window it slides. Report the measured
        // duration against that requirement rather than inventing a loudness for what was not
        // measured.
        const double DurationSeconds = static_cast<double>(NumFrames) / static_cast<double>(In.SampleRate);
        const double ApproxMinimumSeconds = 0.4 + 4096.0 / static_cast<double>(In.SampleRate);
        if (DurationSeconds < ApproxMinimumSeconds)
        {
            return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("Loudness: %.3f s of audio is too short to measure. ")
                    TEXT("FLKFSAnalyzer emits nothing until it has filled a 0.4 s analysis window ")
                    TEXT("plus one 4096-sample FFT window, about %.3f s at %d Hz."),
                    DurationSeconds, ApproxMinimumSeconds, In.SampleRate));
        }
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INTERNAL_ERROR,
            FString::Printf(TEXT("Loudness: FLKFSAnalyzer returned no results for %.3f s of audio, ")
                TEXT("which is long enough to measure."), DurationSeconds));
    }

    double MomentaryMax = LufsSentinelFloor;
    double ShortTermMax = LufsSentinelFloor;
    bool bHaveMomentary = false;
    bool bHaveShortTerm = false;
    for (const Audio::FLKFSResult& Result : Overall)
    {
        // Loudness is the instantaneous value over the 400 ms analysis window, i.e. BS.1770's
        // momentary measure; ShortTermLoudness is the 3 s one.
        if (IsUsableLufs(Result.Loudness))
        {
            MomentaryMax = FMath::Max(MomentaryMax, static_cast<double>(Result.Loudness));
            bHaveMomentary = true;
        }
        if (IsUsableLufs(Result.ShortTermLoudness))
        {
            ShortTermMax = FMath::Max(ShortTermMax, static_cast<double>(Result.ShortTermLoudness));
            bHaveShortTerm = true;
        }
    }

    const Audio::FLKFSResult& Last = Overall.Last();
    double Integrated = 0.0;
    if (IsUsableLufs(Last.GatedLoudness))
    {
        Integrated = static_cast<double>(Last.GatedLoudness);
    }
    else if (IsUsableLufs(Last.IntegratedLoudness))
    {
        // Gating left nothing above the -70 LU / -10 LU thresholds. The ungated running average
        // is the honest remaining answer; it is not relabelled as gated anywhere.
        Integrated = static_cast<double>(Last.IntegratedLoudness);
    }
    else
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INTERNAL_ERROR,
            TEXT("Loudness: FLKFSAnalyzer produced results but neither a gated nor an ungated ")
            TEXT("integrated loudness, on a buffer that is not silent."));
    }

    if (!bHaveMomentary || !bHaveShortTerm)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INTERNAL_ERROR,
            TEXT("Loudness: FLKFSAnalyzer produced results but no usable momentary or short-term ")
            TEXT("loudness, on a buffer that is not silent."));
    }

    Out.IntegratedLufs = Integrated;
    Out.ShortTermMaxLufs = ShortTermMax;
    Out.MomentaryMaxLufs = MomentaryMax;
    // The range is the one field the analyzer can decline to produce while still reporting a
    // loudness, so it carries its own measured flag rather than leaving 0 to mean both "steady"
    // and "unavailable".
    Out.bLoudnessRangeMeasured = Last.LoudnessRange.IsSet();
    if (Out.bLoudnessRangeMeasured)
    {
        Out.LoudnessRangeLu = FMath::Max(0.0,
            static_cast<double>(Last.LoudnessRange->Max - Last.LoudnessRange->Min));
    }
    Out.PeakDb = AmplitudeToDb(PeakAbs);
    Out.RmsDb = AmplitudeToDb(FMath::Sqrt(SumSquares / static_cast<double>(NumSamples)));
    Out.bMeasured = true;
    return true;
#endif
}

// ==============================================================================================
// Onset detection
// ==============================================================================================

bool PwDetectOnsets(const FPwStftResult& Stft, int32 SampleRate, TArray<FPwOnset>& Out,
                    FString& OutErrorCode, FString& OutError)
{
    using namespace PwAudioFeaturesInternal;

    Out.Reset();
    OutErrorCode.Reset();
    OutError.Reset();

    // ---- existence checks FIRST (rpc-design.md §7) ------------------------------------------
    if (Stft.NumFrames <= 0 || Stft.NumBins <= 0 || Stft.Magnitudes.Num() <= 0)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
            FString::Printf(TEXT("Onsets: the spectrogram is empty (%d frames x %d bins, %d ")
                TEXT("magnitudes)."), Stft.NumFrames, Stft.NumBins, Stft.Magnitudes.Num()));
    }

    // Scanned over the raw array rather than through the frame/bin indexing, so a silent input is
    // named before any structural inconsistency can turn it into a different-sounding failure.
    float PeakMagnitude = 0.f;
    for (const float Magnitude : Stft.Magnitudes)
    {
        PeakMagnitude = FMath::Max(PeakMagnitude, Magnitude);
    }
    if (!(PeakMagnitude > SilencePeak))
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
            FString::Printf(TEXT("Onsets: the spectrogram is digitally silent - its largest ")
                TEXT("magnitude is %.3g, below the %.3g silence floor."),
                PeakMagnitude, SilencePeak));
    }

    // ---- structural checks ------------------------------------------------------------------
    if (!Stft.IsValid())
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Onsets: malformed spectrogram - %d frames x %d bins needs %d ")
                TEXT("magnitudes but carries %d; fftSize=%d hopSize=%d."),
                Stft.NumFrames, Stft.NumBins, Stft.NumFrames * Stft.NumBins,
                Stft.Magnitudes.Num(), Stft.FftSize, Stft.HopSize));
    }
    if (SampleRate <= 0)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Onsets: sample rate is %d; it must be positive."), SampleRate));
    }

    // Structural guarantee rather than a convention the caller has to remember (rpc-design.md
    // §2): the spectrogram already knows the rate it was computed at, as BinHz * FftSize. A
    // mismatched pair would still produce onsets - just on the wrong clock, with no symptom.
    if (Stft.BinHz > 0.f)
    {
        const double ImpliedRate = static_cast<double>(Stft.BinHz) * static_cast<double>(Stft.FftSize);
        if (FMath::Abs(ImpliedRate - static_cast<double>(SampleRate)) > 0.01 * static_cast<double>(SampleRate))
        {
            return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("Onsets: sampleRate %d disagrees with the spectrogram, whose ")
                    TEXT("binHz %.6g over fftSize %d implies %.1f Hz. Pass the rate the ")
                    TEXT("spectrogram was computed at."),
                    SampleRate, Stft.BinHz, Stft.FftSize, ImpliedRate));
        }
    }

    if (Stft.NumFrames < 2)
    {
        // Not "no onsets": with one frame there is no inter-frame difference at all, so the
        // detector has nothing to be right or wrong about. Reporting zero here would be a false
        // negative dressed as a measurement.
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            TEXT("Onsets: the spectrogram has a single frame; spectral flux needs at least two. ")
            TEXT("Recompute the STFT over a longer signal or with a smaller fftSize/hopSize."));
    }

    // ---- half-wave rectified spectral flux ---------------------------------------------------
    // Positive differences only: a bin losing energy is a decay, not an onset, and letting decays
    // contribute would make every note-off look like a note-on. Bins are summed flat - no
    // perceptual band weighting was applied. Weighting would need a band layout justified per
    // material, and the STFT's magnitudes (not powers) already compress the dynamic range enough
    // that a bright transient does not swamp a low one.
    const int32 NumFrames = Stft.NumFrames;
    const int32 NumBins = Stft.NumBins;
    const float* Magnitudes = Stft.Magnitudes.GetData();

    TArray<double> RawFlux;
    RawFlux.SetNumZeroed(NumFrames);
    double MeanFrameMagnitude = 0.0;
    {
        double FrameMagnitudeSum = 0.0;
        for (int32 Bin = 0; Bin < NumBins; ++Bin)
        {
            FrameMagnitudeSum += static_cast<double>(Magnitudes[Bin]);
        }
        for (int32 Frame = 1; Frame < NumFrames; ++Frame)
        {
            const float* Current = Magnitudes + static_cast<int64>(Frame) * NumBins;
            const float* Previous = Current - NumBins;
            double Flux = 0.0;
            for (int32 Bin = 0; Bin < NumBins; ++Bin)
            {
                const double Difference = static_cast<double>(Current[Bin]) - static_cast<double>(Previous[Bin]);
                if (Difference > 0.0)
                {
                    Flux += Difference;
                }
                FrameMagnitudeSum += static_cast<double>(Current[Bin]);
            }
            RawFlux[Frame] = Flux;
        }
        MeanFrameMagnitude = FrameMagnitudeSum / static_cast<double>(NumFrames);
    }

    double MaxFlux = 0.0;
    for (const double Flux : RawFlux)
    {
        MaxFlux = FMath::Max(MaxFlux, Flux);
    }
    if (!(MaxFlux > 0.0))
    {
        // Perfectly stationary spectrum. A real measurement that found nothing - success with an
        // empty array, not a failure (rpc-design.md §1).
        return true;
    }

    TArray<double> NormFlux;
    NormFlux.SetNumUninitialized(NumFrames);
    for (int32 Frame = 0; Frame < NumFrames; ++Frame)
    {
        NormFlux[Frame] = RawFlux[Frame] / MaxFlux;
    }

    const double FramesPerMs = static_cast<double>(SampleRate) / (1000.0 * static_cast<double>(Stft.HopSize));
    const int32 MedianHalf = FMath::Max(1, FMath::RoundToInt(0.5 * OnsetMedianWindowMs * FramesPerMs));
    const int32 LocalMaxHalf = FMath::Max(1, FMath::RoundToInt(0.5 * OnsetLocalMaxWindowMs * FramesPerMs));
    const double AbsoluteGate = OnsetMinFluxFraction * MeanFrameMagnitude;

    // ---- adaptive-median peak picking --------------------------------------------------------
    TArray<double> MedianScratch;
    MedianScratch.Reserve(2 * MedianHalf + 1);

    // One interval before zero, so the first candidate always clears the spacing test without
    // relying on an infinity appearing in the subtraction.
    double LastAcceptedMs = -OnsetMinIntervalMs - 1.0;
    for (int32 Frame = 1; Frame < NumFrames; ++Frame)
    {
        if (RawFlux[Frame] < AbsoluteGate)
        {
            continue;
        }

        // Local maximum, with the tie broken toward the earlier frame so a plateau reports once.
        const int32 MaxLo = FMath::Max(1, Frame - LocalMaxHalf);
        const int32 MaxHi = FMath::Min(NumFrames - 1, Frame + LocalMaxHalf);
        bool bIsLocalMax = true;
        for (int32 Other = MaxLo; Other <= MaxHi && bIsLocalMax; ++Other)
        {
            if (Other < Frame)
            {
                bIsLocalMax = NormFlux[Frame] > NormFlux[Other];
            }
            else if (Other > Frame)
            {
                bIsLocalMax = NormFlux[Frame] >= NormFlux[Other];
            }
        }
        if (!bIsLocalMax)
        {
            continue;
        }

        // Adaptive threshold: local median plus a fixed margin. A global threshold fails on
        // material whose level changes across the clip, which is most game SFX.
        const int32 MedLo = FMath::Max(1, Frame - MedianHalf);
        const int32 MedHi = FMath::Min(NumFrames - 1, Frame + MedianHalf);
        MedianScratch.Reset();
        for (int32 Other = MedLo; Other <= MedHi; ++Other)
        {
            MedianScratch.Add(NormFlux[Other]);
        }
        const double LocalMedian = MedianInPlace(MedianScratch);
        if (NormFlux[Frame] <= LocalMedian + OnsetMedianMargin)
        {
            continue;
        }

        // Time of the frame CENTRE. The flux at frame f is caused by energy that entered the
        // window between frames f-1 and f, and for a tapered analysis window the flux peaks
        // roughly when the transient has travelled to the three-quarter point of the window -
        // about a quarter of a window after the centre. That residual quarter-window lead is the
        // inherent latency of flux-based detection, not something the centre convention removes;
        // it bounds the accuracy at about FftSize/2 samples, so a caller who needs tighter
        // onset times should recompute the STFT with a smaller FftSize.
        const double TimeMs = 1000.0
            * (static_cast<double>(Frame) * static_cast<double>(Stft.HopSize) + 0.5 * static_cast<double>(Stft.FftSize))
            / static_cast<double>(SampleRate);

        // Minimum inter-onset interval, applied greedily in time order: the first qualifying peak
        // wins and suppresses everything inside its window. One smeared attack therefore reports
        // once instead of two or three times.
        if (TimeMs - LastAcceptedMs < OnsetMinIntervalMs)
        {
            continue;
        }
        LastAcceptedMs = TimeMs;

        FPwOnset& Onset = Out.AddDefaulted_GetRef();
        Onset.TimeMs = TimeMs;
        Onset.Strength = NormFlux[Frame];
    }

    return true;
}

// ==============================================================================================
// Pitch estimation
// ==============================================================================================

bool PwEstimatePitch(TArrayView<const float> Mono, int32 SampleRate, FPwPitchResult& Out,
                     FString& OutErrorCode, FString& OutError)
{
    using namespace PwAudioFeaturesInternal;

    Out = FPwPitchResult();
    OutErrorCode.Reset();
    OutError.Reset();

    // ---- existence checks FIRST (rpc-design.md §7) ------------------------------------------
    if (Mono.Num() <= 0)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
            TEXT("Pitch: the signal holds no samples."));
    }

    double PeakAbs = 0.0;
    for (const float Sample : Mono)
    {
        PeakAbs = FMath::Max(PeakAbs, FMath::Abs(static_cast<double>(Sample)));
    }
    if (!(PeakAbs > static_cast<double>(SilencePeak)))
    {
        // Silence autocorrelates perfectly with itself at every lag, so a naive YIN run over it
        // returns the shortest searched lag at full confidence - a fabricated 2 kHz tone. Named
        // here so it can never be reported as one.
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
            FString::Printf(TEXT("Pitch: the signal is digitally silent - the largest absolute ")
                TEXT("sample over %d samples is %.3g, below the %.3g silence floor."),
                Mono.Num(), PeakAbs, static_cast<double>(SilencePeak)));
    }

    // ---- structural checks ------------------------------------------------------------------
    if (SampleRate <= 0)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Pitch: sample rate is %d; it must be positive."), SampleRate));
    }

    // Block sizing. The longest lag searched is one period of PitchMinF0Hz; the block is the next
    // power of two at twice that, so even the longest lag still overlaps half the block. Halving
    // the term count is the price of a long lag on a finite block, and capping it there is what
    // keeps the variance of the difference function bounded at the low end of the search.
    const int32 LongestLag = FMath::CeilToInt(static_cast<double>(SampleRate) / PitchMinF0Hz);
    int32 BlockSize = 512;
    while (BlockSize < 2 * LongestLag && BlockSize < 16384)
    {
        BlockSize <<= 1;
    }
    const int32 TauMin = FMath::Max(2, FMath::CeilToInt(static_cast<double>(SampleRate) / PitchMaxF0Hz));
    const int32 TauMax = FMath::Min(BlockSize / 2, LongestLag);
    if (TauMax <= TauMin + 1)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Pitch: sample rate %d Hz leaves no usable lag range between ")
                TEXT("%.0f Hz and %.0f Hz (lags %d..%d)."),
                SampleRate, PitchMaxF0Hz, PitchMinF0Hz, TauMin, TauMax));
    }
    if (Mono.Num() < BlockSize)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Pitch: %d samples is shorter than the %d-sample analysis block ")
                TEXT("(%.1f ms at %d Hz) needed to resolve a %.0f Hz fundamental."),
                Mono.Num(), BlockSize, 1000.0 * BlockSize / static_cast<double>(SampleRate),
                SampleRate, PitchMinF0Hz));
    }

    // ---- YIN over Audio::FBlockCorrelator ----------------------------------------------------
    // The correlator is configured with NO window and NO normalization on purpose. YIN's
    // difference function is an identity between the autocorrelation and two running energy sums,
    // and a taper breaks it: under a window x[n] and x[n+tau] carry different envelope gains, so
    // even at the true period the difference does not vanish and the confidence is understated.
    // The correlator's own normalization is skipped for the same reason - the per-term averaging
    // is done below, where the term count is known exactly.
    Audio::FBlockCorrelatorSettings CorrelatorSettings;
    CorrelatorSettings.Log2NumValuesInBlock = Audio::CeilLog2(BlockSize);
    CorrelatorSettings.WindowType = Audio::EWindowType::None;
    CorrelatorSettings.bDoNormalize = false;
    Audio::FBlockCorrelator Correlator(CorrelatorSettings);

    const int32 NumInputValues = Correlator.GetNumInputValues();
    const int32 NumOutputValues = Correlator.GetNumOutputValues();
    if (NumInputValues != BlockSize || NumOutputValues <= TauMax + 1)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INTERNAL_ERROR,
            FString::Printf(TEXT("Pitch: FBlockCorrelator was built for %d input / %d output ")
                TEXT("values, which does not fit the %d-sample block and %d-sample longest lag."),
                NumInputValues, NumOutputValues, BlockSize, TauMax));
    }

    Audio::FAlignedFloatBuffer Frame;
    Frame.AddZeroed(NumInputValues);
    Audio::FAlignedFloatBuffer Correlation;
    Correlation.AddZeroed(NumOutputValues);

    TArray<double> CumulativeEnergy;
    CumulativeEnergy.SetNumUninitialized(BlockSize + 1);
    TArray<double> MeanDifference;      // dbar(tau): mean squared difference per overlapping sample
    MeanDifference.SetNumZeroed(TauMax + 2);
    TArray<double> Cmnd;                // YIN's cumulative-mean-normalized difference
    Cmnd.SetNumZeroed(TauMax + 2);

    const int32 HopSize = FMath::Max(1, BlockSize / PitchHopDivisor);
    const double FrameSilenceEnergy = static_cast<double>(BlockSize)
        * static_cast<double>(SilencePeak) * static_cast<double>(SilencePeak);

    int32 NumAttempted = 0;
    int32 NumCorrelatorFailures = 0;

    for (int32 Start = 0; Start + BlockSize <= Mono.Num(); Start += HopSize)
    {
        FMemory::Memcpy(Frame.GetData(), Mono.GetData() + Start, BlockSize * sizeof(float));

        CumulativeEnergy[0] = 0.0;
        for (int32 Index = 0; Index < BlockSize; ++Index)
        {
            const double Value = static_cast<double>(Frame[Index]);
            CumulativeEnergy[Index + 1] = CumulativeEnergy[Index] + Value * Value;
        }
        const double TotalEnergy = CumulativeEnergy[BlockSize];
        if (TotalEnergy <= FrameSilenceEnergy)
        {
            // A silent gap inside an otherwise voiced clip. Omitted from the track rather than
            // entered with a zero f0, which would drag the median toward a frequency nothing
            // in the signal ever had.
            continue;
        }
        ++NumAttempted;

        Correlator.AutoCorrelate(Frame, Correlation);

        // Self-calibrate the correlator's scale instead of hard-coding the FFT convention: lag 0
        // of a zero-padded autocorrelation is by definition the block energy, so the ratio
        // recovers whatever constant the backend's forward/inverse scaling introduced.
        const double CorrelationAtZero = static_cast<double>(Correlation[0]);
        if (!(CorrelationAtZero > 0.0))
        {
            ++NumCorrelatorFailures;
            continue;
        }
        const double Scale = TotalEnergy / CorrelationAtZero;

        // d(tau) = sum over the overlap of (x[n] - x[n+tau])^2, expanded into the two partial
        // energy sums and the autocorrelation. Divided by the overlap length so the curve is a
        // mean per sample rather than a sum: without that, d(tau) shrinks with tau purely because
        // fewer terms remain, and the normalized function then slopes downward toward the long
        // lags and invents minima there.
        for (int32 Tau = 1; Tau <= TauMax + 1; ++Tau)
        {
            const double Correlated = static_cast<double>(Correlation[Tau]) * Scale;
            const double Difference = CumulativeEnergy[BlockSize - Tau]
                + (TotalEnergy - CumulativeEnergy[Tau]) - 2.0 * Correlated;
            MeanDifference[Tau] = FMath::Max(Difference, 0.0) / static_cast<double>(BlockSize - Tau);
        }

        Cmnd[0] = 1.0;
        double Running = 0.0;
        for (int32 Tau = 1; Tau <= TauMax + 1; ++Tau)
        {
            Running += MeanDifference[Tau];
            Cmnd[Tau] = (Running > 0.0) ? (MeanDifference[Tau] * static_cast<double>(Tau) / Running) : 1.0;
        }

        // First minimum below the absolute threshold. Taking the FIRST rather than the global
        // minimum is what keeps a harmonically rich tone from locking onto twice its period.
        int32 BestTau = INDEX_NONE;
        for (int32 Tau = TauMin; Tau <= TauMax; ++Tau)
        {
            if (Cmnd[Tau] < YinThreshold)
            {
                while (Tau + 1 <= TauMax && Cmnd[Tau + 1] < Cmnd[Tau])
                {
                    ++Tau;
                }
                BestTau = Tau;
                break;
            }
        }
        if (BestTau == INDEX_NONE)
        {
            // Nothing crossed the threshold. The window is still entered in the track, at the
            // low confidence its shallowest dip earned - dropping it instead would make a noisy
            // clip look sparse rather than unpitched, and the aggregate below needs to see the
            // unconfident windows to answer "how consistently is this pitched".
            double BestValue = TNumericLimits<double>::Max();
            for (int32 Tau = TauMin; Tau <= TauMax; ++Tau)
            {
                if (Cmnd[Tau] < BestValue)
                {
                    BestValue = Cmnd[Tau];
                    BestTau = Tau;
                }
            }
        }
        if (BestTau == INDEX_NONE)
        {
            continue;
        }

        // Parabolic interpolation of the lag through the three samples around the minimum. The
        // offset is clamped to half a lag: a vertex outside the bracketing samples means the
        // curve is not locally parabolic, and following it there would be extrapolation.
        double RefinedTau = static_cast<double>(BestTau);
        {
            const double Before = Cmnd[BestTau - 1];
            const double At = Cmnd[BestTau];
            const double After = Cmnd[BestTau + 1];
            const double Denominator = Before - 2.0 * At + After;
            if (Denominator > UE_DOUBLE_SMALL_NUMBER)
            {
                RefinedTau += FMath::Clamp(0.5 * (Before - After) / Denominator, -0.5, 0.5);
            }
        }
        RefinedTau = FMath::Clamp(RefinedTau,
            static_cast<double>(SampleRate) / PitchMaxF0Hz,
            static_cast<double>(SampleRate) / PitchMinF0Hz);

        FPwPitchPointResult& Point = Out.Track.AddDefaulted_GetRef();
        Point.TimeMs = 1000.0 * (static_cast<double>(Start) + 0.5 * static_cast<double>(BlockSize))
            / static_cast<double>(SampleRate);
        Point.F0Hz = static_cast<double>(SampleRate) / RefinedTau;
        // Confidence from the depth of the minimum, taken at the sampled lag rather than at the
        // interpolated vertex: the sampled value is the one the algorithm actually observed.
        Point.Confidence = FMath::Clamp(1.0 - Cmnd[BestTau], 0.0, 1.0);
    }

    if (NumAttempted > 0 && NumCorrelatorFailures == NumAttempted)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INTERNAL_ERROR,
            FString::Printf(TEXT("Pitch: FBlockCorrelator returned a zero autocorrelation for all ")
                TEXT("%d analysed windows, which means its FFT backend failed to initialise."),
                NumAttempted));
    }

    // ---- aggregate ---------------------------------------------------------------------------
    Out.bMeasured = true;
    Out.Motion = TEXT("unknown");
    if (Out.Track.Num() == 0)
    {
        // Ran to completion, found no analysable window. Honest as reported: bMeasured true,
        // MedianF0Hz 0, no track.
        return true;
    }

    {
        TArray<double> Confidences;
        Confidences.Reserve(Out.Track.Num());
        for (const FPwPitchPointResult& Point : Out.Track)
        {
            Confidences.Add(Point.Confidence);
        }
        Out.Confidence = MedianInPlace(Confidences);
    }

    if (Out.Confidence < PwPitchConfidenceFloor)
    {
        // Measured, not pitched. MedianF0Hz stays 0 rather than carrying the median of a set of
        // frequencies read off noise - a plausible number is worse here than no number.
        return true;
    }

    TArray<double> ConfidentF0;
    TArray<double> ConfidentSeconds;
    TArray<double> ConfidentLog2F0;
    ConfidentF0.Reserve(Out.Track.Num());
    ConfidentSeconds.Reserve(Out.Track.Num());
    ConfidentLog2F0.Reserve(Out.Track.Num());
    for (const FPwPitchPointResult& Point : Out.Track)
    {
        if (Point.Confidence >= PwPitchConfidenceFloor && Point.F0Hz > 0.0)
        {
            ConfidentF0.Add(Point.F0Hz);
            ConfidentSeconds.Add(Point.TimeMs / 1000.0);
            ConfidentLog2F0.Add(FMath::Log2(Point.F0Hz));
        }
    }
    if (ConfidentF0.Num() == 0)
    {
        return true;
    }

    {
        TArray<double> F0Copy = ConfidentF0;
        Out.MedianF0Hz = MedianInPlace(F0Copy);
    }

    // Motion: least-squares line through (seconds, log2 f0) over the confident points only.
    // Fitting log2 makes the criterion musical rather than absolute - a 50 Hz drift matters at
    // 100 Hz and does not at 2 kHz - and fitting a SIGNED slope is what distinguishes a chirp
    // from its reverse (rpc-design.md §6); any magnitude-only measure scores them identically.
    if (ConfidentF0.Num() >= PitchMinPointsForMotion)
    {
        const double Count = static_cast<double>(ConfidentF0.Num());
        double SumT = 0.0, SumY = 0.0, SumTT = 0.0, SumTY = 0.0;
        for (int32 Index = 0; Index < ConfidentF0.Num(); ++Index)
        {
            const double T = ConfidentSeconds[Index];
            const double Y = ConfidentLog2F0[Index];
            SumT += T;
            SumY += Y;
            SumTT += T * T;
            SumTY += T * Y;
        }
        const double Denominator = Count * SumTT - SumT * SumT;
        if (Denominator > UE_DOUBLE_SMALL_NUMBER)
        {
            const double SlopeOctavesPerSecond = (Count * SumTY - SumT * SumY) / Denominator;
            const double Span = ConfidentSeconds.Last() - ConfidentSeconds[0];
            const double TotalOctaves = SlopeOctavesPerSecond * Span;
            if (FMath::Abs(TotalOctaves) < PitchStableOctaves)
            {
                Out.Motion = TEXT("stable");
            }
            else
            {
                Out.Motion = (TotalOctaves > 0.0) ? TEXT("rising") : TEXT("falling");
            }
        }
    }

    return true;
}
