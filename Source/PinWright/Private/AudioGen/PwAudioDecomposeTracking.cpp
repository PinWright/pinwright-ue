// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "AudioGen/PwAudioDecompose.h"

#include "Handlers/ErrorCodes.h"
#include "Math/UnrealMathUtility.h"
#include "PinWrightSubsystem.h"

// The front half of the decomposition analyzer: harmonic/percussive separation, spectral peak
// detection with sub-bin refinement, partial tracking, and transient characterisation. The
// contract, the units, and the design rationale for every constant tuned here are in
// AudioGen/PwAudioDecompose.h - this file is the arithmetic.

// Named (not anonymous) namespace: this module builds with bUseUnity = true, and anonymous
// namespaces in merged TUs are the ODR-collision source the plugin's Build.cs comment warns about.
namespace PwAudioDecomposeInternal
{
    bool Fail(FString& OutErrorCode, FString& OutError, const TCHAR* Code, FString&& Message)
    {
        OutErrorCode = Code;
        OutError = MoveTemp(Message);
        return false;
    }

    /**
     * Peak magnitude at or below which a spectrogram is reported as silence instead of analysed.
     * Same value and same reason as PwStft's and PwAudioFeatures' SilencePeak - deliberately
     * identical so a spectrogram one stage calls silent is not analysed as signal by the next.
     */
    constexpr float SilencePeak = 1e-9f;

    /** Floor for the log domain used by the parabolic peak refinement. 1e-12 is -240 dBFS. */
    constexpr double LogMagnitudeFloor = 1e-12;

    /** Floor for reported dB levels so a pathological input reads as very quiet, not as -inf. */
    constexpr double LevelDbFloor = -200.0;

    // ---- HPSS ------------------------------------------------------------------------------
    // Both kernels are odd so the window is symmetric about the cell being estimated. The
    // justification for the two sizes, in both frames/bins and their equivalent ms/Hz, is in the
    // header; they are fixed rather than caller-tunable because a caller has no way to pick them
    // without knowing the same derivation.

    /** Frames in the median-across-time window that estimates the harmonic component. */
    constexpr int32 HpssTimeKernelFrames = 31;

    /** Bins in the median-across-frequency window that estimates the percussive component. */
    constexpr int32 HpssFreqKernelBins = 31;

    /**
     * Fewest frames HPSS will accept. With one or two frames there is no time axis to call
     * anything horizontal, so "all of it is harmonic" would be an artefact of the window rather
     * than a measurement - refused instead of reported (rpc-design.md §1).
     */
    constexpr int32 HpssMinFrames = 3;

    // ---- partial tracking ------------------------------------------------------------------

    /** Continuation tolerance floor, in bins. See the header for the derivation. */
    constexpr double TrackToleranceBins = 2.0;

    /** Continuation tolerance as a fraction of the track's own frequency (~half a semitone). */
    constexpr double TrackToleranceRelative = 0.03;

    /** How long a track may go unmatched before it dies, ms. Under half an analysis window. */
    constexpr double TrackMaxGapMs = 20.0;

    /** Per-frame peak-list cap = MaxPartials * this, clamped to the two bounds below. */
    constexpr int32 PeaksPerFrameMultiplier = 4;
    constexpr int32 MinPeaksPerFrame = 32;
    constexpr int32 MaxPeaksPerFrame = 512;

    // ---- transients ------------------------------------------------------------------------

    /**
     * Level below the burst's own peak power at which the transient is considered over (and,
     * searching backwards, not yet begun). 20 dB is 1% of peak power - past that the burst is no
     * longer audible over whatever follows it.
     */
    constexpr double TransientDecayDb = 20.0;

    /**
     * Hard bound on the half-extent of a transient window, ms. Without it the -20 dB test never
     * fires on a sound that sustains after its attack and the "transient" becomes the whole clip.
     * A quarter second is past every attack worth calling one: percussive attacks are 5-100 ms
     * and even a slow bowed onset reaches steady state inside ~200 ms.
     */
    constexpr double TransientMaxHalfLengthMs = 250.0;

    /** Cumulative-power percentiles reported as FPwTransient's LowHz / HighHz. */
    constexpr double TransientLowPercentile = 0.05;
    constexpr double TransientHighPercentile = 0.95;

    /** Time of the centre of an analysis frame, ms. The convention PwDetectOnsets reports on. */
    double FrameCentreMs(int32 Frame, int32 HopSize, int32 FftSize, int32 SampleRate)
    {
        return 1000.0
            * (static_cast<double>(Frame) * static_cast<double>(HopSize) + 0.5 * static_cast<double>(FftSize))
            / static_cast<double>(SampleRate);
    }

    /** Inverse of FrameCentreMs, unrounded, so the caller can decide what out-of-range means. */
    double FrameIndexAtMs(double TimeMs, int32 HopSize, int32 FftSize, int32 SampleRate)
    {
        const double Samples = TimeMs * static_cast<double>(SampleRate) / 1000.0;
        return (Samples - 0.5 * static_cast<double>(FftSize)) / static_cast<double>(HopSize);
    }

    /** dBFS under the STFT magnitude convention, floored rather than allowed to reach -inf. */
    double AmplitudeToDb(double Amplitude)
    {
        if (!(Amplitude > 0.0))
        {
            return LevelDbFloor;
        }
        return FMath::Max(LevelDbFloor, 20.0 * FMath::LogX(10.0, Amplitude));
    }

    /** Log domain for the parabolic refinement. Floored far below any real signal, never -inf. */
    double PeakLogDb(double Amplitude)
    {
        return 20.0 * FMath::LogX(10.0, FMath::Max(Amplitude, LogMagnitudeFloor));
    }

