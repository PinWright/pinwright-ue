// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "AudioGen/PwAudioDecompose.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/ErrorCodes.h"
#include "Math/UnrealMathUtility.h"
#include "PinWrightSubsystem.h"

// The decomposition orchestrator and the report an agent actually reads.
//
// PwDecomposeBuffer sequences the stages the siblings implement; it measures only three things
// itself - the stereo behaviour and the harmonic / percussive / residual energy ratios - and
// otherwise its job is to hand each stage the input it documents and to PROPAGATE that stage's
// own error code and message when it refuses. Flattening a stage refusal into a generic
// "decomposition failed" would delete the one sentence that says what to change.
//
// SerializeDecomposition is the other half of the value: a model that cannot hear learns what a
// sound is made of only from this JSON, so its precision is the product and its SIZE is a hard
// constraint. The wrapped MCP ToolResult carries the payload twice (escaped in content[0].text,
// verbatim in structuredContent), so the real ceiling for a bare result is about 4,250 characters
// rather than the 10,000 the spill threshold suggests (board ticket
// E-spill-threshold-measured-post-wrap). The summary form therefore caps every array, REPORTS
// what it capped, and leaves the uncapped data to bFullDetail.
//
// HONESTY RULES THIS FILE IS BUILT AROUND (docs/rpc-design.md):
//
//   §1  Nothing is published that was not measured. The out-parameter is assembled in locals and
//       assigned only on the one path that finished every stage, so a caller that ignores the
//       returned bool cannot read a half-filled decomposition. An unmeasured decomposition, and
//       an unmeasured FPwStereoBehaviour inside a measured one, are OMITTED rather than emitted
//       as zeros.
//
//       interpretationHints is where §1 is easiest to violate, so it is fenced: the block is
//       named as hints, carries its own `note` saying it is derived rather than observed, and
//       every field in it is a deterministic function of the numbers published above it. It
//       never emits a source label ("glass", "wood", "a door slam") - the analysis measured
//       energy in a spectrogram, not an object, and a caller must be able to tell a measurement
//       from a hypothesis at a glance.
//
//   §6  Signed and paired measurements. Stereo behaviour reports both the opening and the closing
//       correlation rather than a single "width" number, so a narrowing image and a widening one
//       cannot score alike; the hint scores are three named quantities rather than one blended
//       "character" score, so a click and a noise bed are separable.
//
//   §7  Degenerate inputs are ordered empty -> non-finite -> silence -> malformed -> over the cost
//       cap, each answered where it is detected. NON-FINITE BEFORE SILENCE is load-bearing:
//       FMath::Max(0.0, NaN) returns 0 because NaN >= A is false for every A, so a NaN buffer
//       scanned for its peak first measures as digital silence and would be reported as "there
//       was no signal".
//
//   Cost is refused, never trimmed: MaxDurationMs is an error naming the limit and the measured
//   duration, because a decomposition of the first 30 s of a 40 s input, reported as a
//   decomposition of the input, is a measurement of a signal the caller never passed.
//
// ENGINE VERSION: nothing here reaches past CoreMinimal / FMath / TArray / FJsonObject and the
// portable STFT in PwStft.h, so this file owes no row in docs/engine-version-support.md.

// Named (not anonymous) namespace: this module builds with bUseUnity = true, and anonymous
// namespaces in merged TUs are the ODR-collision source the plugin's Build.cs comment warns
// about. The name is distinct from the siblings' PwAudioDecomposeInternal and
// PwAudioDecomposeFitInternal for the same reason.
namespace PwAudioDecomposeReportInternal
{
    // ---- floors shared with the rest of AudioGen --------------------------------------------

    /**
     * Peak absolute sample at or below which a buffer is DIGITAL SILENCE rather than quiet audio.
     * Same 1e-9 as PwStft, PwAudioFeatures and the decomposition stages - deliberately identical,
     * so a buffer this function calls silent is not analysed as signal by the next stage.
     */
    constexpr double SilencePeak = 1e-9;

    /** Floor for reported dB levels, so a pathological value reads as very quiet, not as -inf. */
    constexpr double LevelDbFloor = -200.0;

    // ---- analysis grid ----------------------------------------------------------------------

    /**
     * The module's reference analysis grid: 2048 / 256 at 48 kHz is 23.4 Hz bins on a 5.33 ms
     * frame. Every kernel size and tolerance the decomposition stages document is justified
     * against exactly this geometry (see PwSeparateHarmonicPercussive and PwTrackPartials), so
     * the orchestrator asks for it rather than inventing a second grid.
     */
    constexpr int32 PreferredFftSize = 2048;
    constexpr int32 MinFftSize = 64;
    constexpr int32 HopDivisor = 8;

    /**
     * Whole analysis windows the buffer must hold before the window is used. Four windows at hop
     * = window/8 is 25 frames, which clears PwSeparateHarmonicPercussive's 3-frame minimum and
     * PwDetectOnsets' 2-frame minimum with margin - a decomposition run on 3 frames is dominated
     * by kernel truncation at both edges and would be an artefact of the window, not a
     * measurement of the sound.
     */
    constexpr int32 MinWindowsPerBuffer = 4;

    /**
     * Half-width of the main lobe removed around each tracked partial when the residual ENERGY
     * RATIO is measured, in bins. Identical to PwAnalyzeResidual's PartialMaskHalfBins so the
     * published ratio and the published bands describe the same subtraction; a wider mask here
     * would report a residual fraction the band envelopes contradict.
     */
    constexpr int32 PartialMaskHalfBins = 3;

    // ---- stereo behaviour -------------------------------------------------------------------

    /** Share of the buffer each correlation window covers, at the head and at the tail. */
    constexpr double StereoWindowFraction = 0.25;

    /**
     * Fewest frames a correlation window may hold. A Pearson coefficient over a handful of
     * samples is dominated by whatever phase the window happened to open on; 256 frames is 5.3 ms
     * at 48 kHz, which is a whole analysis window of the grid above.
     */
    constexpr int32 MinStereoWindowFrames = 256;

    /**
     * Drop in correlation from the opening window to the closing one that counts as the image
     * OPENING UP. 0.1 is well outside the spread two windows of the same stationary signal
     * produce, and well inside the change a real reverb tail or a decorrelated decay makes.
     */
    constexpr double StereoWidenDelta = 0.10;

    // ---- report caps (summary form only) ----------------------------------------------------
    //
    // Sized against the ~4,250-character wrapped ceiling with the pretty writer the transport
    // actually uses, and every one of them is REPORTED in the `truncated` block when it bites:
    // five modes returned for a sound that produced forty reads as "this sound has five modes".
    //
    // With all three caps biting, the summary form measures about 2,000 characters - the caps
    // make that figure a CEILING rather than an average, since no array can grow past them. The
    // suite holds a 2,500-character gate on it. Two things dominate the cost and are worth
    // knowing before adding a field: the pretty writer puts every array element on its own line,
    // and it prints doubles with "%.17g", which spells a rounded 0.617 as 0.61699999999999999 -
    // so one number costs about twenty characters, and one array element costs a line.

    constexpr int32 MaxSummaryEvents = 3;
    constexpr int32 MaxSummaryModes = 5;
    constexpr int32 MaxSummaryBands = 3;

    // ---- interpretation hints ---------------------------------------------------------------
    //
    // Every threshold below is a HEURISTIC boundary, not a measurement, which is exactly why they
    // are named here instead of spelled inline: the scores are only comparable across runs if a
    // reader can see the floors they were computed against.

    /**
     * Level below a band's own peak at which that band is no longer "sounding", dB. 10 dB is a
     * halving of loudness - past it the band has stopped carrying the sound, and the duty cycle
     * measured against it separates a burst from a bed.
     */
    constexpr double DutyThresholdDb = 10.0;

    /**
     * Duty cycle at or above which the residual counts as fully SUSTAINED. Half the analysed span
     * spent within 10 dB of the band's peak is a bed, not an event.
     */
    constexpr double SustainedDutyRef = 0.5;

    /**
     * Dynamic range, in dB below the loudest tracked partial, inside which a mode is counted as
     * PROMINENT for the harmonicity and pitch heuristics. 25 dB is chosen against the analysis
     * window rather than against taste: a Hann window's first sidelobe sits 31 dB below its main
     * lobe and is a genuine local maximum, so it is tracked as a partial of its own. A window
     * wider than 31 dB would fold those sidelobes into the harmonic ratio test and, because they
     * flank the real peak on both sides, would move the LOWEST prominent frequency - the value
     * every ratio is measured against.
     */
    constexpr double HarmonicDynamicRangeDb = 25.0;

    /**
     * Fractional frequency difference under which two prominent modes are treated as one. 0.5% is
     * a twelfth of a semitone: no two modes of a real body sit that close, but a single partial
     * whose peak jitters between two bins is routinely tracked twice.
     */
    constexpr double ModeMergeFraction = 0.005;

