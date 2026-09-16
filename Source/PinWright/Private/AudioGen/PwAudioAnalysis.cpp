// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "AudioGen/PwAudioAnalysis.h"

#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwStft.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/ErrorCodes.h"
#include "Math/UnrealMathUtility.h"
#include "PinWrightSubsystem.h"

// Named (not anonymous) namespace: this module builds with bUseUnity = true, and anonymous
// namespaces in merged TUs are the ODR-collision source the plugin's Build.cs comment warns about.
namespace PwAudioAnalysisInternal
{
    bool Fail(FString& OutErrorCode, FString& OutError, const TCHAR* Code, FString&& Message)
    {
        OutErrorCode = Code;
        OutError = MoveTemp(Message);
        return false;
    }

    /** Caller must guard Value > 0. */
    double LinearToDb(double Value)
    {
        return 20.0 * FMath::LogX(10.0, Value);
    }

    double DbToLinear(double Db)
    {
        return FMath::Pow(10.0, Db / 20.0);
    }

    /**
     * Rounds to a stated number of decimals before emission - the report must not publish more
     * precision than it measured, and UE's JSON writer prints doubles with "%.17g", so an
     * unrounded value can cost twenty characters of a response budget measured in thousands.
     *
     * Every producer in this file guards its divisions, so a non-finite value reaching here is a
     * bug rather than a data condition; it is logged and emitted as 0 because the alternative is
     * writing "inf" into the payload and handing the caller invalid JSON.
     */
    double RoundTo(double Value, int32 Decimals)
    {
        if (!FMath::IsFinite(Value))
        {
            UE_LOG(LogPinWrightSubsystem, Warning,
                TEXT("PwAudioAnalysis: non-finite value reached serialization; emitting 0."));
            return 0.0;
        }
        const double Scale = FMath::Pow(10.0, static_cast<double>(Decimals));
        return FMath::RoundToDouble(Value * Scale) / Scale;
    }

    void SetNum(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, double Value, int32 Decimals)
    {
        Obj->SetNumberField(Key, RoundTo(Value, Decimals));
    }

    void SetOptNum(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key,
        const TOptional<double>& Value, int32 Decimals)
    {
        if (Value.IsSet())
        {
            Obj->SetNumberField(Key, RoundTo(Value.GetValue(), Decimals));
        }
    }

    /** An absent family always carries a reason; this is the fallback for a report that never ran. */
    FString ReasonOr(const FPwAudioFamilyState& State)
    {
        return State.Reason.IsEmpty() ? FString(TEXT("not measured")) : State.Reason;
    }

    double Median(TArray<double> Values)
    {
        if (Values.Num() == 0)
        {
            return 0.0;
        }
        Values.Sort();
        return Values[Values.Num() / 2];
    }

    /**
     * A per-frame series in the full-detail form, always as the same object shape so a consumer
     * never has to branch on whether decimation happened. Over MaxSeriesPoints the series is
     * thinned and SAYS SO, because a silently thinned series and a short one look identical.
     */
    void AddSeries(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key,
        int32 TotalPoints, int32 Stride, const TCHAR* Reduction,
        TArray<TSharedPtr<FJsonValue>>&& Values)
    {
        const TSharedPtr<FJsonObject> Series = MakeShared<FJsonObject>();
        Series->SetNumberField(TEXT("totalPoints"), TotalPoints);
        Series->SetNumberField(TEXT("stride"), Stride);
        Series->SetStringField(TEXT("reduction"), Reduction);
        Series->SetArrayField(TEXT("values"), MoveTemp(Values));
        Obj->SetObjectField(Key, Series);
    }

    /** Stride that keeps a series of Num points inside the cap. */
    int32 SeriesStride(int32 Num)
    {
        return FMath::Max(1, FMath::DivideAndRoundUp(Num, PwAudioAnalysisLimits::MaxSeriesPoints));
    }

    // -----------------------------------------------------------------------------------------
    // Block RMS envelope. Every timing metric in the report is measured on this one envelope, so
    // they share a definition and cannot disagree about where the sound starts.
    // -----------------------------------------------------------------------------------------
    struct FBlockEnvelope
    {
        TArray<double> Rms;
        double BlockMs = 0.0;
        int32 PeakIndex = INDEX_NONE;
        double PeakRms = 0.0;

        bool IsValid() const { return Rms.Num() > 0 && PeakRms > 0.0; }
    };

    /**
     * Whole blocks only: a trailing remainder shorter than one block is dropped. RMS normalizes
     * by the sample count, so a two-sample remainder that happened to land on a zero crossing
     * would otherwise read as a full decay to silence in the last 0.04 ms of the buffer.
     */
    FBlockEnvelope BuildEnvelope(TArrayView<const float> Mono, int32 SampleRate)
    {
        FBlockEnvelope Envelope;
        const int32 BlockSamples = FMath::Max(1,
            FMath::RoundToInt32(SampleRate * PwAudioAnalysisLimits::EnvelopeBlockMs / 1000.0));
        const int32 NumBlocks = Mono.Num() / BlockSamples;
        if (NumBlocks <= 0)
        {
            return Envelope;
        }

        Envelope.BlockMs = 1000.0 * BlockSamples / static_cast<double>(SampleRate);
        Envelope.Rms.SetNumUninitialized(NumBlocks);
        for (int32 Block = 0; Block < NumBlocks; ++Block)
        {
            double SumSquares = 0.0;
            const int32 Begin = Block * BlockSamples;
            for (int32 Index = Begin; Index < Begin + BlockSamples; ++Index)
            {
                const double Sample = Mono[Index];
                SumSquares += Sample * Sample;
            }
            const double Rms = FMath::Sqrt(SumSquares / BlockSamples);
            Envelope.Rms[Block] = Rms;
            if (Rms > Envelope.PeakRms)
            {
                Envelope.PeakRms = Rms;
                Envelope.PeakIndex = Block;
            }
        }
        return Envelope;
    }

    // -----------------------------------------------------------------------------------------
    // Spectral helpers, all measured on the mean magnitude spectrum with DC and Nyquist excluded.
    // -----------------------------------------------------------------------------------------