    /**
     * Median of a scratch window, sorted in place. Even counts average the two central values so
     * a truncated edge window of length 2 does not systematically favour the lower sample.
     */
    float MedianInPlace(TArray<float, TInlineAllocator<64>>& Values)
    {
        const int32 Num = Values.Num();
        if (Num <= 0)
        {
            return 0.f;
        }
        Values.Sort();
        if ((Num % 2) == 1)
        {
            return Values[Num / 2];
        }
        return 0.5f * (Values[Num / 2 - 1] + Values[Num / 2]);
    }

    /**
     * The degenerate-case gate every entry point in this file runs first.
     *
     * One function rather than three copies so the ORDER cannot drift between the entry points
     * (rpc-design.md §2 - a structural guarantee beats repeated discipline), and the order itself
     * is the §7 rule: existence before thresholds, and NON-FINITE BEFORE SILENCE. That last one
     * is not stylistic - FMath::Max(0.0, NaN) returns 0 because every comparison against NaN is
     * false, so a NaN-filled spectrogram scanned for its peak first measures as digital silence
     * and would be reported as "there was no signal".
     */
    bool ValidateSpectrogram(const FPwStftResult& In, const TCHAR* Label,
                             FString& OutErrorCode, FString& OutError)
    {
        if (In.NumFrames <= 0 || In.NumBins <= 0 || In.Magnitudes.Num() <= 0)
        {
            return Fail(OutErrorCode, OutError, ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
                FString::Printf(TEXT("%s: the spectrogram is empty (%d frames x %d bins, %d ")
                    TEXT("magnitudes)."), Label, In.NumFrames, In.NumBins, In.Magnitudes.Num()));
        }

        int32 NonFinite = 0;
        int32 FirstNonFinite = INDEX_NONE;
        for (int32 Index = 0; Index < In.Magnitudes.Num(); ++Index)
        {
            if (!FMath::IsFinite(In.Magnitudes[Index]))
            {
                ++NonFinite;
                if (FirstNonFinite == INDEX_NONE)
                {
                    FirstNonFinite = Index;
                }
            }
        }
        if (NonFinite > 0)
        {
            return Fail(OutErrorCode, OutError, ErrorCodes::ERR_AUDIO_NON_FINITE_SAMPLES,
                FString::Printf(TEXT("%s: the spectrogram holds %d non-finite magnitudes (NaN or ")
                    TEXT("infinity), first at frame %d bin %d. One of them poisons every median, ")
                    TEXT("every peak comparison and every energy sum downstream, and because NaN ")
                    TEXT("compares false against every threshold it would otherwise be reported ")
                    TEXT("as digital silence."),
                    Label, NonFinite, FirstNonFinite / In.NumBins, FirstNonFinite % In.NumBins));
        }

        float PeakMagnitude = 0.f;
        for (const float Magnitude : In.Magnitudes)
        {
            PeakMagnitude = FMath::Max(PeakMagnitude, Magnitude);
        }
        if (!(PeakMagnitude > SilencePeak))
        {
            return Fail(OutErrorCode, OutError, ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
                FString::Printf(TEXT("%s: the spectrogram is digitally silent - its largest ")
                    TEXT("magnitude is %.3g, below the %.3g silence floor."),
                    Label, PeakMagnitude, SilencePeak));
        }

        if (!In.IsValid())
        {
            return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("%s: malformed spectrogram - %d frames x %d bins needs %d ")
                    TEXT("magnitudes but carries %d; fftSize=%d hopSize=%d."),
                    Label, In.NumFrames, In.NumBins, In.NumFrames * In.NumBins,
                    In.Magnitudes.Num(), In.FftSize, In.HopSize));
        }

        return true;
    }

    /**
     * The rate must be the one the spectrogram was computed at. Cross-checked against
     * BinHz * FftSize rather than trusted: a mismatched pair still produces plausible output,
     * just on the wrong frequency and time axes, with no symptom anywhere downstream.
     */
    bool ValidateSampleRate(const FPwStftResult& In, int32 SampleRate, const TCHAR* Label,
                            FString& OutErrorCode, FString& OutError)
    {
        if (SampleRate <= 0)
        {
            return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("%s: sample rate is %d; it must be positive."), Label, SampleRate));
        }
        if (In.BinHz > 0.f)
        {
            const double ImpliedRate = static_cast<double>(In.BinHz) * static_cast<double>(In.FftSize);
            if (FMath::Abs(ImpliedRate - static_cast<double>(SampleRate)) > 0.01 * static_cast<double>(SampleRate))
            {
                return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
                    FString::Printf(TEXT("%s: sampleRate %d disagrees with the spectrogram, whose ")
                        TEXT("binHz %.6g over fftSize %d implies %.1f Hz. Pass the rate the ")
                        TEXT("spectrogram was computed at."),
                        Label, SampleRate, In.BinHz, In.FftSize, ImpliedRate));
            }
        }
        return true;
    }

    /** Analysed span of the signal the spectrogram covers, ms. */
    double SpectrogramDurationMs(const FPwStftResult& In, int32 SampleRate)
    {
        const double Samples = static_cast<double>(In.NumFrames - 1) * static_cast<double>(In.HopSize)
            + static_cast<double>(In.FftSize);
        return 1000.0 * Samples / static_cast<double>(SampleRate);
    }

    // ---- peak picking ----------------------------------------------------------------------

    /** One refined spectral peak in one frame. */
    struct FSpectralPeak
    {
        double FreqHz = 0.0;
        double AmpLinear = 0.0;
    };

    /**
     * Local maxima of one frame, refined to sub-bin frequency by a parabola through the LOG
     * magnitudes of the three bins around the peak.
     *
     * The log domain is what buys the precision: a windowed sinusoid's mainlobe is close to
     * parabolic in dB and distinctly not parabolic in linear magnitude, so the same three-point
     * fit that leaves ~0.02 bins of bias in dB leaves an order of magnitude more in linear.
     *
     * DC and Nyquist are excluded because they have no neighbour on one side - there is no
     * parabola to fit, and reporting them at their bin centre would mix two accuracies in one
     * array. The local-max test uses > on the left and >= on the right so a two-bin plateau
     * reports once, matching PwDetectOnsets' tie-break.
     */
    void FindFramePeaks(const float* FrameMagnitudes, int32 NumBins, double BinHz,
                        double MinAmpDbRelative, TArray<FSpectralPeak>& OutPeaks)
    {
        OutPeaks.Reset();

        double FrameMax = 0.0;
        for (int32 Bin = 0; Bin < NumBins; ++Bin)
        {
            FrameMax = FMath::Max(FrameMax, static_cast<double>(FrameMagnitudes[Bin]));
        }
        if (!(FrameMax > SilencePeak))
        {
            // A silent frame has no peaks. Returning none is the measurement; inventing one at
            // the numerically largest noise bin would seed a track out of nothing.
            return;
        }

        const double ThresholdDb = PeakLogDb(FrameMax) + MinAmpDbRelative;

        for (int32 Bin = 1; Bin + 1 < NumBins; ++Bin)
        {
            const double Centre = static_cast<double>(FrameMagnitudes[Bin]);
            const double Lower = static_cast<double>(FrameMagnitudes[Bin - 1]);
            const double Upper = static_cast<double>(FrameMagnitudes[Bin + 1]);
            if (!(Centre > Lower) || !(Centre >= Upper))
            {
                continue;
            }

            const double CentreDb = PeakLogDb(Centre);
            if (CentreDb < ThresholdDb)
            {
                continue;
            }

            const double LowerDb = PeakLogDb(Lower);
            const double UpperDb = PeakLogDb(Upper);

            // A maximum has a negative second difference. When it is ~0 the three points are
            // collinear and there is no vertex to find, so the estimate stays on the bin centre
            // rather than dividing by an epsilon and landing anywhere.
            const double SecondDifference = LowerDb - 2.0 * CentreDb + UpperDb;
            double Delta = 0.0;
            if (SecondDifference < -UE_DOUBLE_SMALL_NUMBER)
            {
                Delta = FMath::Clamp(0.5 * (LowerDb - UpperDb) / SecondDifference, -0.5, 0.5);
            }

            const double VertexDb = CentreDb - 0.25 * (LowerDb - UpperDb) * Delta;

            FSpectralPeak& Peak = OutPeaks.AddDefaulted_GetRef();
            Peak.FreqHz = (static_cast<double>(Bin) + Delta) * BinHz;
            Peak.AmpLinear = FMath::Pow(10.0, VertexDb / 20.0);
        }
    }

    // ---- track harvesting -------------------------------------------------------------------

    /** A track still accepting points. */
    struct FActiveTrack
    {
        FPwPartialTrack Track;
        int32 LastMatchedFrame = INDEX_NONE;
        double LastFreqHz = 0.0;
    };

    /**
     * Bounded collector for finished tracks plus the drop bookkeeping the success note is built
     * from. Bounded on purpose: a noisy input can birth hundreds of thousands of tracks, and a
     * finished track's PeakAmpLinear never changes, so pruning down to the cap mid-run is exactly
     * equivalent to pruning once at the end - at a fraction of the memory.
     */
    struct FTrackHarvest
    {
        TArray<FPwPartialTrack> Retained;
        int32 ShortDropped = 0;
        int32 LongEnough = 0;
        double LoudestDroppedAmp = 0.0;
    };

    void SortLoudestFirst(TArray<FPwPartialTrack>& Tracks)
    {
        // Total order, so the output does not depend on the sort's internal pivot choices.
        Tracks.Sort([](const FPwPartialTrack& A, const FPwPartialTrack& B)
        {
            if (A.PeakAmpLinear != B.PeakAmpLinear)
            {
                return A.PeakAmpLinear > B.PeakAmpLinear;
            }
            if (A.StartMs != B.StartMs)
            {
                return A.StartMs < B.StartMs;
            }
            return A.MeanFreqHz < B.MeanFreqHz;
        });
    }

    void CompactHarvest(FTrackHarvest& Harvest, int32 MaxPartials)
    {
        if (Harvest.Retained.Num() <= MaxPartials)
        {
            return;
        }
        SortLoudestFirst(Harvest.Retained);
        Harvest.LoudestDroppedAmp = FMath::Max(Harvest.LoudestDroppedAmp,
            Harvest.Retained[MaxPartials].PeakAmpLinear);
        Harvest.Retained.SetNum(MaxPartials);
    }

    /** Summarise a finished track, apply the duration filter, and hand it to the harvest. */
    void FinishTrack(FActiveTrack& Active, const FPwDecomposeSettings& Settings, FTrackHarvest& Harvest)
    {
        FPwPartialTrack& Track = Active.Track;
        if (Track.Points.Num() <= 0)
        {
            return;
        }

        double FreqSum = 0.0;
        double PeakAmp = 0.0;
        for (const FPwPartialPoint& Point : Track.Points)
        {
            FreqSum += Point.FreqHz;
            PeakAmp = FMath::Max(PeakAmp, Point.AmpLinear);
        }

        Track.StartMs = Track.Points[0].TimeMs;
        Track.EndMs = Track.Points.Last().TimeMs;
        Track.MeanFreqHz = FreqSum / static_cast<double>(Track.Points.Num());
        Track.PeakAmpLinear = PeakAmp;
        Track.FreqDriftHz = Track.Points.Last().FreqHz - Track.Points[0].FreqHz;

        if (Track.EndMs - Track.StartMs < Settings.PartialMinDurationMs)
        {
            ++Harvest.ShortDropped;
            return;
        }

        ++Harvest.LongEnough;
        Harvest.Retained.Add(MoveTemp(Track));

        // int64 comparison so a caller passing a huge MaxPartials cannot overflow the threshold.
        if (static_cast<int64>(Harvest.Retained.Num()) >= static_cast<int64>(Settings.MaxPartials) + 64)
        {
            CompactHarvest(Harvest, Settings.MaxPartials);
        }
    }

    /** One candidate (track, peak) continuation inside tolerance. */
    struct FMatchCandidate
    {
        double DeltaHz = 0.0;
        int32 TrackIndex = 0;
        int32 PeakIndex = 0;
    };
}