    /**
     * Mean deviation from an integer multiple, in units of the lowest prominent frequency, that
     * scores harmonicity 0. A set of frequencies with no harmonic relationship at all is uniform
     * over [0, 0.5] in that deviation, i.e. it averages 0.25 - so 0.25 is the "no relationship"
     * anchor rather than a tuned number, and a stretched piano-like series (deviation ~0.03 at
     * the eighth partial) still scores near 0.9.
     */
    constexpr double InharmonicDeviationScale = 0.25;

    /** Harmonicity at or above which the mode set is reported as `harmonic`. */
    constexpr double HarmonicScoreThreshold = 0.6;

    /**
     * Share of the spectrogram's energy the tracked partials must explain before any pitch
     * interpretation is offered. White noise still yields tracks - every local maximum of a noise
     * spectrum is a real local maximum - so without this floor a noise bed would be reported as
     * an inharmonic mode set rather than as unpitched.
     */
    constexpr double PitchedTonalFloor = 0.35;

    /**
     * How far the loudest mode must stand above the MEDIAN residual band peak before the mode set
     * is treated as pitch rather than as peak-picking on a noise floor, dB. Both numbers are dBFS
     * under the STFT convention, so the comparison is like-for-like in units; it is deliberately
     * conservative in the direction that costs the caller least - a bed of noise reported as
     * pitched sends an agent hunting for a fundamental that is not there.
     */
    constexpr double PitchedProminenceDb = 6.0;

    /** Score at or above which a hint counts as "yes" in the likelyStructure decision. */
    constexpr double StructureScoreThreshold = 0.5;

    // ---- small helpers ----------------------------------------------------------------------

    /** dBFS under the STFT magnitude convention, floored rather than allowed to reach -inf. */
    double AmplitudeToDb(double Amplitude)
    {
        if (!(Amplitude > 0.0))
        {
            return LevelDbFloor;
        }
        return FMath::Max(LevelDbFloor, 20.0 * FMath::LogX(10.0, Amplitude));
    }

    double DbToAmplitude(double Db)
    {
        return FMath::Pow(10.0, Db / 20.0);
    }

    /**
     * True when a regression actually ran for this modal row, i.e. when its level and fit error
     * are measurements rather than defaults.
     *
     * PwFitModes keeps one row per input track so the two arrays cannot drift apart, and a track
     * whose amplitude trajectory supported no regression at all leaves InitialGainDb, FitErrorDb
     * and DecayMs at their zeros. A zero in dBFS is FULL SCALE, so publishing that row's level
     * would be the §1 defect in its purest form - a default printed under a measurement's name.
     * The exactly-zero triple is the sentinel: a regression over real points returns an RMS
     * residual of precisely 0.0 only in a measure-zero case.
     */
    bool FitResolved(const FPwModalFit& Fit)
    {
        return Fit.bMeasured
            || Fit.InitialGainDb != 0.0
            || Fit.FitErrorDb != 0.0
            || Fit.DecayMs != 0.0;
    }

    /**
     * Rounds to a stated number of decimals before emission - the report must not publish more
     * precision than it measured, and UE's JSON writer prints doubles with "%.17g", so one
     * unrounded value can cost twenty characters of a budget measured in thousands.
     *
     * Every producer here guards its divisions, so a non-finite value arriving is a bug rather
     * than a data condition; it is logged and emitted as 0, because the alternative is writing
     * "inf" into the payload and handing the caller invalid JSON.
     */
    double RoundTo(double Value, int32 Decimals)
    {
        if (!FMath::IsFinite(Value))
        {
            UE_LOG(LogPinWrightSubsystem, Warning,
                TEXT("PwAudioDecomposeReport: non-finite value reached serialization; emitting 0."));
            return 0.0;
        }
        const double Scale = FMath::Pow(10.0, static_cast<double>(Decimals));
        return FMath::RoundToDouble(Value * Scale) / Scale;
    }

    void SetNum(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, double Value, int32 Decimals)
    {
        Obj->SetNumberField(Key, RoundTo(Value, Decimals));
    }

    /** A frequency span as one field rather than two, which is one pretty-printed line instead of two. */
    FString FormatHzRange(double LowHz, double HighHz)
    {
        return FString::Printf(TEXT("%.0f-%.0f"), LowHz, HighHz);
    }

    /**
     * The analysis frame a time falls on, unrounded so the caller decides what out-of-range
     * means. Inverse of the frame-centre convention every stage in this module reports on:
     * (Frame * HopSize + FftSize / 2) / SampleRate.
     */
    double FrameIndexAtMs(double TimeMs, int32 HopSize, int32 FftSize, int32 SampleRate)
    {
        const double Samples = TimeMs * static_cast<double>(SampleRate) / 1000.0;
        return (Samples - 0.5 * static_cast<double>(FftSize)) / static_cast<double>(HopSize);
    }

    // =========================================================================================
    // Energy ratios
    // =========================================================================================

    /**
     * Harmonic and percussive shares of the separated energy.
     *
     * The denominator is the separated total (sum of both components' powers), not the original
     * spectrogram's power, so the two shares sum to exactly 1 and each is readable as "this
     * fraction of what the separator resolved". The masks are soft and sum to 1 per BIN in
     * amplitude, which does not make the two POWER sums add up to the original's - normalizing
     * against the original would publish two numbers that visibly fail to account for the sound.
     *
     * @return false only for a pair with no energy at all, which PwSeparateHarmonicPercussive's
     *         own silence check already refuses; the guard exists so a 0/0 can never be published
     *         as "0% harmonic, 0% percussive".
     */
    bool MeasureHarmonicPercussiveRatios(const TArray<float>& Harmonic, const TArray<float>& Percussive,
                                         double& OutHarmonicRatio, double& OutPercussiveRatio)
    {
        const int32 Num = FMath::Min(Harmonic.Num(), Percussive.Num());
        double SumH2 = 0.0;
        double SumP2 = 0.0;
        for (int32 Index = 0; Index < Num; ++Index)
        {
            const double H = static_cast<double>(Harmonic[Index]);
            const double P = static_cast<double>(Percussive[Index]);
            SumH2 += H * H;
            SumP2 += P * P;
        }

        const double Total = SumH2 + SumP2;
        if (!(Total > 0.0))
        {
            return false;
        }

        OutHarmonicRatio = FMath::Clamp(SumH2 / Total, 0.0, 1.0);
        OutPercussiveRatio = FMath::Clamp(SumP2 / Total, 0.0, 1.0);
        return true;
    }

