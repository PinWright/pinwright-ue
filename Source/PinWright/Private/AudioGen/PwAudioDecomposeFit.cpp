// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwAudioDecomposeFit - the back half of the decomposition analyzer.
//
// PwFitModes turns tracked partials into `modal` generator parameters; PwAnalyzeResidual
// describes everything those partials do NOT account for as a small set of banded dB envelopes,
// which is what a filtered-noise layer is authored from. Both outputs exist to be fed straight
// back into PwSynthDsp - a fit the synthesizer cannot consume is decoration, so every convention
// below is the generator's convention, not a re-derivation of it.
//
// THE T60 CONVENTION IS PwGenModal'S, EXACTLY
// -------------------------------------------
// PwGenModal.cpp builds each resonator's pole radius as
//
//     r = exp(-ln(1000) / (T60Seconds * SampleRate))
//
// i.e. `modeDecaysMs` is the time for the mode's AMPLITUDE to fall by a factor of 1000, which is
// -60 dB. So a track whose amplitude envelope is A(t) = A0 * exp(Slope * t) with t in ms has
//
//     DecayMs = -ln(1000) / Slope
//
// and Ln1000 below is the same literal PwGenModalInternal::Ln1000 carries. Getting this wrong is
// not a small error: reporting the e-folding time tau instead of the T60 would publish a decay
// 6.9x too short, and every fitted bank would ring for a seventh of the reference's tail.
// PwFitModesRoundTripsPwGenModalT60Convention in TestPwDecomposeFit.cpp pins the relationship by
// synthesising a track with the generator's own r and asserting the fit returns the T60 that r
// was built from.
//
// FITTING: WEIGHTED LEAST SQUARES IN LOG AMPLITUDE
// ------------------------------------------------
// ln A is linear in t for an exponential decay, so the fit is one linear regression rather than
// an iterative solve. Three decisions make the difference between a usable decay and a wrong one:
//
//   1. WEIGHT BY AMPLITUDE SQUARED. Taking the log is not variance-preserving: for additive
//      noise of roughly constant magnitude sigma on A (which is what a spectrogram bin carries -
//      broadband noise adds to the bin, it does not scale with the partial), Var(ln A) ~
//      (sigma/A)^2, so the inverse-variance weight is exactly w = A^2. Unweighted OLS in log
//      amplitude hands every point equal say, and the quietest points have the worst log-domain
//      SNR, so the tail dominates the regression and the reported decay comes out long and noisy.
//      The cost of A^2 is that the fit is effectively driven by the top ~15-20 dB of the decay:
//      for a decay that is not a true exponential, this reports the early slope - which is the
//      part a listener localises the material from - and FitErrorDb reports how badly the rest
//      of the data disagrees with it.
//
//   2. EXCLUDE POINTS MORE THAN FitFloorDb BELOW THE WINDOW'S PEAK. A real decay does not run to
//      zero, it runs into the analysis noise floor and then flattens. Points inside that flat
//      region pull the slope toward zero, which biases tau LONG - the failure that makes every
//      material sound like metal. FitFloorDb = 40 dB is two thirds of the T60 span, so the
//      extrapolation to -60 dB is a 1.5x reach, and it sits well above the leakage floor a Hann
//      analysis window leaves around a partial.
//
//   3. FIT FROM THE AMPLITUDE PEAK ONWARD. The frames before the peak are the excitation ramp,
//      not the decay, and they carry the LARGEST A^2 weights - so leaving them in is the one
//      mistake weighting makes worse rather than better.
//
// WHAT bMeasured MEANS, AND THE INVARIANT THAT ENFORCES IT
// --------------------------------------------------------
// bMeasured is true only when the row is a usable modal mode: a decay was measured AND its
// values are ones PwGenModal will accept. The structural guarantee (rpc-design.md §2, §1) is
//
//     bMeasured == true   <=>   DecayMs > 0
//
// so a caller that forgets the flag still cannot feed an unmeasured row into the generator -
// PwGenModal rejects modeDecaysMs <= 0 outright. Sign is preserved rather than discarded
// (rpc-design.md §6): a partial that GREW reports a negative DecayMs (the time it would take to
// gain 60 dB), a partial that held level reports 0 (no rate resolvable), and the two therefore
// do not score alike. FreqHz, InitialGainDb and FitErrorDb stay real measurements in every case
// where enough points existed to measure them - the level of a sustained partial is a fact even
// though its decay is not.
//
// PwAudioDecompose.h states the weaker contract that on an unmeasured row "the three numbers are
// meaningless, not zero". This implementation is a strict superset of that: a caller holding to
// the header may read nothing but bMeasured, while the values actually written are the honest
// ones described above. Do not narrow this to zeros - the sign in DecayMs is the only thing
// separating a partial that grew from one that held level.

#include "AudioGen/PwAudioDecompose.h"

#include "AudioGen/PwStft.h"
#include "Handlers/ErrorCodes.h"
#include "Math/UnrealMathUtility.h"

// Named (not anonymous) namespace: this module builds with bUseUnity = true, and anonymous
// namespaces in merged TUs are the ODR-collision source the plugin's Build.cs comment warns about.
namespace PwAudioDecomposeFitInternal
{
    // ---- shared ------------------------------------------------------------------------------

    /**
     * Amplitude at or below which a value is silence rather than signal. Identical to PwStft's
     * and PwAudioFeatures' SilencePeak on purpose: a partial that PwComputeStft would have called
     * silent must not be fitted as signal here.
     */
    constexpr double SilenceAmp = 1e-9;

    /** 20 / ln(10). Converts a natural-log amplitude difference straight into decibels. */
    constexpr double DbPerLogE = 8.685889638065035;

    /**
     * ln(1000) - the amplitude ratio a -60 dB decay spans. Byte-identical to
     * PwGenModalInternal::Ln1000 so a fit fed straight back into the generator reproduces the
     * decay it was measured from rather than a scaled version of it.
     */
    constexpr double Ln1000 = 6.907755278982137;