// ==============================================================================================
// Harmonic / percussive separation
// ==============================================================================================

bool PwSeparateHarmonicPercussive(const FPwStftResult& In, TArray<float>& OutHarmonic,
                                  TArray<float>& OutPercussive, FString& OutErrorCode, FString& OutError)
{
    using namespace PwAudioDecomposeInternal;

    OutHarmonic.Reset();
    OutPercussive.Reset();
    OutErrorCode.Reset();
    OutError.Reset();

    if (!ValidateSpectrogram(In, TEXT("HPSS"), OutErrorCode, OutError))
    {
        return false;
    }

    if (In.NumFrames < HpssMinFrames)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("HPSS: the spectrogram has %d frames; median-filter separation ")
                TEXT("needs at least %d. With fewer there is no time axis to call anything ")
                TEXT("horizontal, so every bin would be reported as harmonic by construction. ")
                TEXT("Recompute the STFT over a longer signal or with a smaller hopSize."),
                In.NumFrames, HpssMinFrames));
    }

    const int32 NumFrames = In.NumFrames;
    const int32 NumBins = In.NumBins;
    const int32 NumCells = NumFrames * NumBins;
    const float* Source = In.Magnitudes.GetData();

    TArray<float> Harmonic;
    TArray<float> Percussive;
    Harmonic.SetNumUninitialized(NumCells);
    Percussive.SetNumUninitialized(NumCells);
    float* HarmonicData = Harmonic.GetData();
    float* PercussiveData = Percussive.GetData();

    const int32 TimeHalf = HpssTimeKernelFrames / 2;
    const int32 FreqHalf = HpssFreqKernelBins / 2;

    TArray<float, TInlineAllocator<64>> Scratch;

    // Harmonic estimate: median across TIME, per bin. A sustained partial occupies every frame in
    // the window and survives; a transient occupies a handful and is rejected as an outlier.
    // Windows are TRUNCATED at the edges rather than zero-padded - padding with zeros would pull
    // the first and last medians down and manufacture a percussive-looking onset at frame 0.
    for (int32 Bin = 0; Bin < NumBins; ++Bin)
    {
        for (int32 Frame = 0; Frame < NumFrames; ++Frame)
        {
            const int32 Lo = FMath::Max(0, Frame - TimeHalf);
            const int32 Hi = FMath::Min(NumFrames - 1, Frame + TimeHalf);
            Scratch.Reset();
            for (int32 Other = Lo; Other <= Hi; ++Other)
            {
                Scratch.Add(*(Source + static_cast<int64>(Other) * NumBins + Bin));
            }
            *(HarmonicData + static_cast<int64>(Frame) * NumBins + Bin) = MedianInPlace(Scratch);
        }
    }

    // Percussive estimate: median across FREQUENCY, per frame. The mirror argument - a broadband
    // transient is flat across the window and survives, a sinusoid's 4-bin mainlobe does not.
    for (int32 Frame = 0; Frame < NumFrames; ++Frame)
    {
        const float* FrameMagnitudes = Source + static_cast<int64>(Frame) * NumBins;
        float* FrameOut = PercussiveData + static_cast<int64>(Frame) * NumBins;
        for (int32 Bin = 0; Bin < NumBins; ++Bin)
        {
            const int32 Lo = FMath::Max(0, Bin - FreqHalf);
            const int32 Hi = FMath::Min(NumBins - 1, Bin + FreqHalf);
            Scratch.Reset();
            for (int32 Other = Lo; Other <= Hi; ++Other)
            {
                Scratch.Add(FrameMagnitudes[Other]);
            }
            FrameOut[Bin] = MedianInPlace(Scratch);
        }
    }

    // Soft Wiener masks, power 2, summing to exactly 1 per cell so the split conserves energy and
    // a caller can read the harmonic/percussive ratios straight off these two arrays.
    OutHarmonic.SetNumUninitialized(NumCells);
    OutPercussive.SetNumUninitialized(NumCells);
    for (int32 Cell = 0; Cell < NumCells; ++Cell)
    {
        const double H = static_cast<double>(Harmonic[Cell]);
        const double P = static_cast<double>(Percussive[Cell]);
        const double HarmonicPower = H * H;
        const double PercussivePower = P * P;
        const double Denominator = HarmonicPower + PercussivePower;

        // Both medians zero means the cell is isolated in time AND in frequency, so there is no
        // evidence in either direction. Split evenly rather than assigning it to whichever side
        // the arithmetic happens to favour - an even split conserves energy and claims nothing.
        const double HarmonicMask = (Denominator > 0.0) ? (HarmonicPower / Denominator) : 0.5;

        const double Magnitude = static_cast<double>(Source[Cell]);
        OutHarmonic[Cell] = static_cast<float>(Magnitude * HarmonicMask);
        OutPercussive[Cell] = static_cast<float>(Magnitude * (1.0 - HarmonicMask));
    }

    return true;
}