    /**
     * Share of the spectrogram's power that the tracked partials do NOT explain.
     *
     * Measured the way PwAnalyzeResidual subtracts, and for the same reason: each sounding
     * partial's main lobe is removed and the noise floor UNDER it is estimated from the nearest
     * unmasked bin on each side, so only the partial's EXCESS over its local floor counts as
     * explained. Zeroing the main lobe outright would be cheaper and wrong in a way that matters
     * here - a spectral peak of a noise bed sits only a few dB above its neighbours, and counting
     * its whole main lobe as "explained by a partial" would report a bed of noise as substantially
     * tonal, which is exactly the misreading the tonal hint must not make.
     *
     * The estimate is a lower bound on the residual in one respect and stated as such: within one
     * main lobe the partial and the noise underneath it are not separable at all, so the floor
     * there is interpolated rather than measured.
     *
     * DC (bin 0) is in neither the total nor the mask, matching the residual bands: it carries
     * whatever offset the source has and it is not audio.
     */
    double MeasureResidualEnergyRatio(const FPwStftResult& Stft, int32 SampleRate,
                                      const TArray<FPwPartialTrack>& Partials)
    {
        const int32 NumFrames = Stft.NumFrames;
        const int32 NumBins = Stft.NumBins;
        if (NumFrames <= 0 || NumBins <= 1 || !(Stft.BinHz > 0.f))
        {
            return 1.0;
        }

        // Bucket the tracked points onto their own analysis frames. The tracker emits one point
        // per matched frame, so this is a re-indexing rather than a resampling - no partial
        // position is invented between the frames it was measured on.
        TArray<TArray<int32>> CentreBinsByFrame;
        CentreBinsByFrame.SetNum(NumFrames);
        const double BinHz = static_cast<double>(Stft.BinHz);
        for (const FPwPartialTrack& Track : Partials)
        {
            for (const FPwPartialPoint& Point : Track.Points)
            {
                if (!(Point.AmpLinear > SilencePeak) || !(Point.FreqHz > 0.0))
                {
                    continue;
                }
                const double FrameExact = FrameIndexAtMs(Point.TimeMs, Stft.HopSize, Stft.FftSize, SampleRate);
                if (!FMath::IsFinite(FrameExact) || FrameExact < -1.0
                    || FrameExact > static_cast<double>(NumFrames))
                {
                    continue;
                }
                const int32 Frame = FMath::RoundToInt32(FrameExact);
                if (Frame < 0 || Frame >= NumFrames)
                {
                    continue;
                }

                // Bounded before the narrowing cast: RoundToInt32 of a value past int32's range
                // is undefined rather than merely out of the array.
                const double CentreExact = Point.FreqHz / BinHz;
                if (CentreExact > static_cast<double>(NumBins + PartialMaskHalfBins))
                {
                    continue;
                }
                CentreBinsByFrame[Frame].Add(FMath::RoundToInt32(CentreExact));
            }
        }

        TArray<uint8> Mask;
        Mask.SetNumZeroed(NumBins);

        double TotalPower = 0.0;
        double ExplainedPower = 0.0;
        for (int32 Frame = 0; Frame < NumFrames; ++Frame)
        {
            const float* FrameMagnitudes = Stft.Magnitudes.GetData() + Frame * NumBins;
            for (int32 Bin = 1; Bin < NumBins; ++Bin)
            {
                const double Magnitude = static_cast<double>(FrameMagnitudes[Bin]);
                TotalPower += Magnitude * Magnitude;
            }

            const TArray<int32>& Centres = CentreBinsByFrame[Frame];
            if (Centres.Num() == 0)
            {
                continue;
            }

            FMemory::Memzero(Mask.GetData(), Mask.Num() * sizeof(uint8));
            for (const int32 Centre : Centres)
            {
                const int32 FirstBin = FMath::Max(1, Centre - PartialMaskHalfBins);
                const int32 LastBin = FMath::Min(NumBins - 1, Centre + PartialMaskHalfBins);
                for (int32 Bin = FirstBin; Bin <= LastBin; ++Bin)
                {
                    Mask[Bin] = 1;
                }
            }

            int32 Bin = 1;
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

                const bool bHasLeft = (Bin - 1) >= 1;
                const bool bHasRight = (RunEnd + 1) <= (NumBins - 1);
                const double Left = bHasLeft ? static_cast<double>(FrameMagnitudes[Bin - 1]) : -1.0;
                const double Right = bHasRight ? static_cast<double>(FrameMagnitudes[RunEnd + 1]) : -1.0;

                for (int32 Index = Bin; Index <= RunEnd; ++Index)
                {
                    double Floor = 0.0;
                    if (bHasLeft && bHasRight)
                    {
                        const double Span = static_cast<double>((RunEnd + 1) - (Bin - 1));
                        const double Alpha = static_cast<double>(Index - (Bin - 1)) / Span;
                        Floor = Left + (Right - Left) * Alpha;
                    }
                    else if (bHasLeft)
                    {
                        Floor = Left;
                    }
                    else if (bHasRight)
                    {
                        Floor = Right;
                    }

                    // The excess is accumulated SIGNED, and the clamp is applied once at the end.
                    // Clamping each bin at zero would rectify the noise: over a bed where the
                    // masked bin and its neighbours are draws from the same distribution, the
                    // per-bin max(0, x - y) has a strictly positive expectation, so a sound with
                    // no partials at all would come back with a fifth of its energy "explained"
                    // by them. The bias is the same asymmetry trap a one-sided measure carries
                    // (rpc-design.md §6).
                    const double Magnitude = static_cast<double>(FrameMagnitudes[Index]);
                    ExplainedPower += Magnitude * Magnitude - Floor * Floor;
                }

                Bin = RunEnd + 1;
            }
        }