    bool Fail(FString& OutErrorCode, FString& OutError, const TCHAR* Code, FString&& Message)
    {
        OutErrorCode = Code;
        OutError = MoveTemp(Message);
        return false;
    }

    /**
     * Existence + finiteness + ordering sweep over a partial set, in that order.
     *
     * Non-finite is checked BEFORE any amplitude comparison, and that ordering is load-bearing
     * rather than tidy: NaN compares false against every threshold, so FMath::Max(0.0, NaN)
     * returns 0 and a track full of NaN measures as digital silence unless it is named first
     * (rpc-design.md §7, and the same reasoning ERR_AUDIO_NON_FINITE_SAMPLES was registered for).
     *
     * Monotonic time is enforced rather than assumed (rpc-design.md §2): an out-of-order track
     * would still produce a slope and a residual, just meaningless ones, with no symptom.
     *
     * @return false with OutErrorCode/OutError populated. Populates nothing on success.
     */
    bool ValidatePartials(const TArray<FPwPartialTrack>& Partials, const TCHAR* Verb,
                          FString& OutErrorCode, FString& OutError)
    {
        for (int32 TrackIndex = 0; TrackIndex < Partials.Num(); ++TrackIndex)
        {
            const TArray<FPwPartialPoint>& Points = Partials[TrackIndex].Points;
            for (int32 PointIndex = 0; PointIndex < Points.Num(); ++PointIndex)
            {
                const FPwPartialPoint& Point = Points[PointIndex];
                if (!FMath::IsFinite(Point.TimeMs) || !FMath::IsFinite(Point.FreqHz)
                    || !FMath::IsFinite(Point.AmpLinear))
                {
                    return Fail(OutErrorCode, OutError, ErrorCodes::ERR_AUDIO_NON_FINITE_SAMPLES,
                        FString::Printf(TEXT("%s: partial %d point %d is non-finite ")
                            TEXT("(timeMs=%g freqHz=%g ampLinear=%g). One NaN poisons every log, ")
                            TEXT("every weighted sum and every threshold downstream, and NaN ")
                            TEXT("compares false against all of them, so it is named here rather ")
                            TEXT("than measured as silence."),
                            Verb, TrackIndex, PointIndex,
                            Point.TimeMs, Point.FreqHz, Point.AmpLinear));
                }
                if (PointIndex > 0 && Point.TimeMs < Points[PointIndex - 1].TimeMs)
                {
                    return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
                        FString::Printf(TEXT("%s: partial %d is not ordered in time - point %d is ")
                            TEXT("at %g ms, before point %d at %g ms. Both the decay fit and the ")
                            TEXT("residual sampler read a track as a time series."),
                            Verb, TrackIndex, PointIndex, Point.TimeMs,
                            PointIndex - 1, Points[PointIndex - 1].TimeMs));
                }
            }
        }
        return true;
    }

    // ---- modal fitting -----------------------------------------------------------------------

    /**
     * Points more than this far below the fit window's peak are dropped before the regression.
     * See the file header for why 40 dB rather than "all of it".
     */
    constexpr double FitFloorDb = 40.0;

    /** FitFloorDb as an amplitude ratio: 10^(-40/20). */
    constexpr double FitFloorRatio = 0.01;

    /**
     * Fewest above-floor points a regression is allowed to run on. Two points define a line
     * exactly and therefore report a zero residual for any data at all, which would make
     * FitErrorDb a constant rather than a measurement; four is the smallest count that leaves the
     * residual two degrees of freedom to be nonzero in.
     */
    constexpr int32 MinFitPoints = 4;

    /**
     * The fitted level change across the fit window must reach this many dB before the trend is
     * called a decay (or, negated, a growth). Below 3 dB - a factor of two in power - the slope
     * is not separable from ordinary level drift across the window, and calling it a decay would
     * publish an arbitrarily long T60 derived from numerical ripple.
     */
    constexpr double MinTrendDb = 3.0;

    /** One weighted log-amplitude regression over a chosen slice of a track. */
    struct FLogLineFit
    {
        /** d(ln A)/dt, per millisecond. Negative for a decay. */
        double SlopePerMs = 0.0;

        /** Fitted 20*log10(A) at FirstTimeMs - the level where the decay starts. */
        double LevelAtStartDb = 0.0;

        /** Fitted level change across the window, positive when the partial decayed. */
        double TrendDb = 0.0;

        /** Weighted RMS of the regression residual, in dB. A real measurement in both branches. */
        double RmsResidualDb = 0.0;

        /** A^2-weighted mean frequency over the fitted points: the frequency where the SNR is. */
        double WeightedFreqHz = 0.0;

        double FirstTimeMs = 0.0;
        double LastTimeMs = 0.0;
        int32 NumPoints = 0;
        bool bValid = false;
    };

    /**
     * Weighted least squares of ln(AmpLinear) against TimeMs over Points[FirstIndex..LastIndex],
     * keeping only points above FloorAmp, with weight w = A^2 (see the file header).
     *
     * Times are centred on their weighted mean before the normal equations are formed. That is
     * conditioning, not style: a track starting at 8000 ms would otherwise build Sxx ~ 6.4e7 * S
     * and lose the slope's low bits to cancellation against Sx^2.
     */
    FLogLineFit FitLogLine(const TArray<FPwPartialPoint>& Points, int32 FirstIndex, int32 LastIndex,
                           double FloorAmp)
    {
        FLogLineFit Fit;

        // Pass 1: weighted means. Also the point count, which gates everything below.
        double SumW = 0.0;
        double SumWt = 0.0;
        double SumWy = 0.0;
        double SumWf = 0.0;
        int32 Count = 0;
        double FirstTimeMs = 0.0;
        double LastTimeMs = 0.0;
        for (int32 Index = FirstIndex; Index <= LastIndex; ++Index)
        {
            const FPwPartialPoint& Point = Points[Index];
            if (!(Point.AmpLinear > FloorAmp))
            {
                continue;
            }
            const double Weight = Point.AmpLinear * Point.AmpLinear;
            SumW += Weight;
            SumWt += Weight * Point.TimeMs;
            SumWy += Weight * FMath::Loge(Point.AmpLinear);
            SumWf += Weight * Point.FreqHz;
            if (Count == 0)
            {
                FirstTimeMs = Point.TimeMs;
            }
            LastTimeMs = Point.TimeMs;
            ++Count;
        }

        if (Count < MinFitPoints || !(SumW > 0.0))
        {
            return Fit;
        }

        const double MeanT = SumWt / SumW;
        const double MeanY = SumWy / SumW;
        Fit.WeightedFreqHz = SumWf / SumW;

        // Pass 2: centred second moments.
        double Stt = 0.0;
        double Sty = 0.0;
        for (int32 Index = FirstIndex; Index <= LastIndex; ++Index)
        {
            const FPwPartialPoint& Point = Points[Index];
            if (!(Point.AmpLinear > FloorAmp))
            {
                continue;
            }
            const double Weight = Point.AmpLinear * Point.AmpLinear;
            const double Dt = Point.TimeMs - MeanT;
            const double Dy = FMath::Loge(Point.AmpLinear) - MeanY;
            Stt += Weight * Dt * Dt;
            Sty += Weight * Dt * Dy;
        }

        if (!(Stt > 0.0))
        {
            // Every surviving point sits at the same instant; there is no time axis to fit along.
            return Fit;
        }

        const double Slope = Sty / Stt;
        if (!FMath::IsFinite(Slope))
        {
            return Fit;
        }

        // Pass 3: the residual, weighted by the same w the fit minimised, so FitErrorDb measures
        // the error where the energy is rather than where the noise is.
        double SumWr2 = 0.0;
        for (int32 Index = FirstIndex; Index <= LastIndex; ++Index)
        {
            const FPwPartialPoint& Point = Points[Index];
            if (!(Point.AmpLinear > FloorAmp))
            {
                continue;
            }
            const double Weight = Point.AmpLinear * Point.AmpLinear;
            const double Predicted = MeanY + Slope * (Point.TimeMs - MeanT);
            const double Residual = FMath::Loge(Point.AmpLinear) - Predicted;
            SumWr2 += Weight * Residual * Residual;
        }

        Fit.SlopePerMs = Slope;
        Fit.FirstTimeMs = FirstTimeMs;
        Fit.LastTimeMs = LastTimeMs;
        Fit.NumPoints = Count;
        Fit.LevelAtStartDb = DbPerLogE * (MeanY + Slope * (FirstTimeMs - MeanT));
        Fit.TrendDb = -DbPerLogE * Slope * (LastTimeMs - FirstTimeMs);
        // No FMath::Max guard on the radicand: SumWr2 is a sum of positive-weighted squares and
        // SumW is already known positive, so the quotient cannot be negative - and a Max(0.0, x)
        // here would be exactly the pattern this file's error ordering exists to avoid.
        Fit.RmsResidualDb = DbPerLogE * FMath::Sqrt(SumWr2 / SumW);
        Fit.bValid = true;
        return Fit;
    }

    /** Index of the first strictly-largest amplitude. Strict `>` so a flat track peaks at 0. */
    int32 FindPeakIndex(const TArray<FPwPartialPoint>& Points)
    {
        int32 PeakIndex = 0;
        double PeakAmp = Points.Num() > 0 ? Points[0].AmpLinear : 0.0;
        for (int32 Index = 1; Index < Points.Num(); ++Index)
        {
            if (Points[Index].AmpLinear > PeakAmp)
            {
                PeakAmp = Points[Index].AmpLinear;
                PeakIndex = Index;
            }
        }
        return PeakIndex;
    }

    /** Largest amplitude over an inclusive index range. Range is assumed non-empty. */
    double PeakAmpOverRange(const TArray<FPwPartialPoint>& Points, int32 FirstIndex, int32 LastIndex)
    {
        double Peak = 0.0;
        for (int32 Index = FirstIndex; Index <= LastIndex; ++Index)
        {
            Peak = FMath::Max(Peak, Points[Index].AmpLinear);
        }
        return Peak;
    }

    // ---- residual --------------------------------------------------------------------------

    /**
     * Half-width, in bins, of the region removed around a tracked partial.
     *
     * A Hann window's main lobe is 4 bins wide - the partial's own bin plus two on each side -
     * and its first sidelobes sit just beyond that, so +/-3 covers the lobe with one guard bin.
     * The two ways this is wrong on other windows are worth knowing because FPwStftResult does
     * not carry the window type: a rectangular window (1-bin half-lobe) has this over-remove two
     * bins per side, and a Blackman-Harris (3-bin half-lobe) leaks a little past it.
     */
    constexpr int32 PartialMaskHalfBins = 3;

    /** Unmasked bins sampled on each side of a masked run to estimate the noise floor under it. */
    constexpr int32 MaskFillProbeBins = 4;

    /** Bands below this are not audio; the lowest band edge never goes under it. */
    constexpr double MinBandHz = 20.0;

    /** Hard ceiling on ResidualBands. 256 log bands across 20 Hz..Nyquist is under 1/24 octave. */
    constexpr int32 MaxResidualBands = 256;

    /**
     * Floor for a band's dB envelope. -120 dBFS is below anything a 16-bit source can carry and
     * below the numerical floor of the subtraction, so a band that came out empty plots at the
     * bottom instead of at -inf (which no JSON encoder can carry).
     */
    constexpr float ResidualFloorDb = -120.f;

    /**
     * Width of the median smoother applied to each band envelope before simplification, as a
     * duration so it behaves the same at any hop.
     *
     * This is not cosmetic. A stationary noise band's per-frame level fluctuates by several dB
     * from frame to frame purely as estimator variance - one realisation of the noise, not a
     * property of the layer being described - and a 1.5 dB simplifier faithfully preserves every
     * one of those wiggles, which is how a "simplified" envelope comes back with one point per
     * frame. A median (not a mean) is used because it removes that variance without rounding off
     * the transients, which are the one thing in the envelope the synth layer must reproduce.
     */
    constexpr double EnvelopeMedianMs = 20.0;

    /** Bounds on the median width in frames, so neither a tiny nor a huge hop degenerates it. */
    constexpr int32 MinMedianFrames = 3;
    constexpr int32 MaxMedianFrames = 15;

    /**
     * Value of a partial track at an arbitrary time, by linear interpolation between its points.
     * Never extrapolates: outside [first point, last point] the track is simply not sounding.
     *
     * InOutCursor is carried across calls and only ever advances, so sweeping frames in time
     * order costs one pass over each track's points in total rather than one search per frame.
     */
    bool SamplePartialAt(const TArray<FPwPartialPoint>& Points, double TimeMs, int32& InOutCursor,
                         double& OutFreqHz, double& OutAmpLinear)
    {
        const int32 Num = Points.Num();
        if (Num == 0 || TimeMs < Points[0].TimeMs || TimeMs > Points[Num - 1].TimeMs)
        {
            return false;
        }
        if (Num == 1)
        {
            OutFreqHz = Points[0].FreqHz;
            OutAmpLinear = Points[0].AmpLinear;
            return true;
        }

        InOutCursor = FMath::Clamp(InOutCursor, 0, Num - 2);
        while (InOutCursor < Num - 2 && Points[InOutCursor + 1].TimeMs < TimeMs)
        {
            ++InOutCursor;
        }

        const FPwPartialPoint& A = Points[InOutCursor];
        const FPwPartialPoint& B = Points[InOutCursor + 1];
        const double Span = B.TimeMs - A.TimeMs;
        const double Alpha = Span > 0.0 ? FMath::Clamp((TimeMs - A.TimeMs) / Span, 0.0, 1.0) : 0.0;
        OutFreqHz = A.FreqHz + (B.FreqHz - A.FreqHz) * Alpha;
        OutAmpLinear = A.AmpLinear + (B.AmpLinear - A.AmpLinear) * Alpha;
        return true;
    }

    /**
     * Median of up to MaskFillProbeBins unmasked bins walking outward from StartBin in direction
     * Step. Returns a negative value when the walk finds none (the mask reaches the array edge).
     */
    double MedianOfUnmaskedNeighbours(const float* FrameMagnitudes, const uint8* Mask,
                                      int32 NumBins, int32 StartBin, int32 Step)
    {
        TArray<float, TInlineAllocator<MaskFillProbeBins>> Probe;
        for (int32 Bin = StartBin; Bin >= 0 && Bin < NumBins && Probe.Num() < MaskFillProbeBins;
             Bin += Step)
        {
            if (Mask[Bin] == 0)
            {
                Probe.Add(FrameMagnitudes[Bin]);
            }
        }
        if (Probe.Num() == 0)
        {
            return -1.0;
        }
        Probe.Sort();
        return static_cast<double>(Probe[Probe.Num() / 2]);
    }

    /** In-place median filter of Width (odd) over a dB series. No-op for Width < 3. */
    void MedianSmooth(TArray<double>& Series, int32 Width)
    {
        const int32 Num = Series.Num();
        if (Width < 3 || Num < 3)
        {
            return;
        }
        const int32 Half = Width / 2;
        TArray<double> Source = Series;
        TArray<double, TInlineAllocator<MaxMedianFrames>> Window;
        for (int32 Index = 0; Index < Num; ++Index)
        {
            Window.Reset();
            const int32 First = FMath::Max(0, Index - Half);
            const int32 Last = FMath::Min(Num - 1, Index + Half);
            for (int32 Probe = First; Probe <= Last; ++Probe)
            {
                Window.Add(Source[Probe]);
            }
            Window.Sort();
            Series[Index] = Window[Window.Num() / 2];
        }
    }

    /**
     * Douglas-Peucker in the dB domain, measuring VERTICAL deviation from the chord rather than
     * perpendicular distance.
     *
     * Perpendicular distance is the textbook formulation and it is wrong here: the axes are
     * milliseconds and decibels, so a perpendicular "distance" mixes units and its value depends
     * on the arbitrary scale factor between them - the same envelope simplified at a different
     * hop would keep a different set of points. Vertical deviation is in dB, which is exactly the
     * unit SimplifyToleranceDb is stated in, and it gives the guarantee the caller can rely on:
     * every dropped point is within ToleranceDb of the retained polyline evaluated at its time.
     *
     * Iterative rather than recursive - a per-frame envelope over a long clip is thousands of
     * points deep in the worst case, and that worst case is a noise band, which is the common one.
     */
    void SimplifyEnvelopeDb(const TArray<FPwDbPoint>& In, double ToleranceDb, TArray<FPwDbPoint>& Out)
    {
        Out.Reset();
        const int32 Num = In.Num();
        if (Num == 0)
        {
            return;
        }
        if (Num <= 2 || !(ToleranceDb > 0.0))
        {
            // A non-positive tolerance means "do not simplify" rather than "drop everything":
            // zero tolerance asking for zero points would be the §7 failure of reading an
            // absence as a small number.
            Out = In;
            return;
        }

        TArray<bool> Keep;
        Keep.Init(false, Num);
        Keep[0] = true;
        Keep[Num - 1] = true;

        TArray<TPair<int32, int32>> Stack;
        Stack.Emplace(0, Num - 1);
        while (Stack.Num() > 0)
        {
            const TPair<int32, int32> Segment = Stack.Pop();
            const int32 First = Segment.Key;
            const int32 Last = Segment.Value;
            if (Last <= First + 1)
            {
                continue;
            }

            const double T0 = In[First].TimeMs;
            const double V0 = In[First].ValueDb;
            const double T1 = In[Last].TimeMs;
            const double V1 = In[Last].ValueDb;
            const double Span = T1 - T0;

            double WorstDeviation = 0.0;
            int32 WorstIndex = -1;
            for (int32 Index = First + 1; Index < Last; ++Index)
            {
                const double Chord = Span > 0.0
                    ? V0 + (V1 - V0) * ((In[Index].TimeMs - T0) / Span)
                    : V0;
                const double Deviation = FMath::Abs(In[Index].ValueDb - Chord);
                if (Deviation > WorstDeviation)
                {
                    WorstDeviation = Deviation;
                    WorstIndex = Index;
                }
            }

            if (WorstIndex >= 0 && WorstDeviation > ToleranceDb)
            {
                Keep[WorstIndex] = true;
                Stack.Emplace(First, WorstIndex);
                Stack.Emplace(WorstIndex, Last);
            }
        }

        Out.Reserve(16);
        for (int32 Index = 0; Index < Num; ++Index)
        {
            if (Keep[Index])
            {
                Out.Add(In[Index]);
            }
        }
    }
}