// ==============================================================================================
// Partial tracking
// ==============================================================================================

bool PwTrackPartials(const FPwStftResult& In, int32 SampleRate, const FPwDecomposeSettings& Settings,
                     TArray<FPwPartialTrack>& Out, FString& OutErrorCode, FString& OutError)
{
    using namespace PwAudioDecomposeInternal;

    Out.Reset();
    OutErrorCode.Reset();
    OutError.Reset();

    if (!ValidateSpectrogram(In, TEXT("Partials"), OutErrorCode, OutError))
    {
        return false;
    }
    if (!ValidateSampleRate(In, SampleRate, TEXT("Partials"), OutErrorCode, OutError))
    {
        return false;
    }

    // Only the settings this function actually consumes are validated here; ResidualBands and
    // SimplifyToleranceDb belong to PwAnalyzeResidual and are not this function's business. The
    // caller's own arguments are named before the input is measured against them, so a caller
    // who passed a nonsensical limit is told that rather than told their audio is too long.
    if (Settings.MaxDurationMs <= 0)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Partials: maxDurationMs is %d; it must be positive."),
                Settings.MaxDurationMs));
    }
    if (Settings.MaxPartials <= 0)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Partials: maxPartials is %d; it must be at least 1."),
                Settings.MaxPartials));
    }
    if (Settings.PartialMinDurationMs < 0.0)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Partials: partialMinDurationMs is %.3f; it cannot be negative."),
                Settings.PartialMinDurationMs));
    }
    if (Settings.PartialMinAmpDb >= 0.0)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Partials: partialMinAmpDb is %.3f. It is a threshold RELATIVE to ")
                TEXT("each frame's own maximum, so it must be negative; at 0 or above only the ")
                TEXT("single loudest bin of a frame could ever qualify."),
                Settings.PartialMinAmpDb));
    }

    // Cost is REFUSED, not trimmed (see the header): cropping the input and reporting the result
    // would be a decomposition of a signal the caller never passed.
    const double DurationMs = SpectrogramDurationMs(In, SampleRate);
    if (DurationMs > static_cast<double>(Settings.MaxDurationMs))
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Partials: the spectrogram covers %.1f ms, over the %d ms ")
                TEXT("maxDurationMs limit. Analyse a shorter excerpt or raise maxDurationMs - the ")
                TEXT("input is not cropped, because a decomposition of the first %d ms reported ")
                TEXT("as a decomposition of the whole sound would be a measurement of a signal ")
                TEXT("you did not pass."),
                DurationMs, Settings.MaxDurationMs, Settings.MaxDurationMs));
    }

    const int32 NumFrames = In.NumFrames;
    const int32 NumBins = In.NumBins;
    const double BinHz = static_cast<double>(SampleRate) / static_cast<double>(In.FftSize);
    const float* Source = In.Magnitudes.GetData();

    const double FramesPerMs = static_cast<double>(SampleRate)
        / (1000.0 * static_cast<double>(In.HopSize));
    const int32 MaxGapFrames = FMath::Max(1, FMath::RoundToInt32(TrackMaxGapMs * FramesPerMs));

    // MaxPartials is clamped BEFORE the multiply, not after: the product would overflow int32 for
    // an absurd MaxPartials, and the result is capped at MaxPeaksPerFrame either way.
    const int32 FramePeakCap = FMath::Clamp(
        FMath::Min(Settings.MaxPartials, MaxPeaksPerFrame) * PeaksPerFrameMultiplier,
        MinPeaksPerFrame, MaxPeaksPerFrame);

    FTrackHarvest Harvest;
    TArray<FActiveTrack> Active;
    TArray<FActiveTrack> Survivors;
    TArray<FSpectralPeak> Peaks;
    TArray<int32> FrequencyOrder;
    TArray<FMatchCandidate> Candidates;
    TArray<bool> TrackClaimed;
    TArray<bool> PeakClaimed;

    int32 ClippedFrames = 0;

    for (int32 Frame = 0; Frame < NumFrames; ++Frame)
    {
        FindFramePeaks(Source + static_cast<int64>(Frame) * NumBins, NumBins, BinHz,
            Settings.PartialMinAmpDb, Peaks);

        if (Peaks.Num() > FramePeakCap)
        {
            // Loudest-first clip. A peak outside its own frame's loudest FramePeakCap cannot be
            // part of a top-MaxPartials track of the whole sound, and the count is reported.
            Peaks.Sort([](const FSpectralPeak& A, const FSpectralPeak& B)
            {
                if (A.AmpLinear != B.AmpLinear)
                {
                    return A.AmpLinear > B.AmpLinear;
                }
                return A.FreqHz < B.FreqHz;
            });
            Peaks.SetNum(FramePeakCap);
            ++ClippedFrames;
        }

        // The two-pointer match below needs both sides in ascending frequency.
        Peaks.Sort([](const FSpectralPeak& A, const FSpectralPeak& B) { return A.FreqHz < B.FreqHz; });

        FrequencyOrder.Reset(Active.Num());
        for (int32 Index = 0; Index < Active.Num(); ++Index)
        {
            FrequencyOrder.Add(Index);
        }
        FrequencyOrder.Sort([&Active](int32 A, int32 B)
        {
            if (Active[A].LastFreqHz != Active[B].LastFreqHz)
            {
                return Active[A].LastFreqHz < Active[B].LastFreqHz;
            }
            return A < B;
        });

        // Candidate continuations. Both lower bounds (F - 2*BinHz and 0.97*F) increase with F, so
        // one forward-only cursor into the peak array covers every track without rescanning.
        Candidates.Reset();
        int32 PeakCursor = 0;
        for (const int32 TrackIndex : FrequencyOrder)
        {
            const double TrackFreq = Active[TrackIndex].LastFreqHz;
            const double Tolerance = FMath::Max(TrackToleranceBins * BinHz,
                TrackToleranceRelative * TrackFreq);
            while (PeakCursor < Peaks.Num() && Peaks[PeakCursor].FreqHz < TrackFreq - Tolerance)
            {
                ++PeakCursor;
            }
            for (int32 PeakIndex = PeakCursor;
                 PeakIndex < Peaks.Num() && Peaks[PeakIndex].FreqHz <= TrackFreq + Tolerance;
                 ++PeakIndex)
            {
                FMatchCandidate& Candidate = Candidates.AddDefaulted_GetRef();
                Candidate.DeltaHz = FMath::Abs(Peaks[PeakIndex].FreqHz - TrackFreq);
                Candidate.TrackIndex = TrackIndex;
                Candidate.PeakIndex = PeakIndex;
            }
        }

        // Nearest-first greedy assignment: a simplification of MQ's two-pass conflict resolution
        // that agrees with it whenever the peaks are resolvable at all. The tie-breaks make the
        // result independent of the sort's pivot choices.
        Candidates.Sort([](const FMatchCandidate& A, const FMatchCandidate& B)
        {
            if (A.DeltaHz != B.DeltaHz)
            {
                return A.DeltaHz < B.DeltaHz;
            }
            if (A.TrackIndex != B.TrackIndex)
            {
                return A.TrackIndex < B.TrackIndex;
            }
            return A.PeakIndex < B.PeakIndex;
        });

        TrackClaimed.Reset();
        TrackClaimed.SetNumZeroed(Active.Num());
        PeakClaimed.Reset();
        PeakClaimed.SetNumZeroed(Peaks.Num());

        const double FrameMs = FrameCentreMs(Frame, In.HopSize, In.FftSize, SampleRate);

        for (const FMatchCandidate& Candidate : Candidates)
        {
            if (TrackClaimed[Candidate.TrackIndex] || PeakClaimed[Candidate.PeakIndex])
            {
                continue;
            }
            TrackClaimed[Candidate.TrackIndex] = true;
            PeakClaimed[Candidate.PeakIndex] = true;

            FActiveTrack& Track = Active[Candidate.TrackIndex];
            FPwPartialPoint& Point = Track.Track.Points.AddDefaulted_GetRef();
            Point.TimeMs = FrameMs;
            Point.FreqHz = Peaks[Candidate.PeakIndex].FreqHz;
            Point.AmpLinear = Peaks[Candidate.PeakIndex].AmpLinear;
            Track.LastMatchedFrame = Frame;
            Track.LastFreqHz = Point.FreqHz;
        }

        for (int32 PeakIndex = 0; PeakIndex < Peaks.Num(); ++PeakIndex)
        {
            if (PeakClaimed[PeakIndex])
            {
                continue;
            }
            FActiveTrack& Born = Active.AddDefaulted_GetRef();
            FPwPartialPoint& Point = Born.Track.Points.AddDefaulted_GetRef();
            Point.TimeMs = FrameMs;
            Point.FreqHz = Peaks[PeakIndex].FreqHz;
            Point.AmpLinear = Peaks[PeakIndex].AmpLinear;
            Born.LastMatchedFrame = Frame;
            Born.LastFreqHz = Point.FreqHz;
        }

        // Death sweep. A track unmatched for longer than the gap ends at its LAST MATCHED frame,
        // so the gap contributes no points and EndMs is a time a peak was actually measured at.
        Survivors.Reset();
        for (FActiveTrack& Track : Active)
        {
            if (Frame - Track.LastMatchedFrame > MaxGapFrames)
            {
                FinishTrack(Track, Settings, Harvest);
            }
            else
            {
                Survivors.Add(MoveTemp(Track));
            }
        }
        Swap(Active, Survivors);
    }

    for (FActiveTrack& Track : Active)
    {
        FinishTrack(Track, Settings, Harvest);
    }
    Active.Reset();

    CompactHarvest(Harvest, Settings.MaxPartials);
    SortLoudestFirst(Harvest.Retained);
    Out = MoveTemp(Harvest.Retained);

    // ---- report what was dropped (rpc-design.md §1) -----------------------------------------
    // A bare list of MaxPartials tracks reads as "this sound has MaxPartials partials". The note
    // rides on OutError with an EMPTY OutErrorCode and a true return, so a caller keying on the
    // return value sees a success and a caller reading the strings sees the counts.
    TArray<FString> Clauses;
    if (Harvest.LongEnough > Out.Num())
    {
        const double QuietestKeptDb = (Out.Num() > 0) ? AmplitudeToDb(Out.Last().PeakAmpLinear) : LevelDbFloor;
        Clauses.Add(FString::Printf(
            TEXT("kept %d of %d tracks (maxPartials=%d), the loudest dropped one peaking at ")
            TEXT("%.1f dBFS against %.1f dBFS for the quietest kept"),
            Out.Num(), Harvest.LongEnough, Settings.MaxPartials,
            AmplitudeToDb(Harvest.LoudestDroppedAmp), QuietestKeptDb));
    }
    if (Harvest.ShortDropped > 0)
    {
        Clauses.Add(FString::Printf(
            TEXT("%d tracks shorter than %.1f ms were discarded as peak-picking noise"),
            Harvest.ShortDropped, Settings.PartialMinDurationMs));
    }
    if (ClippedFrames > 0)
    {
        Clauses.Add(FString::Printf(
            TEXT("per-frame peak lists were clipped to %d peaks in %d of %d frames"),
            FramePeakCap, ClippedFrames, NumFrames));
    }
    if (Clauses.Num() > 0)
    {
        OutError = FString::Printf(TEXT("PwTrackPartials: %s."), *FString::Join(Clauses, TEXT("; ")));
        UE_LOG(LogPinWrightSubsystem, Warning, TEXT("%s"), *OutError);
    }

    return true;
}