    /** Greedy dominant-peak pick: strongest first, separated, and inside the dynamic-range gate. */
    void CollectPeaks(const TArray<double>& Mean, double BinHz, TArray<FPwAudioSpectralPeak>& Out)
    {
        const int32 NumBins = Mean.Num();
        if (NumBins < 5)
        {
            return;
        }

        TArray<int32> Candidates;
        double Strongest = 0.0;
        for (int32 Bin = 2; Bin <= NumBins - 3; ++Bin)
        {
            if (Mean[Bin] > Mean[Bin - 1] && Mean[Bin] >= Mean[Bin + 1])
            {
                Candidates.Add(Bin);
                Strongest = FMath::Max(Strongest, Mean[Bin]);
            }
        }
        if (Candidates.Num() == 0 || Strongest <= 0.0)
        {
            return;
        }
        Candidates.Sort([&Mean](int32 A, int32 B) { return Mean[A] > Mean[B]; });

        const double FloorMagnitude = Strongest * DbToLinear(-PwAudioAnalysisLimits::PeakDynamicRangeDb);
        TArray<int32> Selected;
        for (const int32 Bin : Candidates)
        {
            if (Selected.Num() >= PwAudioAnalysisLimits::MaxDetailPeaks)
            {
                break;
            }
            if (Mean[Bin] < FloorMagnitude)
            {
                // Candidates are sorted, so the first one under the gate ends the list.
                break;
            }
            bool bTooClose = false;
            for (const int32 Taken : Selected)
            {
                if (FMath::Abs(Bin - Taken) < PwAudioAnalysisLimits::MinPeakSeparationBins)
                {
                    bTooClose = true;
                    break;
                }
            }
            if (bTooClose)
            {
                continue;
            }
            Selected.Add(Bin);

            // Sub-bin frequency by parabolic interpolation on the LOG magnitudes, which is the
            // form that is exact for a Gaussian-ish main lobe. The level is the bin's own, not
            // the interpolated apex: interpolating a level the transform never produced would be
            // a computed number reported as a measured one.
            const double YLow = FMath::Loge(FMath::Max(Mean[Bin - 1], UE_DOUBLE_SMALL_NUMBER));
            const double YMid = FMath::Loge(FMath::Max(Mean[Bin], UE_DOUBLE_SMALL_NUMBER));
            const double YHigh = FMath::Loge(FMath::Max(Mean[Bin + 1], UE_DOUBLE_SMALL_NUMBER));
            const double Denominator = YLow - 2.0 * YMid + YHigh;
            double Delta = 0.0;
            if (FMath::Abs(Denominator) > UE_DOUBLE_SMALL_NUMBER)
            {
                Delta = FMath::Clamp(0.5 * (YLow - YHigh) / Denominator, -0.5, 0.5);
            }

            FPwAudioSpectralPeak Peak;
            Peak.Hz = (Bin + Delta) * BinHz;
            Peak.Db = LinearToDb(FMath::Max(Mean[Bin], UE_DOUBLE_SMALL_NUMBER));
            Out.Add(Peak);
        }
    }
}