        if (!(TotalPower > 0.0))
        {
            // Unreachable through PwDecomposeBuffer (silence is refused before the STFT runs).
            // Answering 1.0 rather than 0.0 keeps the failure direction honest: "nothing was
            // explained" is the claim that cannot flatter the analysis.
            return 1.0;
        }
        return FMath::Clamp(1.0 - ExplainedPower / TotalPower, 0.0, 1.0);
    }

    // =========================================================================================
    // Stereo behaviour
    // =========================================================================================

    /** Pearson correlation of two equal-length windows; false when either window has no variation. */
    bool WindowCorrelation(const TArray<float>& Left, const TArray<float>& Right,
                           int32 FirstFrame, int32 NumFrames, double& OutCorrelation)
    {
        if (NumFrames <= 1)
        {
            return false;
        }

        double SumL = 0.0, SumR = 0.0, SumL2 = 0.0, SumR2 = 0.0, SumLR = 0.0;
        for (int32 Index = FirstFrame; Index < FirstFrame + NumFrames; ++Index)
        {
            const double L = static_cast<double>(Left[Index]);
            const double R = static_cast<double>(Right[Index]);
            SumL += L;
            SumR += R;
            SumL2 += L * L;
            SumR2 += R * R;
            SumLR += L * R;
        }

        const double Inverse = 1.0 / static_cast<double>(NumFrames);
        const double MeanL = SumL * Inverse;
        const double MeanR = SumR * Inverse;
        const double VarianceL = FMath::Max(0.0, SumL2 * Inverse - MeanL * MeanL);
        const double VarianceR = FMath::Max(0.0, SumR2 * Inverse - MeanR * MeanR);
        if (!(VarianceL > 0.0) || !(VarianceR > 0.0))
        {
            return false;
        }

        const double Covariance = SumLR * Inverse - MeanL * MeanR;
        OutCorrelation = FMath::Clamp(Covariance / FMath::Sqrt(VarianceL * VarianceR), -1.0, 1.0);
        return true;
    }

    /**
     * How the stereo image behaves over the sound's lifetime: the L/R correlation over an opening
     * window and over a closing one, and whether the second is meaningfully lower than the first.
     *
     * "The tail widens" is a fact a sound designer acts on - it is the difference between a dry
     * source with a wide reverb and a source that was wide to begin with - and it costs two
     * correlations to measure. A single whole-clip correlation cannot express it at all.
     *
     * Out.bMeasured stays false for a mono, dual-mono or single-channel-silent source: a
     * correlation of 1.0 published for a signal carrying no stereo information would read as a
     * deliberately narrow image rather than as an absence (rpc-design.md §1).
     */
    void MeasureStereoBehaviour(const FPwAudioBuffer& In, FPwStereoBehaviour& Out, FString& OutReason)
    {
        Out = FPwStereoBehaviour();
        OutReason.Reset();

        const int32 NumFrames = In.Left.Num();
        if (NumFrames <= 0 || In.Right.Num() != NumFrames)
        {
            OutReason = TEXT("the buffer carries no second channel");
            return;
        }

        double PeakLeft = 0.0;
        double PeakRight = 0.0;
        double PeakDifference = 0.0;
        for (int32 Index = 0; Index < NumFrames; ++Index)
        {
            const double L = static_cast<double>(In.Left[Index]);
            const double R = static_cast<double>(In.Right[Index]);
            PeakLeft = FMath::Max(PeakLeft, FMath::Abs(L));
            PeakRight = FMath::Max(PeakRight, FMath::Abs(R));
            PeakDifference = FMath::Max(PeakDifference, FMath::Abs(L - R));
        }

        if (!(PeakLeft > SilencePeak) || !(PeakRight > SilencePeak))
        {
            OutReason = TEXT("one channel is digitally silent");
            return;
        }
        if (!(PeakDifference > SilencePeak))
        {
            OutReason = TEXT("both channels carry the same signal (dual mono)");
            return;
        }

        const int32 WindowFrames = FMath::Max(MinStereoWindowFrames,
            FMath::RoundToInt32(StereoWindowFraction * static_cast<double>(NumFrames)));
        if (2 * WindowFrames > NumFrames)
        {
            OutReason = FString::Printf(
                TEXT("the buffer is %d frames, too short for two non-overlapping %d-frame windows"),
                NumFrames, WindowFrames);
            return;
        }

        double InitialCorrelation = 0.0;
        double TailCorrelation = 0.0;
        if (!WindowCorrelation(In.Left, In.Right, 0, WindowFrames, InitialCorrelation)
            || !WindowCorrelation(In.Left, In.Right, NumFrames - WindowFrames, WindowFrames, TailCorrelation))
        {
            OutReason = TEXT("one of the two correlation windows carries no variation in a channel");
            return;
        }

        Out.InitialCorrelation = InitialCorrelation;
        Out.TailCorrelation = TailCorrelation;
        Out.bWidthIncreasesOverTime = (InitialCorrelation - TailCorrelation) > StereoWidenDelta;
        Out.bMeasured = true;
    }

    // =========================================================================================
    // Interpretation hints - HEURISTICS COMPUTED FROM THE MEASUREMENTS ABOVE, NOTHING ELSE
    // =========================================================================================

    /**
     * Duty cycle of one band envelope: the share of its analysed span spent within DutyThresholdDb
     * of its own peak, integrated across the piecewise-linear segments rather than sampled.
     *
     * This is the one temporal shape measure available from FPwDecomposition alone, and it is what
     * separates a burst from a bed: steady noise sits near its peak the whole time (duty ~1) while
     * a click reaches its peak once and collapses to the envelope floor (duty ~0).
     */
    bool MeasureBandDuty(const TArray<FPwDbPoint>& Envelope, double& OutDuty, double& OutPeakDb)
    {
        if (Envelope.Num() == 0)
        {
            return false;
        }

        OutPeakDb = Envelope[0].ValueDb;
        for (const FPwDbPoint& Point : Envelope)
        {
            OutPeakDb = FMath::Max(OutPeakDb, Point.ValueDb);
        }

        const double Span = Envelope.Last().TimeMs - Envelope[0].TimeMs;
        if (Envelope.Num() < 2 || !(Span > 0.0))
        {
            // A single point, or a zero-length span, describes one instant. Reporting it as fully
            // sustained is the direction that claims no attack, which is the conservative one.
            OutDuty = 1.0;
            return true;
        }

        const double Threshold = OutPeakDb - DutyThresholdDb;
        double AboveMs = 0.0;
        for (int32 Index = 0; Index + 1 < Envelope.Num(); ++Index)
        {
            const double StartMs = Envelope[Index].TimeMs;
            const double EndMs = Envelope[Index + 1].TimeMs;
            const double SegmentMs = EndMs - StartMs;
            if (!(SegmentMs > 0.0))
            {
                continue;
            }

            const double StartDb = Envelope[Index].ValueDb;
            const double EndDb = Envelope[Index + 1].ValueDb;
            const bool bStartAbove = StartDb >= Threshold;
            const bool bEndAbove = EndDb >= Threshold;
            if (bStartAbove && bEndAbove)
            {
                AboveMs += SegmentMs;
            }
            else if (bStartAbove != bEndAbove)
            {
                const double Denominator = EndDb - StartDb;
                if (FMath::Abs(Denominator) > UE_DOUBLE_KINDA_SMALL_NUMBER)
                {
                    const double Crossing = FMath::Clamp((Threshold - StartDb) / Denominator, 0.0, 1.0);
                    AboveMs += (bStartAbove ? Crossing : (1.0 - Crossing)) * SegmentMs;
                }
            }
        }

        OutDuty = FMath::Clamp(AboveMs / Span, 0.0, 1.0);
        return true;
    }

    /**
     * Power-weighted duty over every residual band, plus the MEDIAN band peak level.
     *
     * The duty is weighted by band power so the bands sitting at the envelope floor - which are
     * always at duty 1, being flat - cannot outvote the bands actually carrying the sound.
     *
     * The level is the median band peak rather than the loudest, and the difference matters for
     * exactly the material this subsystem generates. An impact's excitation is one broadband
     * instant: it makes the loudest band peak as high as the ringing modes, so a maximum would let
     * a 10 ms burst veto the pitch of a 250 ms ring. A noise bed, by contrast, puts EVERY band at
     * a similar level, so its median is as high as its maximum and it is still gated out.
     */
    bool MeasureResidualShape(const TArray<FPwResidualBand>& Bands, double& OutDuty,
                              double& OutMedianBandPeakDb)
    {
        double WeightSum = 0.0;
        double WeightedDuty = 0.0;
        TArray<double> BandPeaks;
        BandPeaks.Reserve(Bands.Num());

        for (const FPwResidualBand& Band : Bands)
        {
            double Duty = 1.0;
            double PeakDb = LevelDbFloor;
            if (!MeasureBandDuty(Band.EnvelopeDb, Duty, PeakDb))
            {
                continue;
            }
            BandPeaks.Add(PeakDb);

            const double Weight = FMath::Pow(10.0, FMath::Max(PeakDb, LevelDbFloor) / 10.0);
            WeightSum += Weight;
            WeightedDuty += Weight * Duty;
        }

        if (BandPeaks.Num() == 0 || !(WeightSum > 0.0))
        {
            return false;
        }

        BandPeaks.Sort();
        OutDuty = FMath::Clamp(WeightedDuty / WeightSum, 0.0, 1.0);
        OutMedianBandPeakDb = BandPeaks[BandPeaks.Num() / 2];
        return true;
    }

    /** One prominent mode: the fit's frequency plus the level the prominence test measured it at. */
    struct FProminentMode
    {
        double FreqHz = 0.0;
        double TrackPeakLinear = 0.0;
    };

    /**
     * The modes worth reasoning about: real frequencies, within HarmonicDynamicRangeDb of the
     * loudest, deduplicated, sorted ascending in frequency.
     *
     * The LEVEL used for selection and weighting comes from the index-aligned partial track, not
     * from FPwModalFit::InitialGainDb. That is deliberate: a track whose amplitude trajectory
     * supported no fit at all leaves InitialGainDb at its zero, and a zero in dBFS is FULL SCALE -
     * it would become the loudest "mode" in the set and pull every other one out of the dynamic
     * window. FPwPartialTrack::PeakAmpLinear is a measurement on every row.
     */
    TArray<FProminentMode> SelectProminentModes(const FPwDecomposition& In)
    {
        TArray<FProminentMode> Selected;
        const bool bAligned = (In.Partials.Num() == In.Modes.Num());

        // ONE writer for the level of a row, so the selection floor and the weights cannot end up
        // measured on different quantities (rpc-design.md §2). A row that was never fitted has no
        // level at all - it is 0 by default, which is full scale - so it scores 0 and drops out
        // rather than becoming the loudest mode in the set.
        auto LevelOf = [&In, bAligned](int32 Index) -> double
        {
            if (bAligned)
            {
                return FMath::Max(0.0, In.Partials[Index].PeakAmpLinear);
            }
            return FitResolved(In.Modes[Index]) ? DbToAmplitude(In.Modes[Index].InitialGainDb) : 0.0;
        };

        double LoudestLinear = 0.0;
        for (int32 Index = 0; Index < In.Modes.Num(); ++Index)
        {
            if (!(In.Modes[Index].FreqHz > 0.0))
            {
                continue;
            }
            LoudestLinear = FMath::Max(LoudestLinear, LevelOf(Index));
        }
        if (!(LoudestLinear > 0.0))
        {
            return Selected;
        }

        const double FloorLinear = LoudestLinear * DbToAmplitude(-HarmonicDynamicRangeDb);
        for (int32 Index = 0; Index < In.Modes.Num(); ++Index)
        {
            const FPwModalFit& Fit = In.Modes[Index];
            if (!(Fit.FreqHz > 0.0))
            {
                continue;
            }
            const double Linear = LevelOf(Index);
            if (!(Linear >= FloorLinear) || !(Linear > 0.0))
            {
                continue;
            }

            FProminentMode& Mode = Selected.AddDefaulted_GetRef();
            Mode.FreqHz = Fit.FreqHz;
            Mode.TrackPeakLinear = Linear;
        }

        Selected.Sort([](const FProminentMode& A, const FProminentMode& B)
        {
            return A.FreqHz < B.FreqHz;
        });

        // Merge near-duplicates, keeping the louder of each pair: one partial whose peak jitters
        // between two bins is tracked twice, and counting it twice would weight it twice in the
        // harmonic ratio test.
        TArray<FProminentMode> Merged;
        for (const FProminentMode& Mode : Selected)
        {
            if (Merged.Num() > 0
                && FMath::Abs(Mode.FreqHz - Merged.Last().FreqHz) <= ModeMergeFraction * Merged.Last().FreqHz)
            {
                if (Mode.TrackPeakLinear > Merged.Last().TrackPeakLinear)
                {
                    Merged.Last() = Mode;
                }
                continue;
            }
            Merged.Add(Mode);
        }
        return Merged;
    }

    /**
     * How close the prominent mode frequencies sit to integer multiples of the lowest of them.
     *
     * The deviation is measured in units of the lowest frequency (|f/f0 - round(f/f0)|), NOT as a
     * fractional frequency error. That choice is what makes the measure work at high partial
     * numbers: above about the tenth multiple every frequency is within a few percent of SOME
     * integer, so a relative error would score a bell's upper modes as harmonic. In units of f0
     * the deviation of an unrelated frequency stays uniform over [0, 0.5] whatever its number.
     *
     * The lowest prominent mode is the reference because that is what "the fundamental" means when
     * one exists; the reference itself is excluded from the mean, since its own deviation is zero
     * by construction and including it would inflate every score.
     *
     * This separates a bell from a string, and it is all it claims: it says the measured
     * frequencies do or do not stack as a harmonic series. It does not identify a material.
     */
    bool MeasureHarmonicity(const TArray<FProminentMode>& Modes, double& OutHarmonicity)
    {
        if (Modes.Num() < 2 || !(Modes[0].FreqHz > 0.0))
        {
            return false;
        }

        const double Fundamental = Modes[0].FreqHz;
        double WeightSum = 0.0;
        double WeightedDeviation = 0.0;
        for (int32 Index = 1; Index < Modes.Num(); ++Index)
        {
            const double Ratio = Modes[Index].FreqHz / Fundamental;
            const double Nearest = FMath::Max(1.0, FMath::RoundToDouble(Ratio));
            const double Deviation = FMath::Abs(Ratio - Nearest);
            const double Weight = FMath::Max(Modes[Index].TrackPeakLinear, 0.0);
            WeightSum += Weight;
            WeightedDeviation += Weight * Deviation;
        }

        if (!(WeightSum > 0.0))
        {
            return false;
        }

        const double MeanDeviation = WeightedDeviation / WeightSum;
        OutHarmonicity = FMath::Clamp(1.0 - MeanDeviation / InharmonicDeviationScale, 0.0, 1.0);
        return true;
    }

    /**
     * The hint block.
     *
     * EVERY FIELD HERE IS A HEURISTIC DERIVED FROM THE MEASUREMENTS PUBLISHED ABOVE IT, and the
     * serialized block says so in its own `note` (rpc-design.md §1). The three scores are three
     * separate named quantities rather than one blended "character" number precisely so a caller
     * can see which of them is driving a conclusion, and so a click and a noise bed - which any
     * single broadband score assigns the same value - come out different.
     */
    struct FInterpretationHints
    {
        /** Share of the spectrogram's energy the tracked partials explain, 0..1. */
        double Tonal = 0.0;

        /** Unexplained energy that is also SUSTAINED rather than concentrated in an attack, 0..1. */
        double Noisy = 0.0;

        /** A measured attack AND energy concentrated in time, 0..1. Zero without a transient. */
        double Transient = 0.0;

        bool bHarmonicityMeasured = false;
        double Harmonicity = 0.0;

        /** "harmonic" | "inharmonic" | "unpitched". */
        FString PitchInterpretation;

        /** One deterministic sentence about STRUCTURE - never about a source object or material. */
        FString LikelyStructure;
    };

    FInterpretationHints ComputeHints(const FPwDecomposition& In)
    {
        FInterpretationHints Hints;

        const double ResidualRatio = FMath::Clamp(In.ResidualEnergyRatio, 0.0, 1.0);
        Hints.Tonal = FMath::Clamp(1.0 - ResidualRatio, 0.0, 1.0);

        // Sustain defaults to 1 ("fully sustained") when no residual band could be shaped, which
        // is the direction that claims no attack rather than inventing one.
        double Duty = 1.0;
        double MedianBandPeakDb = LevelDbFloor;
        MeasureResidualShape(In.Residual, Duty, MedianBandPeakDb);
        const double Sustain = FMath::Clamp(Duty / SustainedDutyRef, 0.0, 1.0);

        Hints.Noisy = FMath::Clamp(ResidualRatio * Sustain, 0.0, 1.0);
        Hints.Transient = (In.Transients.Num() > 0) ? FMath::Clamp(1.0 - Sustain, 0.0, 1.0) : 0.0;

        const TArray<FProminentMode> Prominent = SelectProminentModes(In);
        Hints.bHarmonicityMeasured = MeasureHarmonicity(Prominent, Hints.Harmonicity);

        // Pitched requires all three: a mode set to reason about, enough of the sound's energy in
        // tracked partials, and modes standing clear of the residual. Peak picking on a noise bed
        // produces a full mode table that satisfies the first condition alone.
        double LoudestModeDb = LevelDbFloor;
        for (const FProminentMode& Mode : Prominent)
        {
            LoudestModeDb = FMath::Max(LoudestModeDb, AmplitudeToDb(Mode.TrackPeakLinear));
        }
        const bool bPitched = Prominent.Num() >= 1
            && Hints.Tonal >= PitchedTonalFloor
            && LoudestModeDb >= MedianBandPeakDb + PitchedProminenceDb;

        if (!bPitched)
        {
            Hints.PitchInterpretation = TEXT("unpitched");
        }
        else if (!Hints.bHarmonicityMeasured)
        {
            // One prominent mode and nothing to compare it against. A lone partial is trivially an
            // integer multiple of itself, so it is reported as harmonic - and `harmonicity` is
            // omitted rather than published as a 1.0 nothing measured.
            Hints.PitchInterpretation = TEXT("harmonic");
        }
        else
        {
            Hints.PitchInterpretation = (Hints.Harmonicity >= HarmonicScoreThreshold)
                ? TEXT("harmonic") : TEXT("inharmonic");
        }

        int32 MeasuredModes = 0;
        for (const FPwModalFit& Fit : In.Modes)
        {
            if (Fit.bMeasured && Fit.DecayMs > 0.0)
            {
                ++MeasuredModes;
            }
        }

        const bool bAttack = Hints.Transient >= StructureScoreThreshold;
        const bool bTonal = Hints.Tonal >= StructureScoreThreshold;
        const bool bNoisy = Hints.Noisy >= StructureScoreThreshold;

        // Deterministic and structural. The vocabulary is limited to what the decomposition
        // measured - an excitation, a resonance, a spectrum, a duration - because "a struck
        // ceramic tile" is a claim about the world that no spectrogram contains.
        if (bAttack && MeasuredModes > 0)
        {
            Hints.LikelyStructure = FString::Printf(TEXT("impact exciting %s resonator"),
                *Hints.PitchInterpretation);
        }
        else if (bAttack)
        {
            Hints.LikelyStructure = TEXT("impulsive noise burst with no measured resonance");
        }
        else if (bTonal && MeasuredModes > 0)
        {
            Hints.LikelyStructure = FString::Printf(TEXT("decaying %s resonator with no measured attack"),
                *Hints.PitchInterpretation);
        }
        else if (bTonal)
        {
            Hints.LikelyStructure = FString::Printf(TEXT("sustained %s tone"), *Hints.PitchInterpretation);
        }
        else if (bNoisy)
        {
            Hints.LikelyStructure = TEXT("sustained broadband noise");
        }
        else
        {
            Hints.LikelyStructure = TEXT("mixed tonal and noisy content");
        }

        return Hints;
    }

    // =========================================================================================
    // Serialization pieces
    // =========================================================================================

    TArray<int32> MakeIndexRange(int32 Num)
    {
        TArray<int32> Indices;
        Indices.Reserve(FMath::Max(0, Num));
        for (int32 Index = 0; Index < Num; ++Index)
        {
            Indices.Add(Index);
        }
        return Indices;
    }

    /** Transients that reach the summary: the loudest, still in ascending time order. */
    TArray<int32> SelectSummaryEvents(const TArray<FPwTransient>& Transients, int32 Limit)
    {
        TArray<int32> Indices = MakeIndexRange(Transients.Num());
        if (Indices.Num() > Limit)
        {
            Indices.Sort([&Transients](int32 A, int32 B)
            {
                return Transients[A].PeakDb > Transients[B].PeakDb;
            });
            Indices.SetNum(Limit);
            Indices.Sort();
        }
        return Indices;
    }

    /** Residual bands that reach the summary: the loudest, still in ascending frequency order. */
    TArray<int32> SelectSummaryBands(const TArray<FPwResidualBand>& Bands, int32 Limit)
    {
        TArray<double> PeakDb;
        PeakDb.Reserve(Bands.Num());
        for (const FPwResidualBand& Band : Bands)
        {
            double Peak = LevelDbFloor;
            for (const FPwDbPoint& Point : Band.EnvelopeDb)
            {
                Peak = FMath::Max(Peak, Point.ValueDb);
            }
            PeakDb.Add(Peak);
        }

        TArray<int32> Indices = MakeIndexRange(Bands.Num());
        if (Indices.Num() > Limit)
        {
            Indices.Sort([&PeakDb](int32 A, int32 B)
            {
                return PeakDb[A] > PeakDb[B];
            });
            Indices.SetNum(Limit);
            Indices.Sort();
        }
        return Indices;
    }

    /**
     * A dB envelope as `ms:dB` pairs on ONE line, ascending in time.
     *
     * Not a JSON array of numbers, and the reason is arithmetic rather than taste. The transport's
     * pretty writer puts every array element on its own line, and it prints doubles with "%.17g",
     * which spells a rounded -42.1 as -42.100000000000001 - so a 19-point envelope costs about
     * 950 characters as an array against 230 as this string, against a whole-response budget of
     * ~4,250. The same argument applies to the event rows below.
     */
    FString EnvelopeToPairs(const TArray<FPwDbPoint>& Points)
    {
        TArray<FString> Pairs;
        Pairs.Reserve(Points.Num());
        for (const FPwDbPoint& Point : Points)
        {
            Pairs.Add(FString::Printf(TEXT("%.0f:%.1f"), Point.TimeMs, Point.ValueDb));
        }
        return FString::Join(Pairs, TEXT(" "));
    }

    /** One residual band: its span, its envelope, and - when thinned - how much was left out. */
    FString FormatResidualBand(const FPwResidualBand& Band, const TArray<FPwDbPoint>& Shown)
    {
        FString Row = FString::Printf(TEXT("%s Hz | %s"),
            *FormatHzRange(Band.LowHz, Band.HighHz), *EnvelopeToPairs(Shown));
        if (Shown.Num() < Band.EnvelopeDb.Num())
        {
            Row += FString::Printf(TEXT(" | %d of %d pts"), Shown.Num(), Band.EnvelopeDb.Num());
        }
        return Row;
    }

    /**
     * One characterised transient as a row. The frequency span is the 5% / 95% cumulative-power
     * pair - percentiles, not the edges of the burst's support, which is why the row calls it a
     * span and reports the bandwidth separately in the full form.
     */
    FString FormatTransient(const FPwTransient& Transient, bool bFullDetail)
    {
        if (bFullDetail)
        {
            return FString::Printf(
                TEXT("%.1f-%.1f ms, peak %.1f ms | %.1f dB | centroid %.0f Hz | bandwidth %.0f Hz | %s Hz"),
                Transient.StartMs, Transient.EndMs, Transient.PeakMs, Transient.PeakDb,
                Transient.CentroidHz, Transient.BandwidthHz,
                *FormatHzRange(Transient.LowHz, Transient.HighHz));
        }
        return FString::Printf(TEXT("peak %.1f ms | %.1f dB | %.1f ms long | centroid %.0f Hz | %s Hz"),
            Transient.PeakMs, Transient.PeakDb, Transient.EndMs - Transient.StartMs,
            Transient.CentroidHz, *FormatHzRange(Transient.LowHz, Transient.HighHz));
    }

    /**
     * The three points of an envelope that describe its arc: where it starts, where it peaks and
     * where it ends. Chosen over a stride because a stride can miss the peak entirely, and the
     * peak is the one point a reader needs to see whether the band bursts or holds. The full
     * simplified envelope is behind bFullDetail, and the row names the count it dropped, so a
     * thinned envelope can never be mistaken for a short one.
     */
    TArray<FPwDbPoint> SummarizeEnvelope(const TArray<FPwDbPoint>& Points)
    {
        TArray<FPwDbPoint> Summary;
        if (Points.Num() == 0)
        {
            return Summary;
        }

        int32 PeakIndex = 0;
        for (int32 Index = 1; Index < Points.Num(); ++Index)
        {
            if (Points[Index].ValueDb > Points[PeakIndex].ValueDb)
            {
                PeakIndex = Index;
            }
        }

        TArray<int32> Chosen;
        Chosen.Add(0);
        if (PeakIndex != 0)
        {
            Chosen.Add(PeakIndex);
        }
        if (Points.Num() - 1 != PeakIndex && Points.Num() - 1 != 0)
        {
            Chosen.Add(Points.Num() - 1);
        }
        Chosen.Sort();

        for (const int32 Index : Chosen)
        {
            Summary.Add(Points[Index]);
        }
        return Summary;
    }

    /**
     * One modal row.
     *
     * `decayMs` is emitted ONLY for a fitted mode. FPwModalFit's invariant is bMeasured == true
     * iff DecayMs > 0; a partial that GREW reports a negative DecayMs and one that held level
     * reports 0, and serializing either as a decay would hand the modal generator a number it
     * rejects (or, worse, one it would render as an audibly wrong answer). Those rows still carry
     * their frequency, level and fit error - real measurements in both branches - plus a `decay`
     * string naming which branch they are. A row that was never fitted carries its frequency and
     * nothing else.
     */
    void SerializeMode(const TSharedPtr<FJsonObject>& Object, const FPwModalFit& Fit)
    {
        SetNum(Object, TEXT("hz"), Fit.FreqHz, 1);
        if (!FitResolved(Fit))
        {
            Object->SetStringField(TEXT("decay"), TEXT("unfitted"));
            return;
        }

        SetNum(Object, TEXT("db"), Fit.InitialGainDb, 1);
        if (Fit.bMeasured && Fit.DecayMs > 0.0)
        {
            SetNum(Object, TEXT("decayMs"), Fit.DecayMs, 1);
        }
        else
        {
            Object->SetStringField(TEXT("decay"),
                Fit.DecayMs < 0.0 ? TEXT("growing") : TEXT("sustained"));
        }
        SetNum(Object, TEXT("fitErrorDb"), Fit.FitErrorDb, 2);
    }
}