// ==============================================================================================
// Transient characterisation
// ==============================================================================================

bool PwDetectTransients(const FPwStftResult& In, int32 SampleRate, const TArray<FPwOnset>& Onsets,
                        TArray<FPwTransient>& Out, FString& OutErrorCode, FString& OutError)
{
    using namespace PwAudioDecomposeInternal;

    Out.Reset();
    OutErrorCode.Reset();
    OutError.Reset();

    if (!ValidateSpectrogram(In, TEXT("Transients"), OutErrorCode, OutError))
    {
        return false;
    }
    if (!ValidateSampleRate(In, SampleRate, TEXT("Transients"), OutErrorCode, OutError))
    {
        return false;
    }

    if (Onsets.Num() <= 0)
    {
        // No seeds is a legal, successful call. A sustained tone genuinely has no transients, and
        // manufacturing one is the false-positive direction that costs the caller most (§6).
        return true;
    }

    const int32 NumFrames = In.NumFrames;
    const int32 NumBins = In.NumBins;
    const double BinHz = static_cast<double>(SampleRate) / static_cast<double>(In.FftSize);
    const float* Source = In.Magnitudes.GetData();

    // Broadband frame power, the curve both the peak search and the extent test run on.
    TArray<double> FramePower;
    FramePower.SetNumZeroed(NumFrames);
    for (int32 Frame = 0; Frame < NumFrames; ++Frame)
    {
        const float* FrameMagnitudes = Source + static_cast<int64>(Frame) * NumBins;
        double Power = 0.0;
        for (int32 Bin = 0; Bin < NumBins; ++Bin)
        {
            const double Magnitude = static_cast<double>(FrameMagnitudes[Bin]);
            Power += Magnitude * Magnitude;
        }
        FramePower[Frame] = Power;
    }

    // Seed frames, ascending and de-duplicated. Two onsets landing on the same frame describe one
    // burst at this resolution; keeping both would report the same transient twice.
    TArray<int32> RawSeedFrames;
    RawSeedFrames.Reserve(Onsets.Num());
    int32 OutOfRangeSeeds = 0;
    for (const FPwOnset& Onset : Onsets)
    {
        const double RawIndex = FrameIndexAtMs(Onset.TimeMs, In.HopSize, In.FftSize, SampleRate);
        const int32 Frame = FMath::RoundToInt32(RawIndex);
        if (Frame < 0 || Frame >= NumFrames)
        {
            // Skipped rather than clamped onto the first or last frame: a transient reported at
            // the end of the clip that was actually past its end is a fabricated measurement.
            ++OutOfRangeSeeds;
            continue;
        }
        RawSeedFrames.Add(Frame);
    }
    RawSeedFrames.Sort();

    TArray<int32> SeedFrames;
    SeedFrames.Reserve(RawSeedFrames.Num());
    int32 DuplicateSeeds = 0;
    for (const int32 Frame : RawSeedFrames)
    {
        if (SeedFrames.Num() > 0 && SeedFrames.Last() == Frame)
        {
            ++DuplicateSeeds;
            continue;
        }
        SeedFrames.Add(Frame);
    }

    // The flux detector reports a time that LEADS the true transient by about a quarter of a
    // window, so the peak search runs forward by a whole window and back by one frame.
    const int32 WindowFrames = FMath::Max(1,
        FMath::CeilToInt32(static_cast<double>(In.FftSize) / static_cast<double>(In.HopSize)));
    const double FramesPerMs = static_cast<double>(SampleRate)
        / (1000.0 * static_cast<double>(In.HopSize));
    const int32 MaxHalfLengthFrames = FMath::Max(1,
        FMath::RoundToInt32(TransientMaxHalfLengthMs * FramesPerMs));
    const double DecayFraction = FMath::Pow(10.0, -TransientDecayDb / 10.0);

    TArray<double> BinPower;
    BinPower.SetNumZeroed(NumBins);

    int32 SilentSeeds = 0;
    int32 OverlappedSeeds = 0;
    int32 PreviousEndFrame = INDEX_NONE;

    for (int32 SeedIndex = 0; SeedIndex < SeedFrames.Num(); ++SeedIndex)
    {
        const int32 SeedFrame = SeedFrames[SeedIndex];
        const int32 NextSeedFrame = SeedFrames.IsValidIndex(SeedIndex + 1)
            ? SeedFrames[SeedIndex + 1] : NumFrames;

        // Bounded so two transients cannot overlap and neither can swallow the next onset.
        const int32 SearchLo = FMath::Max(0, FMath::Max(SeedFrame - 1, PreviousEndFrame + 1));
        const int32 SearchHi = FMath::Min(FMath::Min(NumFrames - 1, SeedFrame + WindowFrames),
            NextSeedFrame - 1);
        if (SearchHi < SearchLo)
        {
            // The previous transient's tail already covers every frame this seed could own.
            // Counted separately from a silent seed: "swallowed by its neighbour" and "had no
            // energy" point the caller at different repairs.
            ++OverlappedSeeds;
            continue;
        }

        int32 PeakFrame = SearchLo;
        for (int32 Frame = SearchLo; Frame <= SearchHi; ++Frame)
        {
            if (FramePower[Frame] > FramePower[PeakFrame])
            {
                PeakFrame = Frame;
            }
        }
        if (!(FramePower[PeakFrame] > 0.0))
        {
            // Existence before shape (§7): a window with no energy has no centroid, no bandwidth
            // and no level, so it is skipped rather than reported as a transient at 0 Hz.
            ++SilentSeeds;
            continue;
        }

        const double DecayThreshold = FramePower[PeakFrame] * DecayFraction;

        const int32 BackstopLo = FMath::Max(0,
            FMath::Max(PeakFrame - MaxHalfLengthFrames, PreviousEndFrame + 1));
        int32 StartFrame = PeakFrame;
        while (StartFrame > BackstopLo && FramePower[StartFrame - 1] > DecayThreshold)
        {
            --StartFrame;
        }

        const int32 BackstopHi = FMath::Min(FMath::Min(NumFrames - 1, PeakFrame + MaxHalfLengthFrames),
            NextSeedFrame - 1);
        int32 EndFrame = PeakFrame;
        while (EndFrame < BackstopHi && FramePower[EndFrame + 1] > DecayThreshold)
        {
            ++EndFrame;
        }

        // Spectral shape over the whole window, power-weighted.
        FMemory::Memzero(BinPower.GetData(), BinPower.Num() * sizeof(double));
        double TotalPower = 0.0;
        double PeakMagnitude = 0.0;
        for (int32 Frame = StartFrame; Frame <= EndFrame; ++Frame)
        {
            const float* FrameMagnitudes = Source + static_cast<int64>(Frame) * NumBins;
            for (int32 Bin = 0; Bin < NumBins; ++Bin)
            {
                const double Magnitude = static_cast<double>(FrameMagnitudes[Bin]);
                BinPower[Bin] += Magnitude * Magnitude;
                PeakMagnitude = FMath::Max(PeakMagnitude, Magnitude);
            }
        }
        for (int32 Bin = 0; Bin < NumBins; ++Bin)
        {
            TotalPower += BinPower[Bin];
        }
        if (!(TotalPower > 0.0))
        {
            ++SilentSeeds;
            continue;
        }

        double Centroid = 0.0;
        for (int32 Bin = 0; Bin < NumBins; ++Bin)
        {
            Centroid += static_cast<double>(Bin) * BinHz * BinPower[Bin];
        }
        Centroid /= TotalPower;

        double Variance = 0.0;
        for (int32 Bin = 0; Bin < NumBins; ++Bin)
        {
            const double Offset = static_cast<double>(Bin) * BinHz - Centroid;
            Variance += Offset * Offset * BinPower[Bin];
        }
        Variance /= TotalPower;

        // Cumulative-power percentiles, reported at the crossing bin's centre with no
        // interpolation - the report must not claim more resolution than one bin.
        const double LowTarget = TransientLowPercentile * TotalPower;
        const double HighTarget = TransientHighPercentile * TotalPower;
        double Cumulative = 0.0;
        int32 LowBin = 0;
        int32 HighBin = NumBins - 1;
        bool bLowFound = false;
        bool bHighFound = false;
        for (int32 Bin = 0; Bin < NumBins; ++Bin)
        {
            Cumulative += BinPower[Bin];
            if (!bLowFound && Cumulative >= LowTarget)
            {
                LowBin = Bin;
                bLowFound = true;
            }
            if (!bHighFound && Cumulative >= HighTarget)
            {
                HighBin = Bin;
                bHighFound = true;
                break;
            }
        }

        FPwTransient& Transient = Out.AddDefaulted_GetRef();
        Transient.StartMs = FrameCentreMs(StartFrame, In.HopSize, In.FftSize, SampleRate);
        Transient.PeakMs = FrameCentreMs(PeakFrame, In.HopSize, In.FftSize, SampleRate);
        Transient.EndMs = FrameCentreMs(EndFrame, In.HopSize, In.FftSize, SampleRate);
        Transient.CentroidHz = Centroid;
        Transient.BandwidthHz = FMath::Sqrt(FMath::Max(0.0, Variance));
        Transient.LowHz = static_cast<double>(LowBin) * BinHz;
        Transient.HighHz = static_cast<double>(HighBin) * BinHz;
        Transient.PeakDb = AmplitudeToDb(PeakMagnitude);

        PreviousEndFrame = EndFrame;
    }

    if (OutOfRangeSeeds > 0 || DuplicateSeeds > 0 || SilentSeeds > 0 || OverlappedSeeds > 0)
    {
        UE_LOG(LogPinWrightSubsystem, Warning,
            TEXT("PwDetectTransients: characterised %d of %d onsets - %d fell outside the ")
            TEXT("spectrogram, %d landed on a frame another onset already claimed, %d had no ")
            TEXT("energy to measure, %d were covered by the preceding transient."),
            Out.Num(), Onsets.Num(), OutOfRangeSeeds, DuplicateSeeds, SilentSeeds, OverlappedSeeds);
    }

    return true;
}