// =============================================================================================
// PwAnalyzeBuffer
// =============================================================================================
bool PwAnalyzeBuffer(const FPwAudioBuffer& In, FPwAudioAnalysis& Out,
                     FString& OutErrorCode, FString& OutError)
{
    using namespace PwAudioAnalysisInternal;
    using namespace PwAudioAnalysisLimits;

    // Failure is the default: every family in a freshly constructed report is unmeasured, so an
    // early return cannot leave a number behind for a caller that ignored the bool.
    Out = FPwAudioAnalysis();
    OutErrorCode.Reset();
    OutError.Reset();

    // -----------------------------------------------------------------------------------------
    // Degenerate cases first, each ANSWERED here rather than left to fall through into a
    // threshold test (rpc-design.md §7).
    // -----------------------------------------------------------------------------------------
    const int32 NumFrames = In.Left.Num();
    if (NumFrames <= 0)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
            TEXT("Buffer holds 0 frames; there is nothing to measure. This is not the same as a ")
            TEXT("silent render - a silent buffer of the right length analyses successfully and ")
            TEXT("reports digitalSilence."));
    }
    if (In.Right.Num() != NumFrames)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Buffer channel lengths disagree: left has %d frames, right has %d. ")
                            TEXT("FPwAudioBuffer's invariant is that both channels are the same length."),
                NumFrames, In.Right.Num()));
    }
    if (In.SampleRate <= 0)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Buffer sample rate is %d; every time-domain and frequency-domain ")
                            TEXT("measurement needs a positive rate."), In.SampleRate));
    }

    // Non-finite BEFORE silence, and the order is load-bearing: NaN compares false against every
    // threshold, so a NaN-filled buffer tested for silence first passes the silence test and is
    // reported as valid audio that happens to be quiet. Inf is the mirror case - it would be
    // reported as a clipped but analysable signal while poisoning every sum it enters.
    int32 NonFinite = 0;
    int32 FirstNonFiniteFrame = INDEX_NONE;
    const TCHAR* FirstNonFiniteChannel = TEXT("");
    for (int32 Index = 0; Index < NumFrames; ++Index)
    {
        const bool bLeftBad = !FMath::IsFinite(In.Left[Index]);
        const bool bRightBad = !FMath::IsFinite(In.Right[Index]);
        NonFinite += (bLeftBad ? 1 : 0) + (bRightBad ? 1 : 0);
        if ((bLeftBad || bRightBad) && FirstNonFiniteFrame == INDEX_NONE)
        {
            FirstNonFiniteFrame = Index;
            FirstNonFiniteChannel = bLeftBad ? TEXT("left") : TEXT("right");
        }
    }
    if (NonFinite > 0)
    {
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_AUDIO_NON_FINITE_SAMPLES,
            FString::Printf(TEXT("Buffer holds %d non-finite samples (NaN or infinity), first at ")
                            TEXT("frame %d of the %s channel. One of them poisons every sum, every ")
                            TEXT("FFT bin and every threshold comparison downstream, so no metric ")
                            TEXT("is reported rather than a mixture of real and meaningless ones."),
                NonFinite, FirstNonFiniteFrame, FirstNonFiniteChannel));
    }

    // -----------------------------------------------------------------------------------------
    // One pass over the samples: everything the technical and stereo families need. The stereo
    // mid/side energies are derived from these same sums (mid^2 = (L^2 + 2LR + R^2)/4), so the
    // image measurement needs no second pass and cannot disagree with the level measurement.
    // -----------------------------------------------------------------------------------------
    TArray<float> Mono;
    Mono.SetNumUninitialized(NumFrames);

    double SumL = 0.0, SumR = 0.0;
    double SumL2 = 0.0, SumR2 = 0.0, SumLR = 0.0;
    double PeakAbs = 0.0, PeakLeft = 0.0, PeakRight = 0.0;
    int32 ClippedSamples = 0;
    int32 ZeroCrossings = 0;
    bool bPreviousNonNegative = true;

    for (int32 Index = 0; Index < NumFrames; ++Index)
    {
        const double Left = In.Left[Index];
        const double Right = In.Right[Index];
        const double MonoSample = 0.5 * (Left + Right);
        Mono[Index] = static_cast<float>(MonoSample);

        SumL += Left;
        SumR += Right;
        SumL2 += Left * Left;
        SumR2 += Right * Right;
        SumLR += Left * Right;

        const double AbsLeft = FMath::Abs(Left);
        const double AbsRight = FMath::Abs(Right);
        PeakLeft = FMath::Max(PeakLeft, AbsLeft);
        PeakRight = FMath::Max(PeakRight, AbsRight);

        ClippedSamples += (AbsLeft >= ClipThreshold ? 1 : 0) + (AbsRight >= ClipThreshold ? 1 : 0);

        const bool bNonNegative = MonoSample >= 0.0;
        if (Index > 0 && bNonNegative != bPreviousNonNegative)
        {
            ++ZeroCrossings;
        }
        bPreviousNonNegative = bNonNegative;
    }
    PeakAbs = FMath::Max(PeakLeft, PeakRight);

    const double InverseFrames = 1.0 / static_cast<double>(NumFrames);
    const double DurationMs = 1000.0 * NumFrames / static_cast<double>(In.SampleRate);

    FPwAudioTechnical& Technical = Out.Technical;
    Technical.NonFiniteSamples = 0;
    Technical.DurationMs = DurationMs;
    Technical.SampleRate = In.SampleRate;
    Technical.NumFrames = NumFrames;
    Technical.PeakLinear = PeakAbs;
    Technical.ClippedSamples = ClippedSamples;
    Technical.DcOffset = (SumL + SumR) * 0.5 * InverseFrames;
    Technical.ZeroCrossingRate = (NumFrames > 1)
        ? ZeroCrossings * static_cast<double>(In.SampleRate) / (NumFrames - 1)
        : 0.0;
    Technical.StartDiscontinuity = FMath::Abs(static_cast<double>(Mono[0]));
    Technical.EndDiscontinuity = FMath::Abs(static_cast<double>(Mono[NumFrames - 1]));

    // -----------------------------------------------------------------------------------------
    // Digital silence: named here and answered here. Everything below this point is a level or a
    // brightness measurement, and every one of them is undefined on an all-zero buffer - a
    // 0/0 centroid, a log of zero for the peak, a -inf LUFS. Reporting them as small numbers is
    // the exact defect §7 exists to prevent: "-90 LUFS, centroid 0 Hz" sends the agent off
    // raising the gain on a signal that does not exist.
    // -----------------------------------------------------------------------------------------
    if (PeakAbs < SilencePeak)
    {
        Technical.bDigitalSilence = true;
        Technical.ActiveDurationMs = 0.0;
        Technical.State.MarkMeasured();

        const FString Reason = FString::Printf(
            TEXT("buffer is digital silence (peak %.3e is below the %.0e floor, about -180 dBFS); ")
            TEXT("this metric is undefined for a signal that is not there"), PeakAbs, SilencePeak);
        Out.Envelope.State.MarkUnmeasured(CopyTemp(Reason));
        Out.Loudness.State.MarkUnmeasured(CopyTemp(Reason));
        Out.Spectral.State.MarkUnmeasured(CopyTemp(Reason));
        Out.Pitch.State.MarkUnmeasured(CopyTemp(Reason));
        Out.Stereo.State.MarkUnmeasured(CopyTemp(Reason));
        return true;
    }

    Technical.PeakDb = LinearToDb(PeakAbs);
    const double RmsAll = FMath::Sqrt(FMath::Max(0.0, (SumL2 + SumR2) * 0.5 * InverseFrames));
    if (RmsAll > 0.0)
    {
        Technical.RmsDb = LinearToDb(RmsAll);
    }

    // -----------------------------------------------------------------------------------------
    // Envelope.
    // -----------------------------------------------------------------------------------------
    const FBlockEnvelope Envelope = BuildEnvelope(Mono, In.SampleRate);
    if (!Envelope.IsValid() || !Technical.RmsDb.IsSet())
    {
        // The RmsDb arm is unreachable arithmetic (a peak at or above the silence floor implies a
        // positive RMS) but it is checked rather than assumed, because the alternative is
        // publishing a crest factor computed against an absent RMS.
        const FString Reason = Envelope.Rms.Num() == 0
            ? FString::Printf(TEXT("buffer is %.2f ms, shorter than one %.0f ms envelope block"),
                DurationMs, PwAudioAnalysisLimits::EnvelopeBlockMs)
            : FString::Printf(TEXT("every whole %.0f ms envelope block is silent; the only signal ")
                              TEXT("sits in the trailing partial block"),
                PwAudioAnalysisLimits::EnvelopeBlockMs);
        Out.Envelope.State.MarkUnmeasured(CopyTemp(Reason));
    }
    else
    {
        const double FloorLinear = Envelope.PeakRms * DbToLinear(EnvelopeFloorDb);
        const double TailLinear = Envelope.PeakRms * DbToLinear(EnvelopeTailDb);

        int32 FirstActive = INDEX_NONE;
        int32 ActiveBlocks = 0;
        double WeightedTime = 0.0;
        double WeightSum = 0.0;
        for (int32 Block = 0; Block < Envelope.Rms.Num(); ++Block)
        {
            const double Rms = Envelope.Rms[Block];
            if (Rms > FloorLinear)
            {
                if (FirstActive == INDEX_NONE)
                {
                    FirstActive = Block;
                }
                ++ActiveBlocks;
            }
            WeightedTime += (Block + 0.5) * Envelope.BlockMs * Rms;
            WeightSum += Rms;
        }

        // The peak block is by definition above the floor, so FirstActive is always set here.
        int32 DecayEnd = INDEX_NONE;
        int32 TailStart = INDEX_NONE;
        for (int32 Block = Envelope.PeakIndex; Block < Envelope.Rms.Num(); ++Block)
        {
            if (TailStart == INDEX_NONE && Envelope.Rms[Block] <= TailLinear)
            {
                TailStart = Block;
            }
            if (Envelope.Rms[Block] <= FloorLinear)
            {
                DecayEnd = Block;
                break;
            }
        }

        FPwAudioEnvelope& Result = Out.Envelope;
        Result.OnsetMs = FirstActive * Envelope.BlockMs;
        Result.AttackMs = FMath::Max(0, Envelope.PeakIndex - FirstActive) * Envelope.BlockMs;
        if (DecayEnd != INDEX_NONE)
        {
            Result.DecayMs = (DecayEnd - Envelope.PeakIndex) * Envelope.BlockMs;
            if (TailStart != INDEX_NONE)
            {
                Result.TailMs = FMath::Max(0, DecayEnd - TailStart) * Envelope.BlockMs;
            }
        }
        Result.TemporalCentroidMs = (WeightSum > 0.0) ? (WeightedTime / WeightSum) : 0.0;
        Result.CrestDb = Technical.PeakDb.GetValue() - Technical.RmsDb.GetValue();
        Result.State.MarkMeasured();

        Technical.ActiveDurationMs = ActiveBlocks * Envelope.BlockMs;
    }
    Technical.State.MarkMeasured();

    // -----------------------------------------------------------------------------------------
    // Spectral. The window shrinks for a short buffer rather than failing outright, because a
    // 30 ms impact is exactly the material this subsystem generates.
    // -----------------------------------------------------------------------------------------
    FPwStftResult Stft;
    {
        int32 FftSize = PreferredFftSize;
        while (FftSize > MinFftSize && NumFrames < FftSize)
        {
            FftSize >>= 1;
        }
        if (NumFrames < FftSize)
        {
            Out.Spectral.State.MarkUnmeasured(FString::Printf(
                TEXT("buffer is %d samples (%.2f ms), shorter than the smallest %d-sample analysis window"),
                NumFrames, DurationMs, MinFftSize));
        }
        else
        {
            FPwStftSettings Settings;
            Settings.FftSize = FftSize;
            Settings.HopSize = FMath::Max(1, FftSize / 4);

            FPwStftError StftError;
            if (!PwComputeStft(Mono, In.SampleRate, Settings, Stft, &StftError))
            {
                Out.Spectral.State.MarkUnmeasured(FString::Printf(TEXT("%s: %s"),
                    *StftError.Code, *StftError.Message));
            }
        }
    }

    if (Stft.IsValid())
    {
        FPwAudioSpectral& Spectral = Out.Spectral;
        const int32 NumBins = Stft.NumBins;
        const double BinHz = Stft.BinHz;

        // Mean magnitude spectrum. Averaging the SPECTRA and then measuring is deliberate: the
        // mean of per-frame flatness values for white noise converges on 0.56 (a single-frame
        // periodogram has exponentially distributed bins), which reads as "half tonal" for a
        // signal that is definitionally flat.
        TArray<double> Mean;
        Mean.SetNumZeroed(NumBins);
        for (int32 Frame = 0; Frame < Stft.NumFrames; ++Frame)
        {
            const float* Row = Stft.Magnitudes.GetData() + static_cast<int64>(Frame) * NumBins;
            for (int32 Bin = 0; Bin < NumBins; ++Bin)
            {
                Mean[Bin] += Row[Bin];
            }
        }
        const double InverseStftFrames = 1.0 / static_cast<double>(Stft.NumFrames);
        for (int32 Bin = 0; Bin < NumBins; ++Bin)
        {
            Mean[Bin] *= InverseStftFrames;
        }

        // DC and Nyquist are excluded from every spectral metric: neither has a folding partner,
        // so their magnitudes sit on a different scale than the interior bins, and DC would let a
        // plain offset dominate the centroid.
        const int32 FirstBin = 1;
        const int32 LastBin = NumBins - 2;
        const int32 InteriorBins = FMath::Max(0, LastBin - FirstBin + 1);

        double SumMagnitude = 0.0;
        double SumMagnitudeFreq = 0.0;
        double SumPower = 0.0;
        double SumLogPower = 0.0;
        for (int32 Bin = FirstBin; Bin <= LastBin; ++Bin)
        {
            const double Magnitude = Mean[Bin];
            const double Power = Magnitude * Magnitude;
            SumMagnitude += Magnitude;
            SumMagnitudeFreq += Magnitude * Bin * BinHz;
            SumPower += Power;
            // Power floor at 1e-20 (-200 dBFS): a bin that came out exactly zero would otherwise
            // send the geometric mean, and with it every flatness reading, to zero.
            SumLogPower += FMath::Loge(FMath::Max(Power, 1e-20));
        }

        if (InteriorBins <= 0 || SumMagnitude <= 0.0)
        {
            Spectral.State.MarkUnmeasured(FString::Printf(
                TEXT("the %d-bin mean spectrum carries no energy outside DC and Nyquist"), NumBins));
        }
        else
        {
            Spectral.CentroidHz = SumMagnitudeFreq / SumMagnitude;

            double SumSpread = 0.0;
            for (int32 Bin = FirstBin; Bin <= LastBin; ++Bin)
            {
                const double Deviation = Bin * BinHz - Spectral.CentroidHz;
                SumSpread += Mean[Bin] * Deviation * Deviation;
            }
            Spectral.BandwidthHz = FMath::Sqrt(FMath::Max(0.0, SumSpread / SumMagnitude));

            const double RolloffTarget = RolloffFraction * SumMagnitude;
            double Cumulative = 0.0;
            Spectral.RolloffHz = LastBin * BinHz;
            for (int32 Bin = FirstBin; Bin <= LastBin; ++Bin)
            {
                Cumulative += Mean[Bin];
                if (Cumulative >= RolloffTarget)
                {
                    Spectral.RolloffHz = Bin * BinHz;
                    break;
                }
            }

            const double GeometricMean = FMath::Exp(SumLogPower / InteriorBins);
            const double ArithmeticMean = SumPower / InteriorBins;
            Spectral.Flatness = (ArithmeticMean > 0.0)
                ? FMath::Clamp(GeometricMean / ArithmeticMean, 0.0, 1.0)
                : 0.0;

            // Six-band power split. A band whose LOW edge is at or above Nyquist is skipped
            // outright: at 8 kHz there is no 8k-20k band to be empty, and a 0.0 there would read
            // as "you have no top end" rather than "this rate cannot carry one".
            const double NyquistHz = 0.5 * In.SampleRate;
            Spectral.NyquistHz = NyquistHz;

            double BandEnergy[NumBands] = {};
            double TotalBandEnergy = 0.0;
            for (int32 Band = 0; Band < NumBands; ++Band)
            {
                if (BandEdgesHz[Band] >= NyquistHz)
                {
                    continue;
                }
                for (int32 Bin = FirstBin; Bin <= LastBin; ++Bin)
                {
                    const double Frequency = Bin * BinHz;
                    if (Frequency >= BandEdgesHz[Band] && Frequency < BandEdgesHz[Band + 1])
                    {
                        BandEnergy[Band] += Mean[Bin] * Mean[Bin];
                    }
                }
                TotalBandEnergy += BandEnergy[Band];
            }
            if (TotalBandEnergy > 0.0)
            {
                for (int32 Band = 0; Band < NumBands; ++Band)
                {
                    if (BandEdgesHz[Band] >= NyquistHz)
                    {
                        continue;
                    }
                    FPwAudioBandRatio Ratio;
                    Ratio.LowHz = BandEdgesHz[Band];
                    Ratio.HighHz = BandEdgesHz[Band + 1];
                    Ratio.Ratio = BandEnergy[Band] / TotalBandEnergy;
                    Spectral.Bands.Add(Ratio);
                }
            }

            if (Stft.NumFrames >= 2)
            {
                Spectral.Flux.SetNumUninitialized(Stft.NumFrames - 1);
                double FluxSum = 0.0;
                double FluxMax = 0.0;
                for (int32 Frame = 1; Frame < Stft.NumFrames; ++Frame)
                {
                    const float* Current = Stft.Magnitudes.GetData() + static_cast<int64>(Frame) * NumBins;
                    const float* Previous = Current - NumBins;
                    double Accumulator = 0.0;
                    for (int32 Bin = FirstBin; Bin <= LastBin; ++Bin)
                    {
                        Accumulator += FMath::Max(0.f, Current[Bin] - Previous[Bin]);
                    }
                    const double Flux = Accumulator / InteriorBins;
                    Spectral.Flux[Frame - 1] = static_cast<float>(Flux);
                    FluxSum += Flux;
                    FluxMax = FMath::Max(FluxMax, Flux);
                }
                Spectral.FluxMean = FluxSum / (Stft.NumFrames - 1);
                Spectral.FluxMax = FluxMax;
            }

            CollectPeaks(Mean, BinHz, Spectral.Peaks);

            Spectral.FftSize = Stft.FftSize;
            Spectral.HopSize = Stft.HopSize;
            Spectral.NumFrames = Stft.NumFrames;
            Spectral.HopMs = 1000.0 * Stft.HopSize / static_cast<double>(In.SampleRate);
            Spectral.State.MarkMeasured();
        }

        // Onsets share the spectrogram, so they are only reachable when it exists. A failure here
        // leaves TransientCount unset rather than 0: "no transients were found" and "transient
        // detection did not run" are different answers and only one of them means the sound is
        // smooth.
        if (Out.Envelope.State.bMeasured)
        {
            TArray<FPwOnset> Onsets;
            FString OnsetCode;
            FString OnsetMessage;
            if (PwDetectOnsets(Stft, In.SampleRate, Onsets, OnsetCode, OnsetMessage))
            {
                Out.Envelope.TransientCount = Onsets.Num();
                Out.Envelope.Onsets = MoveTemp(Onsets);
            }
            else
            {
                Out.Envelope.OnsetReason = FString::Printf(TEXT("%s: %s"), *OnsetCode, *OnsetMessage);
            }
        }
    }
    else if (Out.Envelope.State.bMeasured)
    {
        Out.Envelope.OnsetReason = FString::Printf(TEXT("onset detection needs the spectrogram: %s"),
            *ReasonOr(Out.Spectral.State));
    }

    // -----------------------------------------------------------------------------------------
    // Loudness. A back-end failure takes down only this family.
    // -----------------------------------------------------------------------------------------
    {
        FPwLoudnessResult Loudness;
        FString LoudnessCode;
        FString LoudnessMessage;
        if (!PwComputeLoudness(In, Loudness, LoudnessCode, LoudnessMessage))
        {
            Out.Loudness.State.MarkUnmeasured(FString::Printf(TEXT("%s: %s"),
                *LoudnessCode, *LoudnessMessage));
        }
        else if (!Loudness.bMeasured)
        {
            Out.Loudness.State.MarkUnmeasured(
                TEXT("the loudness back-end ran but reported no measurement, which it does for a ")
                TEXT("buffer shorter than one BS.1770 integration window"));
        }
        else
        {
            Out.Loudness.IntegratedLufs = Loudness.IntegratedLufs;
            Out.Loudness.ShortTermMaxLufs = Loudness.ShortTermMaxLufs;
            Out.Loudness.MomentaryMaxLufs = Loudness.MomentaryMaxLufs;
            Out.Loudness.bLoudnessRangeMeasured = Loudness.bLoudnessRangeMeasured;
            Out.Loudness.LoudnessRangeLu = Loudness.LoudnessRangeLu;
            Out.Loudness.State.MarkMeasured();
        }
    }

    // -----------------------------------------------------------------------------------------
    // Pitch, with the confidence gate.
    // -----------------------------------------------------------------------------------------
    {
        FPwPitchResult Pitch;
        FString PitchCode;
        FString PitchMessage;
        if (!PwEstimatePitch(Mono, In.SampleRate, Pitch, PitchCode, PitchMessage))
        {
            Out.Pitch.State.MarkUnmeasured(FString::Printf(TEXT("%s: %s"), *PitchCode, *PitchMessage));
        }
        else if (!Pitch.bMeasured)
        {
            Out.Pitch.State.MarkUnmeasured(
                TEXT("the pitch estimator ran but reported no measurement"));
        }
        else if (Pitch.Confidence < MinPitchConfidence)
        {
            // Suppressed ENTIRELY, fields left cleared. Noise and clicks have no meaningful f0,
            // and a number the agent can read is worse than no number: it will chase it.
            //
            // The confidence goes in the REASON rather than into the report, which is the whole
            // distinction: it explains why there is no pitch, it is not a measurement of one.
            // PwEstimatePitch has already zeroed MedianF0Hz at this confidence, so the estimate
            // is not quotable here even if it were worth quoting.
            Out.Pitch.State.MarkUnmeasured(FString::Printf(
                TEXT("pitch confidence %.3f is below the %.2f floor, so no f0 is reported; ")
                TEXT("noise and clicks have no meaningful pitch (motion class: '%s')"),
                Pitch.Confidence, MinPitchConfidence,
                Pitch.Motion.IsEmpty() ? TEXT("unknown") : *Pitch.Motion));
        }
        else if (!(Pitch.MedianF0Hz > 0.0))
        {
            // Confident overall, yet the estimator still cleared its aggregate - too few windows
            // landed inside the 50-2000 Hz search range to take a median over.
            Out.Pitch.State.MarkUnmeasured(FString::Printf(
                TEXT("the estimator is %.3f confident but produced no aggregate f0, which it does ")
                TEXT("when too few windows resolved inside its search range"), Pitch.Confidence));
        }
        else
        {
            Out.Pitch.F0Hz = Pitch.MedianF0Hz;
            Out.Pitch.Confidence = Pitch.Confidence;
            Out.Pitch.Motion = Pitch.Motion;

            // Signed semitone travel across the confident part of the track (§6): "rising" and
            // "falling" become different NUMBERS, and the number says how far, which the class
            // cannot. Quartile medians rather than the raw endpoints so one stray frame at either
            // end cannot invert the sign.
            TArray<double> Confident;
            Confident.Reserve(Pitch.Track.Num());
            for (const FPwPitchPointResult& Point : Pitch.Track)
            {
                if (Point.F0Hz > 0.0 && Point.Confidence >= MinPitchConfidence)
                {
                    Confident.Add(Point.F0Hz);
                }
            }
            if (Confident.Num() >= 4)
            {
                const int32 Quarter = FMath::Max(1, Confident.Num() / 4);
                TArray<double> Head(Confident.GetData(), Quarter);
                TArray<double> Tail(Confident.GetData() + Confident.Num() - Quarter, Quarter);
                const double HeadHz = Median(MoveTemp(Head));
                const double TailHz = Median(MoveTemp(Tail));
                if (HeadHz > 0.0 && TailHz > 0.0)
                {
                    Out.Pitch.SemitoneDelta = 12.0 * FMath::LogX(2.0, TailHz / HeadHz);
                }
            }
            else if (Confident.Num() >= 2)
            {
                Out.Pitch.SemitoneDelta = 12.0 * FMath::LogX(2.0, Confident.Last() / Confident[0]);
            }

            Out.Pitch.Track = Pitch.Track;
            Out.Pitch.State.MarkMeasured();
        }
    }

    // -----------------------------------------------------------------------------------------
    // Stereo image, from the sums already gathered above.
    // -----------------------------------------------------------------------------------------
    {
        FPwAudioStereo& Stereo = Out.Stereo;

        const double MeanL = SumL * InverseFrames;
        const double MeanR = SumR * InverseFrames;
        const double VarianceL = FMath::Max(0.0, SumL2 * InverseFrames - MeanL * MeanL);
        const double VarianceR = FMath::Max(0.0, SumR2 * InverseFrames - MeanR * MeanR);
        const double Covariance = SumLR * InverseFrames - MeanL * MeanR;

        const bool bLeftSilent = PeakLeft < SilencePeak;
        const bool bRightSilent = PeakRight < SilencePeak;
        Stereo.bOneChannelSilent = bLeftSilent != bRightSilent;
        Stereo.SilentChannel = Stereo.bOneChannelSilent
            ? FString(bLeftSilent ? TEXT("left") : TEXT("right"))
            : FString();

        if (VarianceL > 0.0 && VarianceR > 0.0)
        {
            Stereo.Correlation = FMath::Clamp(
                Covariance / FMath::Sqrt(VarianceL * VarianceR), -1.0, 1.0);
        }

        const double MidRms = FMath::Sqrt(FMath::Max(0.0, 0.25 * (SumL2 + 2.0 * SumLR + SumR2) * InverseFrames));
        const double SideRms = FMath::Sqrt(FMath::Max(0.0, 0.25 * (SumL2 - 2.0 * SumLR + SumR2) * InverseFrames));
        Stereo.Width = (MidRms + SideRms > 0.0) ? (SideRms / (MidRms + SideRms)) : 0.0;

        const double ReferenceRms = FMath::Sqrt(FMath::Max(0.0, 0.5 * (SumL2 + SumR2) * InverseFrames));
        Stereo.MonoCompatibility = (ReferenceRms > 0.0) ? (MidRms / ReferenceRms) : 0.0;

        Stereo.State.MarkMeasured();
    }

    return true;
}