bool PwFitModes(const TArray<FPwPartialTrack>& Partials, int32 SampleRate,
                TArray<FPwModalFit>& Out, FString& OutErrorCode, FString& OutError)
{
    using namespace PwAudioDecomposeFitInternal;

    Out.Reset();
    OutErrorCode.Reset();
    OutError.Reset();

    // ---- existence checks FIRST (rpc-design.md §7) -------------------------------------------
    // Zero is not a small number: "there were no partials" and "the partials were quiet" and "the
    // partials were NaN" are three different situations with three different remedies, and the
    // order below is the only one in which each can be named as itself.
    if (Partials.Num() == 0)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
            TEXT("FitModes: no partials were supplied. Track partials first - an empty set is not ")
            TEXT("a bank of zero modes, it is an analysis that has not run."));
    }

    int32 TotalPoints = 0;
    for (const FPwPartialTrack& Track : Partials)
    {
        TotalPoints += Track.Points.Num();
    }
    if (TotalPoints == 0)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
            FString::Printf(TEXT("FitModes: %d partial tracks were supplied and every one of them ")
                TEXT("is empty, so there is nothing to fit."), Partials.Num()));
    }

    // Non-finite before any amplitude test. See ValidatePartials.
    if (!ValidatePartials(Partials, TEXT("FitModes"), OutErrorCode, OutError))
    {
        return false;
    }

    if (SampleRate <= 0)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("FitModes: sample rate is %d; it must be positive. The rate ")
                TEXT("bounds which fitted frequencies the modal generator can accept."), SampleRate));
    }

    double GlobalPeakAmp = 0.0;
    for (const FPwPartialTrack& Track : Partials)
    {
        for (const FPwPartialPoint& Point : Track.Points)
        {
            GlobalPeakAmp = FMath::Max(GlobalPeakAmp, Point.AmpLinear);
        }
    }
    if (!(GlobalPeakAmp > SilenceAmp))
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
            FString::Printf(TEXT("FitModes: every partial is digitally silent - the largest ")
                TEXT("amplitude across %d tracks is %.3g, below the %.3g silence floor."),
                Partials.Num(), GlobalPeakAmp, SilenceAmp));
    }

    // ---- one row per input track, index-aligned ----------------------------------------------
    // Out[i] describes Partials[i]. Rows are kept rather than dropped so a caller can correlate a
    // rejection with the track that produced it; bMeasured is what says whether the row is a mode
    // (the FGroundContactReport::bMeasured pattern rpc-design.md §4 asks for).
    const double Nyquist = 0.5 * static_cast<double>(SampleRate);
    Out.Reserve(Partials.Num());
    for (const FPwPartialTrack& Track : Partials)
    {
        FPwModalFit& Fit = Out.AddDefaulted_GetRef();
        Fit.bMeasured = false;

        const TArray<FPwPartialPoint>& Points = Track.Points;
        if (Points.Num() == 0)
        {
            // Nothing at all was measured for this track. The tracker's own mean frequency is
            // still a fact, so it is carried; everything else stays at its zero, which cannot be
            // mistaken for a mode because DecayMs <= 0 is what the generator rejects.
            Fit.FreqHz = FMath::IsFinite(Track.MeanFreqHz) && Track.MeanFreqHz > 0.0
                ? Track.MeanFreqHz : 0.0;
            continue;
        }

        // The decay starts at the amplitude peak; everything before it is the excitation ramp,
        // and it carries the largest A^2 weights, so leaving it in would flatten the slope.
        const int32 PeakIndex = FindPeakIndex(Points);
        const int32 LastIndex = Points.Num() - 1;

        double FloorAmp = FMath::Max(SilenceAmp, PeakAmpOverRange(Points, PeakIndex, LastIndex) * FitFloorRatio);
        FLogLineFit Line = FitLogLine(Points, PeakIndex, LastIndex, FloorAmp);
        bool bWindowIsDecayOnly = true;

        if (!Line.bValid)
        {
            // Too little of the track sits after its peak to fit a decay - which is exactly what
            // a growing partial looks like. Regress the WHOLE track instead. That window contains
            // the excitation ramp, so its slope is never reported as a measured decay; it exists
            // only to keep the sign, so a partial that grew and a partial that held level do not
            // score alike (rpc-design.md §6).
            FloorAmp = FMath::Max(SilenceAmp, PeakAmpOverRange(Points, 0, LastIndex) * FitFloorRatio);
            Line = FitLogLine(Points, 0, LastIndex, FloorAmp);
            bWindowIsDecayOnly = false;
        }

        if (!Line.bValid)
        {
            Fit.FreqHz = FMath::IsFinite(Track.MeanFreqHz) && Track.MeanFreqHz > 0.0
                ? Track.MeanFreqHz : 0.0;
            continue;
        }

        // Frequency, level and fit error are measurements in every branch below - only the decay
        // is conditional. The frequency is A^2-weighted for the same reason the slope is: the
        // quiet tail of a track is where the tracker's frequency estimate is worst.
        Fit.FreqHz = Line.WeightedFreqHz;
        Fit.InitialGainDb = Line.LevelAtStartDb;
        Fit.FitErrorDb = Line.RmsResidualDb;

        if (Line.TrendDb <= -MinTrendDb)
        {
            // Grew. The magnitude is the time it would take to gain 60 dB at this rate; the sign
            // is what says it is not a mode, and a negative decay is structurally unconsumable
            // (PwGenModal rejects modeDecaysMs <= 0), so it cannot leak into a recipe.
            const double SignedT60Ms = -Ln1000 / Line.SlopePerMs;
            Fit.DecayMs = FMath::IsFinite(SignedT60Ms) ? SignedT60Ms : 0.0;
            continue;
        }
        if (Line.TrendDb < MinTrendDb)
        {
            // Sustained: the level held within MinTrendDb across the window. No rate is
            // resolvable, so no rate is published - 0, not a very long one.
            Fit.DecayMs = 0.0;
            continue;
        }
        if (!bWindowIsDecayOnly)
        {
            // The only window that fitted spans the excitation too, so the slope is a blend of
            // attack and decay. Report the sign (it decayed overall) but not a decay time that
            // the generator would reproduce as something else.
            Fit.DecayMs = 0.0;
            continue;
        }

        const double DecayMs = -Ln1000 / Line.SlopePerMs;
        if (!FMath::IsFinite(DecayMs) || DecayMs <= 0.0)
        {
            Fit.DecayMs = 0.0;
            continue;
        }

        // The generator rejects a mode at or above Nyquist (its cos(theta) folds onto a mirrored
        // frequency) and one at or below 0 Hz. Publishing a decay for such a partial would be a
        // number no caller can consume, so the row reports its frequency and level and stops.
        if (!(Fit.FreqHz > 0.0) || Fit.FreqHz >= Nyquist)
        {
            Fit.DecayMs = 0.0;
            continue;
        }

        Fit.DecayMs = DecayMs;
        Fit.bMeasured = true;
    }

    return true;
}