// =============================================================================================
// PwDecomposeBuffer
// =============================================================================================

bool PwDecomposeBuffer(const FPwAudioBuffer& In, const FPwDecomposeSettings& Settings,
                       FPwDecomposition& Out, FString& OutErrorCode, FString& OutError)
{
    using namespace PwAudioDecomposeReportInternal;

    // Failure is the default. Everything below is assembled into LOCALS and moved into Out on the
    // single path that finished every stage, so a stage failing halfway cannot leave a populated
    // Transients array beside bMeasured=false (rpc-design.md §1, §2).
    Out = FPwDecomposition();
    OutErrorCode.Reset();
    OutError.Reset();

    // The only two refusal paths, so a refusal cannot reach the caller without also naming itself
    // on the decomposition (rpc-design.md §2). UnmeasuredReason is what SerializeDecomposition
    // publishes and what PwCompareBuffers reads back, so leaving it empty would turn a specific
    // refusal into "not measured" one layer up.
    auto Refuse = [&Out, &OutErrorCode, &OutError](const TCHAR* Code, FString&& Message) -> bool
    {
        Out.UnmeasuredReason = Message;
        OutErrorCode = Code;
        OutError = MoveTemp(Message);
        return false;
    };

    // A stage's own code and message are forwarded unchanged behind a locator naming the stage.
    // Flattening them into one generic decomposition error would delete the sentence that says
    // what to change (rpc-design.md §7).
    auto FailStage = [&Out, &OutErrorCode, &OutError](const TCHAR* Stage, const FString& Code,
                                                      const FString& Message) -> bool
    {
        OutErrorCode = Code;
        OutError = FString::Printf(TEXT("Decompose(%s): %s"), Stage, *Message);
        Out.UnmeasuredReason = OutError;
        return false;
    };

    // -----------------------------------------------------------------------------------------
    // Degenerate cases, in the fixed order of PwAudioDecompose.h. Each is ANSWERED here rather
    // than left to fall through into a later threshold test (rpc-design.md §7).
    // -----------------------------------------------------------------------------------------
    const int32 NumSamples = In.Left.Num();
    if (NumSamples <= 0)
    {
        return Refuse(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
            TEXT("Decompose: the buffer holds 0 frames; there is nothing to take apart. This is ")
            TEXT("not the same as a silent render - a silent buffer of the right length is ")
            TEXT("refused as silence, which names a different repair."));
    }

    // NON-FINITE BEFORE SILENCE, and the order is load-bearing rather than stylistic: NaN
    // compares false against every threshold, so FMath::Max(0.0, NaN) is 0 and a NaN-filled
    // buffer scanned for its peak first measures as digital silence.
    int32 NonFinite = 0;
    int32 FirstNonFiniteFrame = INDEX_NONE;
    const TCHAR* FirstNonFiniteChannel = TEXT("");
    for (int32 Index = 0; Index < NumSamples; ++Index)
    {
        if (!FMath::IsFinite(In.Left[Index]))
        {
            ++NonFinite;
            if (FirstNonFiniteFrame == INDEX_NONE)
            {
                FirstNonFiniteFrame = Index;
                FirstNonFiniteChannel = TEXT("left");
            }
        }
    }
    for (int32 Index = 0; Index < In.Right.Num(); ++Index)
    {
        if (!FMath::IsFinite(In.Right[Index]))
        {
            ++NonFinite;
            if (FirstNonFiniteFrame == INDEX_NONE)
            {
                FirstNonFiniteFrame = Index;
                FirstNonFiniteChannel = TEXT("right");
            }
        }
    }
    if (NonFinite > 0)
    {
        return Refuse(ErrorCodes::ERR_AUDIO_NON_FINITE_SAMPLES,
            FString::Printf(TEXT("Decompose: the buffer holds %d non-finite samples (NaN or ")
                TEXT("infinity), first at frame %d of the %s channel. One of them poisons every ")
                TEXT("FFT bin, every median and every energy sum downstream, and because NaN ")
                TEXT("compares false against every threshold it would otherwise be reported as ")
                TEXT("digital silence."),
                NonFinite, FirstNonFiniteFrame, FirstNonFiniteChannel));
    }

    double PeakAbs = 0.0;
    for (int32 Index = 0; Index < NumSamples; ++Index)
    {
        PeakAbs = FMath::Max(PeakAbs, FMath::Abs(static_cast<double>(In.Left[Index])));
    }
    for (int32 Index = 0; Index < In.Right.Num(); ++Index)
    {
        PeakAbs = FMath::Max(PeakAbs, FMath::Abs(static_cast<double>(In.Right[Index])));
    }
    if (!(PeakAbs > SilencePeak))
    {
        return Refuse(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
            FString::Printf(TEXT("Decompose: the buffer holds %d frames but its peak absolute ")
                TEXT("amplitude is %.3e, below the %.0e silence floor (about -180 dBFS): the ")
                TEXT("signal is digital silence, not quiet audio."),
                NumSamples, PeakAbs, SilencePeak));
    }

    if (In.Right.Num() != NumSamples)
    {
        return Refuse(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Decompose: the channel lengths disagree - left has %d frames, ")
                TEXT("right has %d. FPwAudioBuffer's invariant is that both channels are the same ")
                TEXT("length; a mono source duplicates its channel into both sides."),
                NumSamples, In.Right.Num()));
    }
    if (In.SampleRate <= 0)
    {
        return Refuse(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Decompose: the buffer's sample rate is %d; every frequency and ")
                TEXT("every decay time in the decomposition is derived from it."), In.SampleRate));
    }

    // The caller's own limit is validated before the input is measured against it, so a caller who
    // passed a nonsensical limit is told that rather than told their audio is too long.
    if (Settings.MaxDurationMs <= 0)
    {
        return Refuse(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Decompose: maxDurationMs is %d; it must be positive."),
                Settings.MaxDurationMs));
    }

    const double DurationMs = 1000.0 * static_cast<double>(NumSamples) / static_cast<double>(In.SampleRate);
    if (DurationMs > static_cast<double>(Settings.MaxDurationMs))
    {
        // REFUSED, not trimmed. The median-filter pass in PwSeparateHarmonicPercussive is the
        // expensive stage and this is the limit that bounds it; cropping the input and reporting
        // the result would be a decomposition of a signal the caller never passed.
        return Refuse(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Decompose: the buffer is %.1f ms, over the %d ms maxDurationMs ")
                TEXT("limit. Analyse a shorter excerpt or raise maxDurationMs - the input is not ")
                TEXT("cropped, because a decomposition of the first %d ms reported as a ")
                TEXT("decomposition of the whole sound would be a measurement of a signal you did ")
                TEXT("not pass."),
                DurationMs, Settings.MaxDurationMs, Settings.MaxDurationMs));
    }

    // -----------------------------------------------------------------------------------------
    // Mono collapse. Same 0.5 * (L + R) as PwAnalyzeBuffer, deliberately: the two reports of one
    // sound must not disagree about what its mono signal is. The stereo BEHAVIOUR is measured
    // separately, off the two channels, further down.
    // -----------------------------------------------------------------------------------------
    TArray<float> Mono;
    Mono.SetNumUninitialized(NumSamples);
    for (int32 Index = 0; Index < NumSamples; ++Index)
    {
        Mono[Index] = static_cast<float>(
            0.5 * (static_cast<double>(In.Left[Index]) + static_cast<double>(In.Right[Index])));
    }

    // The window shrinks for a short buffer rather than failing outright - a 30 ms impact is
    // exactly the material this subsystem generates - but it never shrinks below the point where
    // the frame count stops supporting the stages.
    int32 FftSize = PreferredFftSize;
    while (FftSize > MinFftSize && NumSamples < MinWindowsPerBuffer * FftSize)
    {
        FftSize >>= 1;
    }
    if (NumSamples < MinWindowsPerBuffer * FftSize)
    {
        return Refuse(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Decompose: the buffer is %d samples (%.2f ms), under the %d ")
                TEXT("samples the decomposition needs - %d analysis windows of the smallest ")
                TEXT("%d-sample size. Below that the harmonic/percussive split has no time axis ")
                TEXT("to call anything horizontal, so its answer would be an artefact of the ")
                TEXT("window rather than a measurement of the sound."),
                NumSamples, DurationMs, MinWindowsPerBuffer * MinFftSize,
                MinWindowsPerBuffer, MinFftSize));
    }

    FPwStftSettings StftSettings;
    StftSettings.FftSize = FftSize;
    StftSettings.HopSize = FMath::Max(1, FftSize / HopDivisor);

    FPwStftResult Stft;
    {
        FPwStftError StftError;
        if (!PwComputeStft(Mono, In.SampleRate, StftSettings, Stft, &StftError))
        {
            return FailStage(TEXT("stft"), StftError.Code, StftError.Message);
        }
    }

    FString StageCode;
    FString StageMessage;

    TArray<FPwOnset> Onsets;
    if (!PwDetectOnsets(Stft, In.SampleRate, Onsets, StageCode, StageMessage))
    {
        return FailStage(TEXT("onsets"), StageCode, StageMessage);
    }

    TArray<float> Harmonic;
    TArray<float> Percussive;
    if (!PwSeparateHarmonicPercussive(Stft, Harmonic, Percussive, StageCode, StageMessage))
    {
        return FailStage(TEXT("harmonicPercussive"), StageCode, StageMessage);
    }

    double HarmonicRatio = 0.0;
    double PercussiveRatio = 0.0;
    if (!MeasureHarmonicPercussiveRatios(Harmonic, Percussive, HarmonicRatio, PercussiveRatio))
    {
        return Refuse(ErrorCodes::ERR_INTERNAL_ERROR,
            TEXT("Decompose: the harmonic/percussive split returned two components with no energy ")
            TEXT("between them, from a spectrogram that passed its own silence check. No ratio is ")
            TEXT("published rather than a 0/0 reported as '0% harmonic'."));
    }

    TArray<FPwTransient> Transients;
    if (!PwDetectTransients(Stft, In.SampleRate, Onsets, Transients, StageCode, StageMessage))
    {
        return FailStage(TEXT("transients"), StageCode, StageMessage);
    }

    // PwTrackPartials reports truncation as a NOTE: a true return with an EMPTY error code and a
    // message naming the counts. Forwarded on the same terms, because "64 tracks kept of 300" and
    // "this sound has 64 partials" are different claims (rpc-design.md §1).
    TArray<FPwPartialTrack> Partials;
    if (!PwTrackPartials(Stft, In.SampleRate, Settings, Partials, StageCode, StageMessage))
    {
        return FailStage(TEXT("partials"), StageCode, StageMessage);
    }
    const FString TruncationNote = StageCode.IsEmpty() ? StageMessage : FString();

    // PwFitModes refuses an empty partial set, and rightly: "no partials were supplied" is not a
    // bank of zero modes. For a noise burst that legitimately produced none, that refusal is not
    // the decomposition's failure, so the stage is SKIPPED and the empty modes array stands as the
    // honest answer - index-aligned with an equally empty partials array.
    TArray<FPwModalFit> Modes;
    if (Partials.Num() > 0)
    {
        if (!PwFitModes(Partials, In.SampleRate, Modes, StageCode, StageMessage))
        {
            return FailStage(TEXT("modes"), StageCode, StageMessage);
        }
    }

    TArray<FPwResidualBand> Residual;
    if (!PwAnalyzeResidual(Stft, Partials, In.SampleRate, Settings, Residual, StageCode, StageMessage))
    {
        return FailStage(TEXT("residual"), StageCode, StageMessage);
    }

    const double ResidualRatio = MeasureResidualEnergyRatio(Stft, In.SampleRate, Partials);

    FPwStereoBehaviour Stereo;
    FString StereoReason;
    MeasureStereoBehaviour(In, Stereo, StereoReason);
    if (!Stereo.bMeasured && !StereoReason.IsEmpty())
    {
        UE_LOG(LogPinWrightSubsystem, Verbose,
            TEXT("PwDecomposeBuffer: stereo behaviour not measured - %s."), *StereoReason);
    }

    // -----------------------------------------------------------------------------------------
    // Every stage the input supported completed. Only now does anything reach Out.
    // -----------------------------------------------------------------------------------------
    Out.DurationMs = DurationMs;
    Out.SampleRate = In.SampleRate;
    Out.Transients = MoveTemp(Transients);
    Out.Partials = MoveTemp(Partials);
    Out.Modes = MoveTemp(Modes);
    Out.Residual = MoveTemp(Residual);
    Out.Stereo = Stereo;
    Out.HarmonicEnergyRatio = HarmonicRatio;
    Out.PercussiveEnergyRatio = PercussiveRatio;
    Out.ResidualEnergyRatio = ResidualRatio;
    Out.bMeasured = true;

    OutError = TruncationNote;
    return true;
}