// =============================================================================================
// PwGetAudioAnalysisMetric
// =============================================================================================
bool PwGetAudioAnalysisMetric(const FPwAudioAnalysis& In, EPwSynthMetric Metric, double& OutValue)
{
    const FPwAudioTechnical& Technical = In.Technical;

    switch (Metric)
    {
    case EPwSynthMetric::PeakDb:
        if (Technical.State.bMeasured && Technical.PeakDb.IsSet())
        {
            OutValue = Technical.PeakDb.GetValue();
            return true;
        }
        return false;

    case EPwSynthMetric::RmsDb:
        if (Technical.State.bMeasured && Technical.RmsDb.IsSet())
        {
            OutValue = Technical.RmsDb.GetValue();
            return true;
        }
        return false;

    case EPwSynthMetric::Lufs:
        if (In.Loudness.State.bMeasured)
        {
            OutValue = In.Loudness.IntegratedLufs;
            return true;
        }
        return false;

    case EPwSynthMetric::CrestDb:
        if (In.Envelope.State.bMeasured)
        {
            OutValue = In.Envelope.CrestDb;
            return true;
        }
        return false;

    case EPwSynthMetric::DurationMs:
        if (Technical.State.bMeasured)
        {
            OutValue = Technical.DurationMs;
            return true;
        }
        return false;

    case EPwSynthMetric::CentroidHz:
        if (In.Spectral.State.bMeasured)
        {
            OutValue = In.Spectral.CentroidHz;
            return true;
        }
        return false;

    case EPwSynthMetric::RolloffHz:
        if (In.Spectral.State.bMeasured)
        {
            OutValue = In.Spectral.RolloffHz;
            return true;
        }
        return false;

    case EPwSynthMetric::Flatness:
        if (In.Spectral.State.bMeasured)
        {
            OutValue = In.Spectral.Flatness;
            return true;
        }
        return false;

    case EPwSynthMetric::ZeroCrossingRate:
        if (Technical.State.bMeasured)
        {
            OutValue = Technical.ZeroCrossingRate;
            return true;
        }
        return false;

    case EPwSynthMetric::AttackMs:
        if (In.Envelope.State.bMeasured)
        {
            OutValue = In.Envelope.AttackMs;
            return true;
        }
        return false;

    case EPwSynthMetric::DecayMs:
        if (In.Envelope.State.bMeasured && In.Envelope.DecayMs.IsSet())
        {
            OutValue = In.Envelope.DecayMs.GetValue();
            return true;
        }
        return false;

    case EPwSynthMetric::DcOffset:
        if (Technical.State.bMeasured)
        {
            OutValue = Technical.DcOffset;
            return true;
        }
        return false;

    case EPwSynthMetric::ClippedSamples:
        if (Technical.State.bMeasured)
        {
            OutValue = Technical.ClippedSamples;
            return true;
        }
        return false;

    case EPwSynthMetric::StereoCorrelation:
        if (In.Stereo.State.bMeasured && In.Stereo.Correlation.IsSet())
        {
            OutValue = In.Stereo.Correlation.GetValue();
            return true;
        }
        return false;

    default:
        return false;
    }
}