bool PwAnalyzeResidual(const FPwStftResult& Original, const TArray<FPwPartialTrack>& Partials,
                       int32 SampleRate, const FPwDecomposeSettings& Settings,
                       TArray<FPwResidualBand>& Out, FString& OutErrorCode, FString& OutError)
{
    using namespace PwAudioDecomposeFitInternal;

    Out.Reset();
    OutErrorCode.Reset();
    OutError.Reset();

    // ---- existence checks FIRST (rpc-design.md §7) -------------------------------------------
    if (Original.NumFrames <= 0 || Original.NumBins <= 0 || Original.Magnitudes.Num() == 0)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
            FString::Printf(TEXT("Residual: the spectrogram is empty (%d frames x %d bins, %d ")
                TEXT("magnitudes). Compute the STFT before decomposing it."),
                Original.NumFrames, Original.NumBins, Original.Magnitudes.Num()));
    }

    // Non-finite before the silence test, in both inputs. FMath::Max(0.0, NaN) is 0, so a NaN
    // spectrogram measures as digital silence unless it is named first.
    for (int32 Index = 0; Index < Original.Magnitudes.Num(); ++Index)
    {
        if (!FMath::IsFinite(Original.Magnitudes[Index]))
        {
            return Fail(OutErrorCode, OutError, ErrorCodes::ERR_AUDIO_NON_FINITE_SAMPLES,
                FString::Printf(TEXT("Residual: spectrogram magnitude %d (frame %d, bin %d) is ")
                    TEXT("%g. A non-finite bin poisons the band sums and compares false against ")
                    TEXT("every threshold, so it is named rather than measured as silence."),
                    Index, Index / Original.NumBins, Index % Original.NumBins,
                    Original.Magnitudes[Index]));
        }
    }

    // An empty partial set is NOT an error here, unlike in PwFitModes: a sound with no tracked
    // partials is legitimately all residual, and reporting its bands is the correct answer.
    if (!ValidatePartials(Partials, TEXT("Residual"), OutErrorCode, OutError))
    {
        return false;
    }

    float PeakMagnitude = 0.f;
    for (const float Magnitude : Original.Magnitudes)
    {
        PeakMagnitude = FMath::Max(PeakMagnitude, Magnitude);
    }
    if (!(static_cast<double>(PeakMagnitude) > SilenceAmp))
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
            FString::Printf(TEXT("Residual: the spectrogram is digitally silent - its largest ")
                TEXT("magnitude is %.3g, below the %.3g silence floor."),
                PeakMagnitude, SilenceAmp));
    }

    // ---- structural checks -------------------------------------------------------------------
    if (!Original.IsValid())
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Residual: malformed spectrogram - %d frames x %d bins needs %d ")
                TEXT("magnitudes but carries %d; fftSize=%d hopSize=%d."),
                Original.NumFrames, Original.NumBins, Original.NumFrames * Original.NumBins,
                Original.Magnitudes.Num(), Original.FftSize, Original.HopSize));
    }
    if (SampleRate <= 0)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Residual: sample rate is %d; it must be positive. Frame times ")
                TEXT("and band edges are both derived from it."), SampleRate));
    }

    // Structural guarantee rather than a convention the caller has to remember (rpc-design.md §2):
    // the spectrogram already knows the rate it was computed at, as BinHz * FftSize. A mismatched
    // pair would still produce bands - just on the wrong frequency axis, with no symptom.
    if (Original.BinHz > 0.f)
    {
        const double ImpliedRate = static_cast<double>(Original.BinHz) * static_cast<double>(Original.FftSize);
        if (FMath::Abs(ImpliedRate - static_cast<double>(SampleRate)) > 0.01 * static_cast<double>(SampleRate))
        {
            return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("Residual: sampleRate %d disagrees with the spectrogram, ")
                    TEXT("whose binHz %.6g over fftSize %d implies %.1f Hz. Pass the rate the ")
                    TEXT("spectrogram was computed at."),
                    SampleRate, Original.BinHz, Original.FftSize, ImpliedRate));
        }
    }
    if (!(Original.BinHz > 0.f))
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Residual: the spectrogram reports binHz %g; without a bin width ")
                TEXT("there is no frequency axis to lay bands on."), Original.BinHz));
    }
    if (Settings.ResidualBands < 1 || Settings.ResidualBands > MaxResidualBands)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Residual: residualBands is %d; it must be between 1 and %d. ")
                TEXT("Zero bands is not an empty description, it is no description."),
                Settings.ResidualBands, MaxResidualBands));
    }

    const double Nyquist = 0.5 * static_cast<double>(SampleRate);
    const double BinHz = static_cast<double>(Original.BinHz);
    const double LowEdgeHz = FMath::Max(MinBandHz, BinHz);
    if (!(Nyquist > LowEdgeHz))
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Residual: the band range is degenerate - Nyquist is %.1f Hz but ")
                TEXT("the lowest usable band edge is %.1f Hz (the larger of %.1f Hz and one bin ")
                TEXT("width). Analyse at a higher sample rate or a larger fftSize."),
                Nyquist, LowEdgeHz, MinBandHz));
    }

    // ---- spectral subtraction ------------------------------------------------------------------
    //
    // WHY SPECTRAL AND NOT TIME DOMAIN. Time-domain subtraction is the tempting option because it
    // yields an audible residual waveform, but it is not merely more expensive here - it is wrong.
    // FPwPartialPoint carries frequency and amplitude and NO PHASE, so a resynthesised partial has
    // an arbitrary phase relative to the original. Subtracting two sinusoids of equal amplitude and
    // random relative phase produces, on average, sqrt(2) times the amplitude of either: the
    // "subtraction" would ADD up to 3 dB of the very partial it was removing. Tracking phase well
    // enough to cancel would mean a phase-locked analysis the tracker does not do.
    //
    // So each partial's main lobe is REMOVED from the magnitude spectrogram and the noise floor
    // underneath it is estimated from its neighbours (median of up to MaskFillProbeBins unmasked
    // bins on each side, linearly interpolated across the run). Plain zeroing would have been
    // cheaper still, but it punches a hole in whichever band the partial sits in, so the one band
    // most likely to carry interesting noise would be the one that under-reports it.
    //
    // The costs, stated rather than hidden: (1) the noise under a partial is ESTIMATED, not
    // measured - within one main lobe the partial and the noise are not separable at all; (2) the
    // mask width assumes a Hann-shaped main lobe, which FPwStftResult does not record (see
    // PartialMaskHalfBins); (3) there is no residual waveform to audition, only its band envelopes.
    // The gain is O(frames x tracks) work instead of O(samples x tracks) - at hop 256 that is a
    // factor of 256 - and no additive-phase error at all.
    const int32 NumFrames = Original.NumFrames;
    const int32 NumBins = Original.NumBins;

    TArray<float> Residual = Original.Magnitudes;
    TArray<uint8> Mask;
    Mask.SetNumZeroed(NumBins);
    TArray<int32> Cursors;
    Cursors.SetNumZeroed(Partials.Num());

    const double MsPerFrame = 1000.0 * static_cast<double>(Original.HopSize) / static_cast<double>(SampleRate);
    const double FrameOffsetMs = 1000.0 * 0.5 * static_cast<double>(Original.FftSize) / static_cast<double>(SampleRate);

    for (int32 Frame = 0; Frame < NumFrames; ++Frame)
    {
        // Same frame-centre convention as PwDetectOnsets: the window's midpoint, not its start.
        const double FrameTimeMs = static_cast<double>(Frame) * MsPerFrame + FrameOffsetMs;

        bool bAnyMasked = false;
        FMemory::Memzero(Mask.GetData(), Mask.Num() * sizeof(uint8));
        for (int32 TrackIndex = 0; TrackIndex < Partials.Num(); ++TrackIndex)
        {
            double FreqHz = 0.0;
            double AmpLinear = 0.0;
            if (!SamplePartialAt(Partials[TrackIndex].Points, FrameTimeMs, Cursors[TrackIndex],
                                 FreqHz, AmpLinear))
            {
                continue;
            }
            // A partial that has faded to nothing at this frame is not sounding here; masking it
            // would remove noise that no partial was covering.
            if (!(AmpLinear > SilenceAmp) || !(FreqHz > 0.0))
            {
                continue;
            }

            // Bounded before the narrowing cast: a partial frequency is validated finite but not
            // bounded, and RoundToInt32 of a value past int32's range is undefined rather than
            // merely out of the array.
            const double CentreBinExact = FreqHz / BinHz;
            if (CentreBinExact > static_cast<double>(NumBins + PartialMaskHalfBins))
            {
                continue;
            }

            const int32 CentreBin = FMath::RoundToInt32(CentreBinExact);
            const int32 FirstBin = FMath::Max(0, CentreBin - PartialMaskHalfBins);
            const int32 LastBin = FMath::Min(NumBins - 1, CentreBin + PartialMaskHalfBins);
            for (int32 Bin = FirstBin; Bin <= LastBin; ++Bin)
            {
                Mask[Bin] = 1;
                bAnyMasked = true;
            }
        }

        if (!bAnyMasked)
        {
            continue;
        }

        float* FrameMagnitudes = Residual.GetData() + Frame * NumBins;
        int32 Bin = 0;
        while (Bin < NumBins)
        {
            if (Mask[Bin] == 0)
            {
                ++Bin;
                continue;
            }
            int32 RunEnd = Bin;
            while (RunEnd + 1 < NumBins && Mask[RunEnd + 1] != 0)
            {
                ++RunEnd;
            }

            const double Left = MedianOfUnmaskedNeighbours(FrameMagnitudes, Mask.GetData(), NumBins, Bin - 1, -1);
            const double Right = MedianOfUnmaskedNeighbours(FrameMagnitudes, Mask.GetData(), NumBins, RunEnd + 1, +1);

            if (Left < 0.0 && Right < 0.0)
            {
                // Every bin in the frame is under a partial. There is no floor to estimate from,
                // and inventing one would be the §1 defect - so the frame's residual is zero,
                // which is what "the partials explain all of it" honestly looks like.
                for (int32 Fill = Bin; Fill <= RunEnd; ++Fill)
                {
                    FrameMagnitudes[Fill] = 0.f;
                }
            }
            else if (Left < 0.0 || Right < 0.0)
            {
                const float Only = static_cast<float>(Left >= 0.0 ? Left : Right);
                for (int32 Fill = Bin; Fill <= RunEnd; ++Fill)
                {
                    FrameMagnitudes[Fill] = Only;
                }
            }
            else
            {
                const double Span = static_cast<double>((RunEnd + 1) - (Bin - 1));
                for (int32 Fill = Bin; Fill <= RunEnd; ++Fill)
                {
                    const double Alpha = static_cast<double>(Fill - (Bin - 1)) / Span;
                    FrameMagnitudes[Fill] = static_cast<float>(Left + (Right - Left) * Alpha);
                }
            }

            Bin = RunEnd + 1;
        }
    }

    // ---- log-spaced bands ----------------------------------------------------------------------
    // Bin 0 (DC) is deliberately in no band: it carries any offset the source has and it is not
    // audio. A band that ends up with no bins at all is OMITTED rather than reported at the floor
    // (rpc-design.md §1) - "this band was never measured" and "this band was silent" are different
    // facts, and at a coarse bin width the bottom two or three log bands are always the former.
    const int32 NumBands = Settings.ResidualBands;
    const double LogSpan = FMath::Loge(Nyquist / LowEdgeHz);

    TArray<int32> BandOfBin;
    BandOfBin.Init(-1, NumBins);
    TArray<int32> BinsPerBand;
    BinsPerBand.Init(0, NumBands);
    for (int32 Bin = 1; Bin < NumBins; ++Bin)
    {
        const double CentreHz = static_cast<double>(Bin) * BinHz;
        if (CentreHz < LowEdgeHz)
        {
            continue;
        }
        const int32 Band = FMath::Clamp(
            FMath::FloorToInt32(static_cast<double>(NumBands) * FMath::Loge(CentreHz / LowEdgeHz) / LogSpan),
            0, NumBands - 1);
        BandOfBin[Bin] = Band;
        ++BinsPerBand[Band];
    }

    TArray<TArray<double>> BandLevelDb;
    BandLevelDb.SetNum(NumBands);
    for (int32 Band = 0; Band < NumBands; ++Band)
    {
        if (BinsPerBand[Band] > 0)
        {
            BandLevelDb[Band].SetNumUninitialized(NumFrames);
        }
    }

    TArray<double> FramePower;
    FramePower.SetNumZeroed(NumBands);
    for (int32 Frame = 0; Frame < NumFrames; ++Frame)
    {
        for (int32 Band = 0; Band < NumBands; ++Band)
        {
            FramePower[Band] = 0.0;
        }
        const float* FrameMagnitudes = Residual.GetData() + Frame * NumBins;
        for (int32 Bin = 1; Bin < NumBins; ++Bin)
        {
            const int32 Band = BandOfBin[Bin];
            if (Band >= 0)
            {
                const double Magnitude = static_cast<double>(FrameMagnitudes[Bin]);
                FramePower[Band] += Magnitude * Magnitude;
            }
        }
        for (int32 Band = 0; Band < NumBands; ++Band)
        {
            if (BinsPerBand[Band] > 0)
            {
                // Root-sum-square of the band's bin amplitudes, then PwStft's own dBFS mapping, so
                // a band holding one 0 dBFS component reads 0 dB and the scale is comparable
                // across FFT sizes.
                BandLevelDb[Band][Frame] = static_cast<double>(
                    PwMagnitudeToDb(static_cast<float>(FMath::Sqrt(FramePower[Band])), ResidualFloorDb));
            }
        }
    }

    // ---- smooth, simplify, emit ------------------------------------------------------------------
    int32 MedianFrames = FMath::RoundToInt32(EnvelopeMedianMs / FMath::Max(MsPerFrame, UE_DOUBLE_KINDA_SMALL_NUMBER));
    MedianFrames = FMath::Clamp(MedianFrames, MinMedianFrames, MaxMedianFrames);
    if ((MedianFrames & 1) == 0)
    {
        ++MedianFrames;
    }

    TArray<FPwDbPoint> Dense;
    Dense.Reserve(NumFrames);
    for (int32 Band = 0; Band < NumBands; ++Band)
    {
        if (BinsPerBand[Band] == 0)
        {
            continue;
        }

        MedianSmooth(BandLevelDb[Band], MedianFrames);

        Dense.Reset();
        for (int32 Frame = 0; Frame < NumFrames; ++Frame)
        {
            FPwDbPoint& Point = Dense.AddDefaulted_GetRef();
            Point.TimeMs = static_cast<double>(Frame) * MsPerFrame + FrameOffsetMs;
            Point.ValueDb = BandLevelDb[Band][Frame];
        }

        FPwResidualBand& OutBand = Out.AddDefaulted_GetRef();
        OutBand.LowHz = LowEdgeHz * FMath::Exp(LogSpan * static_cast<double>(Band) / static_cast<double>(NumBands));
        OutBand.HighHz = LowEdgeHz * FMath::Exp(LogSpan * static_cast<double>(Band + 1) / static_cast<double>(NumBands));
        SimplifyEnvelopeDb(Dense, Settings.SimplifyToleranceDb, OutBand.EnvelopeDb);
    }

    if (Out.Num() == 0)
    {
        // Every band came out binless, which only happens when the whole spectrum sits below
        // MinBandHz. Reporting an empty success would publish "the residual has no bands" as if
        // that were a measurement of the sound.
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Residual: none of the %d requested bands contains a spectrogram ")
                TEXT("bin (bin width %.3f Hz, band range %.1f-%.1f Hz). Ask for fewer bands or ")
                TEXT("analyse with a larger fftSize."),
                NumBands, BinHz, LowEdgeHz, Nyquist));
    }

    return true;
}