// =============================================================================================
// SerializeDecomposition
// =============================================================================================

TSharedPtr<FJsonObject> SerializeDecomposition(const FPwDecomposition& In, bool bFullDetail)
{
    using namespace PwAudioDecomposeReportInternal;

    const TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    if (!In.bMeasured)
    {
        // Absent, not empty-and-confident. An empty events array beside a measured decomposition
        // is the claim "this sound has no transients"; the same array under an unmeasured one
        // would be the claim "the analysis found none", which nothing established.
        const TSharedPtr<FJsonObject> Unmeasured = MakeShared<FJsonObject>();
        Unmeasured->SetStringField(TEXT("decomposition"),
            In.UnmeasuredReason.IsEmpty() ? FString(TEXT("not measured")) : In.UnmeasuredReason);
        Root->SetObjectField(TEXT("unmeasured"), Unmeasured);
        return Root;
    }

    const TSharedPtr<FJsonObject> Truncated = MakeShared<FJsonObject>();
    const TSharedPtr<FJsonObject> Unmeasured = MakeShared<FJsonObject>();

    // ---- top-level facts --------------------------------------------------------------------
    SetNum(Root, TEXT("durationMs"), In.DurationMs, 1);
    Root->SetNumberField(TEXT("sampleRate"), In.SampleRate);

    // Channel count is published only when the stereo stage measured two independent channels.
    // The decomposition itself runs on the mono collapse, so anything else would be an assumption
    // rather than a measurement.
    if (In.Stereo.bMeasured)
    {
        Root->SetNumberField(TEXT("channels"), 2);
    }

    // The loudest level anywhere in the decomposition, dBFS under the STFT magnitude convention -
    // the maximum over the characterised transients' peak bins and the tracked partials' peak
    // amplitudes, both of which are measurements on every row. Named for what it is: it is NOT
    // the peak sample of the source, which this struct does not carry.
    {
        double LoudestDb = LevelDbFloor;
        bool bAnyLevel = false;
        for (const FPwTransient& Transient : In.Transients)
        {
            LoudestDb = FMath::Max(LoudestDb, Transient.PeakDb);
            bAnyLevel = true;
        }
        for (const FPwPartialTrack& Track : In.Partials)
        {
            if (Track.PeakAmpLinear > 0.0)
            {
                LoudestDb = FMath::Max(LoudestDb, AmplitudeToDb(Track.PeakAmpLinear));
                bAnyLevel = true;
            }
        }
        if (bAnyLevel)
        {
            SetNum(Root, TEXT("loudestComponentDb"), LoudestDb, 1);
        }
    }

    {
        const TSharedPtr<FJsonObject> Energy = MakeShared<FJsonObject>();
        SetNum(Energy, TEXT("harmonic"), In.HarmonicEnergyRatio, 3);
        SetNum(Energy, TEXT("percussive"), In.PercussiveEnergyRatio, 3);
        SetNum(Energy, TEXT("residual"), In.ResidualEnergyRatio, 3);
        Root->SetObjectField(TEXT("energy"), Energy);
    }

    // ---- events -----------------------------------------------------------------------------
    {
        const TArray<int32> Chosen = bFullDetail
            ? MakeIndexRange(In.Transients.Num())
            : SelectSummaryEvents(In.Transients, MaxSummaryEvents);

        TArray<TSharedPtr<FJsonValue>> Events;
        Events.Reserve(Chosen.Num());
        for (const int32 Index : Chosen)
        {
            Events.Add(MakeShared<FJsonValueString>(
                FormatTransient(In.Transients[Index], bFullDetail)));
        }
        Root->SetArrayField(TEXT("events"), Events);
        if (Chosen.Num() < In.Transients.Num())
        {
            Truncated->SetStringField(TEXT("events"),
                FString::Printf(TEXT("loudest %d of %d"), Chosen.Num(), In.Transients.Num()));
        }
    }

    // ---- modes ------------------------------------------------------------------------------
    // Loudest first: the order PwTrackPartials sorted the tracks into, which PwFitModes preserves
    // by staying index-aligned with them. The cap is applied on that order, so the rows dropped
    // are always the quietest.
    {
        const int32 Limit = bFullDetail ? In.Modes.Num() : FMath::Min(MaxSummaryModes, In.Modes.Num());
        TArray<TSharedPtr<FJsonValue>> ModeValues;
        ModeValues.Reserve(Limit);
        for (int32 Index = 0; Index < Limit; ++Index)
        {
            const TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            SerializeMode(Entry, In.Modes[Index]);
            if (bFullDetail && In.Partials.Num() == In.Modes.Num())
            {
                const FPwPartialTrack& Track = In.Partials[Index];
                SetNum(Entry, TEXT("startMs"), Track.StartMs, 1);
                SetNum(Entry, TEXT("endMs"), Track.EndMs, 1);
                // SIGNED, and signed deliberately: a magnitude cannot tell a rising sweep from a
                // falling one, and the two ask for opposite pitch envelopes (rpc-design.md §6).
                SetNum(Entry, TEXT("freqDriftHz"), Track.FreqDriftHz, 1);
                Entry->SetNumberField(TEXT("points"), Track.Points.Num());
            }
            ModeValues.Add(MakeShared<FJsonValueObject>(Entry));
        }
        Root->SetArrayField(TEXT("modes"), ModeValues);
        if (Limit < In.Modes.Num())
        {
            Truncated->SetStringField(TEXT("modes"),
                FString::Printf(TEXT("loudest %d of %d"), Limit, In.Modes.Num()));
        }
    }

    // ---- residual ---------------------------------------------------------------------------
    {
        const TArray<int32> Chosen = bFullDetail
            ? MakeIndexRange(In.Residual.Num())
            : SelectSummaryBands(In.Residual, MaxSummaryBands);

        bool bThinned = false;
        TArray<TSharedPtr<FJsonValue>> Bands;
        Bands.Reserve(Chosen.Num());
        for (const int32 Index : Chosen)
        {
            const FPwResidualBand& Band = In.Residual[Index];
            const TArray<FPwDbPoint> Points = bFullDetail
                ? Band.EnvelopeDb
                : SummarizeEnvelope(Band.EnvelopeDb);
            bThinned = bThinned || (Points.Num() < Band.EnvelopeDb.Num());
            Bands.Add(MakeShared<FJsonValueString>(FormatResidualBand(Band, Points)));
        }
        Root->SetArrayField(TEXT("residual"), Bands);
        if (Chosen.Num() < In.Residual.Num())
        {
            Truncated->SetStringField(TEXT("residualBands"),
                FString::Printf(TEXT("loudest %d of %d"), Chosen.Num(), In.Residual.Num()));
        }
        if (bThinned)
        {
            Truncated->SetStringField(TEXT("residualEnvelopes"),
                TEXT("start, loudest and end point of each"));
        }
    }

    // ---- stereo -----------------------------------------------------------------------------
    if (In.Stereo.bMeasured)
    {
        const TSharedPtr<FJsonObject> Stereo = MakeShared<FJsonObject>();
        SetNum(Stereo, TEXT("initialCorrelation"), In.Stereo.InitialCorrelation, 3);
        SetNum(Stereo, TEXT("tailCorrelation"), In.Stereo.TailCorrelation, 3);
        Stereo->SetBoolField(TEXT("widthIncreasesOverTime"), In.Stereo.bWidthIncreasesOverTime);
        Root->SetObjectField(TEXT("stereo"), Stereo);
    }
    else
    {
        // Named as absent rather than emitted as a correlation of 1.0, which would read as a
        // deliberately narrow image instead of as no stereo information at all.
        Unmeasured->SetStringField(TEXT("stereo"),
            TEXT("mono, dual mono, or too short for two correlation windows"));
    }

    // ---- interpretation hints ---------------------------------------------------------------
    // Fenced in their own block, named as hints, and carrying their own disclaimer, because this
    // is the one place in the report where a heuristic could be mistaken for a measurement.
    {
        const FInterpretationHints Hints = ComputeHints(In);
        const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("note"),
            TEXT("heuristics derived from the measurements above; not claims about the source"));
        SetNum(Object, TEXT("tonal"), Hints.Tonal, 2);
        SetNum(Object, TEXT("noisy"), Hints.Noisy, 2);
        SetNum(Object, TEXT("transient"), Hints.Transient, 2);
        if (Hints.bHarmonicityMeasured)
        {
            SetNum(Object, TEXT("harmonicity"), Hints.Harmonicity, 2);
        }
        Object->SetStringField(TEXT("pitchInterpretation"), Hints.PitchInterpretation);
        Object->SetStringField(TEXT("likelyStructure"), Hints.LikelyStructure);
        Root->SetObjectField(TEXT("interpretationHints"), Object);
    }

    if (Truncated->Values.Num() > 0)
    {
        Root->SetObjectField(TEXT("truncated"), Truncated);
    }
    if (Unmeasured->Values.Num() > 0)
    {
        Root->SetObjectField(TEXT("unmeasured"), Unmeasured);
    }
    return Root;
}