// =============================================================================================
// SerializeAudioAnalysis
// =============================================================================================
TSharedPtr<FJsonObject> SerializeAudioAnalysis(const FPwAudioAnalysis& In, bool bFullDetail)
{
    using namespace PwAudioAnalysisInternal;

    const TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    const TSharedPtr<FJsonObject> Unmeasured = MakeShared<FJsonObject>();

    // ---- technical --------------------------------------------------------------------------
    if (In.Technical.State.bMeasured)
    {
        const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        SetNum(Object, TEXT("durationMs"), In.Technical.DurationMs, 2);
        Object->SetNumberField(TEXT("sampleRate"), In.Technical.SampleRate);
        Object->SetBoolField(TEXT("digitalSilence"), In.Technical.bDigitalSilence);
        Object->SetNumberField(TEXT("nonFiniteSamples"), In.Technical.NonFiniteSamples);
        SetNum(Object, TEXT("peak"), In.Technical.PeakLinear, 4);
        SetOptNum(Object, TEXT("peakDb"), In.Technical.PeakDb, 2);
        SetOptNum(Object, TEXT("rmsDb"), In.Technical.RmsDb, 2);
        Object->SetNumberField(TEXT("clippedSamples"), In.Technical.ClippedSamples);
        SetNum(Object, TEXT("dcOffset"), In.Technical.DcOffset, 5);
        SetNum(Object, TEXT("zeroCrossingRate"), In.Technical.ZeroCrossingRate, 1);
        SetOptNum(Object, TEXT("activeDurationMs"), In.Technical.ActiveDurationMs, 2);
        SetNum(Object, TEXT("startDiscontinuity"), In.Technical.StartDiscontinuity, 4);
        SetNum(Object, TEXT("endDiscontinuity"), In.Technical.EndDiscontinuity, 4);
        if (bFullDetail)
        {
            Object->SetNumberField(TEXT("frames"), In.Technical.NumFrames);
        }
        Root->SetObjectField(TEXT("technical"), Object);
    }
    else
    {
        Unmeasured->SetStringField(TEXT("technical"), ReasonOr(In.Technical.State));
    }

    // ---- envelope ---------------------------------------------------------------------------
    if (In.Envelope.State.bMeasured)
    {
        const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        SetNum(Object, TEXT("onsetMs"), In.Envelope.OnsetMs, 2);
        SetNum(Object, TEXT("attackMs"), In.Envelope.AttackMs, 2);
        SetOptNum(Object, TEXT("decayMs"), In.Envelope.DecayMs, 2);
        SetOptNum(Object, TEXT("tailMs"), In.Envelope.TailMs, 2);
        SetNum(Object, TEXT("temporalCentroidMs"), In.Envelope.TemporalCentroidMs, 2);
        SetNum(Object, TEXT("crestDb"), In.Envelope.CrestDb, 2);
        if (In.Envelope.TransientCount.IsSet())
        {
            Object->SetNumberField(TEXT("transientCount"), In.Envelope.TransientCount.GetValue());
        }
        else if (!In.Envelope.OnsetReason.IsEmpty())
        {
            Object->SetStringField(TEXT("transientCountUnmeasured"), In.Envelope.OnsetReason);
        }
        if (bFullDetail && In.Envelope.Onsets.Num() > 0)
        {
            const int32 Stride = SeriesStride(In.Envelope.Onsets.Num());
            TArray<TSharedPtr<FJsonValue>> Values;
            for (int32 Index = 0; Index < In.Envelope.Onsets.Num(); Index += Stride)
            {
                const TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                SetNum(Entry, TEXT("ms"), In.Envelope.Onsets[Index].TimeMs, 2);
                SetNum(Entry, TEXT("strength"), In.Envelope.Onsets[Index].Strength, 4);
                Values.Add(MakeShared<FJsonValueObject>(Entry));
            }
            AddSeries(Object, TEXT("onsets"), In.Envelope.Onsets.Num(), Stride, TEXT("stride"),
                MoveTemp(Values));
        }
        Root->SetObjectField(TEXT("envelope"), Object);
    }
    else
    {
        Unmeasured->SetStringField(TEXT("envelope"), ReasonOr(In.Envelope.State));
    }

    // ---- loudness ---------------------------------------------------------------------------
    if (In.Loudness.State.bMeasured)
    {
        const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        SetNum(Object, TEXT("integratedLufs"), In.Loudness.IntegratedLufs, 2);
        SetNum(Object, TEXT("shortTermMaxLufs"), In.Loudness.ShortTermMaxLufs, 2);
        SetNum(Object, TEXT("momentaryMaxLufs"), In.Loudness.MomentaryMaxLufs, 2);
        // Omitted, not zeroed, when the analyzer produced no gated short-term distribution: a
        // reported 0 LU is "steady", and only absence can say "not measured".
        if (In.Loudness.bLoudnessRangeMeasured)
        {
            SetNum(Object, TEXT("loudnessRangeLu"), In.Loudness.LoudnessRangeLu, 2);
        }
        Root->SetObjectField(TEXT("loudness"), Object);
    }
    else
    {
        Unmeasured->SetStringField(TEXT("loudness"), ReasonOr(In.Loudness.State));
    }

    // ---- spectral ---------------------------------------------------------------------------
    if (In.Spectral.State.bMeasured)
    {
        const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        SetNum(Object, TEXT("centroidHz"), In.Spectral.CentroidHz, 1);
        SetNum(Object, TEXT("rolloffHz"), In.Spectral.RolloffHz, 1);
        SetNum(Object, TEXT("flatness"), In.Spectral.Flatness, 4);
        SetNum(Object, TEXT("bandwidthHz"), In.Spectral.BandwidthHz, 1);
        SetOptNum(Object, TEXT("fluxMean"), In.Spectral.FluxMean, 5);
        SetOptNum(Object, TEXT("fluxMax"), In.Spectral.FluxMax, 5);

        if (In.Spectral.Bands.Num() > 0)
        {
            const TSharedPtr<FJsonObject> Bands = MakeShared<FJsonObject>();
            for (const FPwAudioBandRatio& Band : In.Spectral.Bands)
            {
                // The key is formatted from the edges the ratio was measured over, so a band
                // edge cannot drift away from the name it is published under.
                const FString Key = FString::Printf(TEXT("hz%d_%d"),
                    FMath::RoundToInt32(Band.LowHz), FMath::RoundToInt32(Band.HighHz));
                SetNum(Bands, *Key, Band.Ratio, 4);
            }
            Object->SetObjectField(TEXT("bands"), Bands);
        }

        const int32 PeakLimit = bFullDetail
            ? PwAudioAnalysisLimits::MaxDetailPeaks
            : PwAudioAnalysisLimits::MaxSummaryPeaks;
        if (In.Spectral.Peaks.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Peaks;
            for (int32 Index = 0; Index < FMath::Min(PeakLimit, In.Spectral.Peaks.Num()); ++Index)
            {
                const TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                SetNum(Entry, TEXT("hz"), In.Spectral.Peaks[Index].Hz, 1);
                SetNum(Entry, TEXT("db"), In.Spectral.Peaks[Index].Db, 1);
                Peaks.Add(MakeShared<FJsonValueObject>(Entry));
            }
            Object->SetArrayField(TEXT("peaks"), Peaks);
        }

        if (bFullDetail)
        {
            Object->SetNumberField(TEXT("fftSize"), In.Spectral.FftSize);
            Object->SetNumberField(TEXT("hopSize"), In.Spectral.HopSize);
            Object->SetNumberField(TEXT("frames"), In.Spectral.NumFrames);
            SetNum(Object, TEXT("hopMs"), In.Spectral.HopMs, 3);
            SetNum(Object, TEXT("nyquistHz"), In.Spectral.NyquistHz, 1);
            if (In.Spectral.Flux.Num() > 0)
            {
                // Decimated by MAX rather than by stride: flux exists to show where the
                // transients are, and stride sampling drops exactly the frames that carry them.
                const int32 Stride = SeriesStride(In.Spectral.Flux.Num());
                TArray<TSharedPtr<FJsonValue>> Values;
                for (int32 Index = 0; Index < In.Spectral.Flux.Num(); Index += Stride)
                {
                    float Bucket = 0.f;
                    for (int32 Offset = 0; Offset < Stride && Index + Offset < In.Spectral.Flux.Num(); ++Offset)
                    {
                        Bucket = FMath::Max(Bucket, In.Spectral.Flux[Index + Offset]);
                    }
                    Values.Add(MakeShared<FJsonValueNumber>(RoundTo(Bucket, 5)));
                }
                AddSeries(Object, TEXT("flux"), In.Spectral.Flux.Num(), Stride, TEXT("max"),
                    MoveTemp(Values));
            }
        }
        Root->SetObjectField(TEXT("spectral"), Object);
    }
    else
    {
        Unmeasured->SetStringField(TEXT("spectral"), ReasonOr(In.Spectral.State));
    }

    // ---- pitch ------------------------------------------------------------------------------
    if (In.Pitch.State.bMeasured)
    {
        const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        SetNum(Object, TEXT("f0Hz"), In.Pitch.F0Hz, 1);
        SetNum(Object, TEXT("confidence"), In.Pitch.Confidence, 3);
        if (!In.Pitch.Motion.IsEmpty())
        {
            Object->SetStringField(TEXT("motion"), In.Pitch.Motion);
        }
        SetOptNum(Object, TEXT("semitoneDelta"), In.Pitch.SemitoneDelta, 2);
        if (bFullDetail && In.Pitch.Track.Num() > 0)
        {
            const int32 Stride = SeriesStride(In.Pitch.Track.Num());
            TArray<TSharedPtr<FJsonValue>> Values;
            for (int32 Index = 0; Index < In.Pitch.Track.Num(); Index += Stride)
            {
                const TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                SetNum(Entry, TEXT("ms"), In.Pitch.Track[Index].TimeMs, 2);
                SetNum(Entry, TEXT("hz"), In.Pitch.Track[Index].F0Hz, 1);
                SetNum(Entry, TEXT("confidence"), In.Pitch.Track[Index].Confidence, 3);
                Values.Add(MakeShared<FJsonValueObject>(Entry));
            }
            AddSeries(Object, TEXT("track"), In.Pitch.Track.Num(), Stride, TEXT("stride"),
                MoveTemp(Values));
        }
        Root->SetObjectField(TEXT("pitch"), Object);
    }
    else
    {
        Unmeasured->SetStringField(TEXT("pitch"), ReasonOr(In.Pitch.State));
    }

    // ---- stereo -----------------------------------------------------------------------------
    if (In.Stereo.State.bMeasured)
    {
        const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        SetOptNum(Object, TEXT("correlation"), In.Stereo.Correlation, 4);
        SetNum(Object, TEXT("width"), In.Stereo.Width, 4);
        SetNum(Object, TEXT("monoCompatibility"), In.Stereo.MonoCompatibility, 4);
        if (In.Stereo.bOneChannelSilent)
        {
            // Emitted only in the case it describes, and it is also the reason `correlation` is
            // missing: the correlation of a channel with silence is 0/0, not 0.
            Object->SetStringField(TEXT("silentChannel"), In.Stereo.SilentChannel);
        }
        Root->SetObjectField(TEXT("stereo"), Object);
    }
    else
    {
        Unmeasured->SetStringField(TEXT("stereo"), ReasonOr(In.Stereo.State));
    }

    // ---- opt-in context families ------------------------------------------------------------
    // Writes nothing at all - not even an `unmeasured` entry - for a family the caller never
    // requested, so the default summary form is byte-identical to what it was before they existed.
    PwSerializeAudioContext(In, bFullDetail, Root, Unmeasured);

    if (Unmeasured->Values.Num() > 0)
    {
        Root->SetObjectField(TEXT("unmeasured"), Unmeasured);
    }
    return Root;
}
