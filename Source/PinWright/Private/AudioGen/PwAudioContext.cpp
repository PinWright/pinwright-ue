// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "AudioGen/PwAudioContext.h"

#include "AudioGen/PwAudioAnalysis.h"
#include "AudioGen/PwAudioBuffer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/ErrorCodes.h"
#include "Math/UnrealMathUtility.h"
#include "PinWrightSubsystem.h"

#include "DSP/Filter.h"

// Named (not anonymous) namespace with a file-specific prefix: the module builds with
// bUseUnity = true, and anonymous namespaces in merged TUs are the ODR-collision source the
// plugin's Build.cs comment warns about. Deliberately distinct from the sibling analysis TUs'
// PwAudioDecomposeInternal / PwAudioDecomposeFitInternal / PwAudioDecomposeReportInternal /
// PwAudioCompareInternal / PwAudioAnalysisInternal.
namespace PwAudioContextInternal
{
    using namespace PwAudioContextLimits;

    // -----------------------------------------------------------------------------------------
    // The aliased thresholds, checked rather than trusted. PwAudioContextLimits cannot include
    // PwAudioAnalysis.h (that header includes this one, for the two optional families), so the
    // constants are spelled twice and the compiler is made to prove they agree - a structural
    // guarantee instead of a comment someone has to honour (rpc-design.md §2).
    // -----------------------------------------------------------------------------------------
    static_assert(LowBandCornerHz == PwAudioAnalysisLimits::BandEdgesHz[1],
        "The retrigger low band must be the report's first band edge, or lowBandDbDelta and "
        "spectral.hz20_150 would describe different regions of the spectrum.");
    static_assert(ContinuousGapDb == -PwAudioAnalysisLimits::EnvelopeTailDb,
        "The gap depth that ends an event must be the same level the report already calls the "
        "boundary between a sound's body and its ring-out.");

    constexpr double NaturalLog2 = 0.69314718055994530942;

    bool Fail(FString& OutErrorCode, FString& OutError, const TCHAR* Code, FString&& Message)
    {
        OutErrorCode = Code;
        OutError = MoveTemp(Message);
        return false;
    }

    /** dBFS with a documented floor rather than -inf. Also catches NaN, which fails `> 0.0`. */
    double LinearToDb(double Value)
    {
        if (!(Value > 0.0))
        {
            return DbFloor;
        }
        return FMath::Max(20.0 * FMath::LogX(10.0, Value), DbFloor);
    }

    double DbToLinear(double Db)
    {
        return FMath::Pow(10.0, Db / 20.0);
    }

    /**
     * Rounds to a stated number of decimals before emission - the report must not publish more
     * precision than it measured, and UE's JSON writer prints doubles with "%.17g". Every
     * producer here guards its divisions, so a non-finite value arriving is a bug rather than a
     * data condition; it is logged and emitted as 0 because the alternative is writing "inf" into
     * the payload and handing the caller invalid JSON.
     */
    double RoundTo(double Value, int32 Decimals)
    {
        if (!FMath::IsFinite(Value))
        {
            UE_LOG(LogPinWrightSubsystem, Warning,
                TEXT("PwAudioContext: non-finite value reached serialization; emitting 0."));
            return 0.0;
        }
        const double Scale = FMath::Pow(10.0, static_cast<double>(Decimals));
        return FMath::RoundToDouble(Value * Scale) / Scale;
    }

    void SetNum(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, double Value, int32 Decimals)
    {
        Obj->SetNumberField(Key, RoundTo(Value, Decimals));
    }

    // -----------------------------------------------------------------------------------------
    // Input validation. Both entry points share it so they cannot disagree about what a
    // degenerate buffer is, and it answers each case rather than falling through (§7).
    // -----------------------------------------------------------------------------------------
    bool ValidateInput(const FPwAudioBuffer& In, FString& OutErrorCode, FString& OutError)
    {
        const int32 NumFrames = In.Left.Num();
        if (NumFrames <= 0)
        {
            return Fail(OutErrorCode, OutError, ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
                TEXT("Buffer holds 0 frames; there is nothing to retrigger or re-render."));
        }
        if (In.Right.Num() != NumFrames)
        {
            return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("Buffer channel lengths disagree: left has %d frames, right ")
                                TEXT("has %d. FPwAudioBuffer's invariant is that both channels are ")
                                TEXT("the same length."),
                    NumFrames, In.Right.Num()));
        }
        if (In.SampleRate <= 0)
        {
            return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("Buffer sample rate is %d; a retrigger interval and every ")
                                TEXT("filter corner need a positive rate."), In.SampleRate));
        }

        // Non-finite BEFORE silence, and the order is load-bearing: NaN compares false against
        // every threshold, so a NaN-filled buffer tested for silence first passes the silence
        // test and is reported as a valid render that happens to be quiet.
        int32 NonFinite = 0;
        int32 FirstFrame = INDEX_NONE;
        const TCHAR* FirstChannel = TEXT("");
        double PeakAbs = 0.0;
        for (int32 Index = 0; Index < NumFrames; ++Index)
        {
            const bool bLeftBad = !FMath::IsFinite(In.Left[Index]);
            const bool bRightBad = !FMath::IsFinite(In.Right[Index]);
            NonFinite += (bLeftBad ? 1 : 0) + (bRightBad ? 1 : 0);
            if ((bLeftBad || bRightBad) && FirstFrame == INDEX_NONE)
            {
                FirstFrame = Index;
                FirstChannel = bLeftBad ? TEXT("left") : TEXT("right");
            }
            if (!bLeftBad)
            {
                PeakAbs = FMath::Max(PeakAbs, FMath::Abs(static_cast<double>(In.Left[Index])));
            }
            if (!bRightBad)
            {
                PeakAbs = FMath::Max(PeakAbs, FMath::Abs(static_cast<double>(In.Right[Index])));
            }
        }
        if (NonFinite > 0)
        {
            return Fail(OutErrorCode, OutError, ErrorCodes::ERR_AUDIO_NON_FINITE_SAMPLES,
                FString::Printf(TEXT("Buffer holds %d non-finite samples (NaN or infinity), first ")
                                TEXT("at frame %d of the %s channel. Summing copies of it or ")
                                TEXT("running it through a biquad spreads the poison across the ")
                                TEXT("whole synthesized signal, so nothing is measured."),
                    NonFinite, FirstFrame, FirstChannel));
        }
        if (PeakAbs < PwAudioAnalysisLimits::SilencePeak)
        {
            // Same spelling PwComputeLoudness uses for the same condition, so an agent sees one
            // code for one fault whichever entry point rejected it.
            return Fail(OutErrorCode, OutError, ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
                TEXT("Buffer is digitally silent. Repeating silence produces silence and filtering ")
                TEXT("it removes nothing, so neither family has anything to measure."));
        }
        return true;
    }

    // -----------------------------------------------------------------------------------------
    // Envelope helpers. Every level here is the TWO-CHANNEL energy, sqrt((L^2 + R^2) / 2), not a
    // mono downmix: an anti-phase stereo signal downmixes to nothing, and measuring its
    // full-range reference on the downmix would make every Mono comparison divide by zero.
    // -----------------------------------------------------------------------------------------
    double BlockLevel(const FPwAudioBuffer& In, int32 Start, int32 Count)
    {
        if (Count <= 0)
        {
            return 0.0;
        }
        double Sum = 0.0;
        const int32 End = FMath::Min(Start + Count, In.NumFrames());
        for (int32 Index = FMath::Max(0, Start); Index < End; ++Index)
        {
            const double Left = In.Left[Index];
            const double Right = In.Right[Index];
            Sum += 0.5 * (Left * Left + Right * Right);
        }
        return FMath::Sqrt(Sum / static_cast<double>(Count));
    }

    /** RMS envelope in fixed-size blocks over [Start, Start + Count). Partial tail block dropped. */
    void BuildEnvelope(const FPwAudioBuffer& In, int32 Start, int32 Count, int32 BlockFrames,
        TArray<double>& Out)
    {
        Out.Reset();
        if (BlockFrames <= 0 || Count <= 0)
        {
            return;
        }
        const int32 NumBlocks = Count / BlockFrames;
        Out.Reserve(NumBlocks);
        for (int32 Block = 0; Block < NumBlocks; ++Block)
        {
            Out.Add(BlockLevel(In, Start + Block * BlockFrames, BlockFrames));
        }
    }

    int32 EnvelopeBlockFrames(int32 SampleRate)
    {
        return FMath::Max(1,
            FMath::RoundToInt32(SampleRate * PwAudioAnalysisLimits::EnvelopeBlockMs / 1000.0));
    }

    /**
     * The steepest amplitude rise in the envelope - the transient. Block 0 is measured against
     * silence rather than against nothing, so a sound that starts at full level on its first
     * sample reports the hard onset it genuinely has instead of a zero attack.
     */
    double LargestEnvelopeRise(const TArray<double>& Envelope)
    {
        double Step = 0.0;
        double Previous = 0.0;
        for (const double Level : Envelope)
        {
            Step = FMath::Max(Step, Level - Previous);
            Previous = Level;
        }
        return Step;
    }

    // -----------------------------------------------------------------------------------------
    // Biquad plumbing.
    // -----------------------------------------------------------------------------------------

    /**
     * Q -> bandwidth in octaves. Audio::FBiquadFilter::Init takes BANDWIDTH, not Q, and the two
     * RBJ-cookbook spellings of the biquad's alpha have to agree:
     *     alpha = sin(w0) * sinh( ln(2)/2 * BW * w0 / sin(w0) )   the bandwidth form the engine
     *                                                             computes (Filter.cpp:289 on 5.8)
     *     alpha = sin(w0) / (2Q)                                  the Q form meant here
     * Equating and solving for BW gives the exact inversion below - the same one PwFxChainA.cpp's
     * PwFxFilter derives at length, duplicated here rather than shared because it lives in that
     * TU's private namespace. Butterworth Q at a corner well below Nyquist comes out at 1.90
     * octaves, the textbook pairing.
     */
    double BandwidthOctavesForQ(double CutoffHz, int32 SampleRate, double Q)
    {
        const double Omega = 2.0 * UE_DOUBLE_PI * CutoffHz / static_cast<double>(SampleRate);
        const double SafeQ = FMath::Max(Q, UE_DOUBLE_KINDA_SMALL_NUMBER);
        const double HalfInvQ = 1.0 / (2.0 * SafeQ);
        const double ArcSinh = FMath::Loge(HalfInvQ + FMath::Sqrt(HalfInvQ * HalfInvQ + 1.0));
        const double SinOmega = FMath::Sin(Omega);
        if (Omega <= 0.0 || SinOmega <= 0.0)
        {
            return 1.0;
        }
        return 2.0 * ArcSinh * SinOmega / (NaturalLog2 * Omega);
    }

    /** True when FBiquadFilter would honour this corner rather than silently clamping it. */
    bool IsRealisableCutoff(double CutoffHz, int32 SampleRate)
    {
        return CutoffHz >= MinFilterCutoffHz
            && CutoffHz <= NyquistFraction * static_cast<double>(SampleRate);
    }

    /**
     * One biquad stage over both planar channels.
     *
     * Init FIRST and never SetParams on a fresh instance: FBiquadFilter's constructor leaves its
     * per-channel coefficient array null and only Init allocates it, so SetParams on an un-Init'd
     * filter dereferences null. Init takes the full parameter set anyway.
     *
     * One single-channel filter run once per channel with Reset() between them, rather than the
     * planar ProcessAudio overload, which is 5.4+ and bit-identical to this for a shared set of
     * coefficients. Processing in place is safe - the engine reads the source sample into a local
     * before writing the destination at the same index.
     */
    void ApplyBiquad(FPwAudioBuffer& InOut, Audio::EBiquadFilter::Type Type, double CutoffHz)
    {
        const int32 NumFrames = InOut.NumFrames();
        if (NumFrames <= 0)
        {
            return;
        }
        const double Bandwidth = BandwidthOctavesForQ(CutoffHz, InOut.SampleRate, FilterQ);

        Audio::FBiquadFilter Filter;
        Filter.Init(static_cast<float>(InOut.SampleRate), /*InNumChannels=*/1, Type,
            static_cast<float>(CutoffHz), static_cast<float>(Bandwidth), /*InGainDB=*/0.f);

        Filter.ProcessAudio(InOut.Left.GetData(), NumFrames, InOut.Left.GetData());
        Filter.Reset();
        Filter.ProcessAudio(InOut.Right.GetData(), NumFrames, InOut.Right.GetData());
    }

    FPwAudioBuffer CopyRange(const FPwAudioBuffer& In, int32 Start, int32 Count)
    {
        FPwAudioBuffer Out;
        Out.SampleRate = In.SampleRate;
        if (Count <= 0)
        {
            return Out;
        }
        Out.SetNumFrames(Count, /*bZeroed=*/false);
        FMemory::Memcpy(Out.Left.GetData(), In.Left.GetData() + Start, Count * sizeof(float));
        FMemory::Memcpy(Out.Right.GetData(), In.Right.GetData() + Start, Count * sizeof(float));
        return Out;
    }

    // -----------------------------------------------------------------------------------------
    // Retrigger internals
    // -----------------------------------------------------------------------------------------

    /**
     * The active region of the input, [Start, End), on the report's own 10 ms block envelope and
     * its own -60 dB relative floor. Leading and trailing silence outside it are dropped before
     * anything is synthesized: a designer retriggering a cue triggers the sound, not the asset's
     * padding, and a window of leading silence compared against a window of audio would report a
     * fabricated infinite buildup.
     */
    void FindActiveRegion(const FPwAudioBuffer& In, int32& OutStart, int32& OutEnd)
    {
        const int32 NumFrames = In.NumFrames();
        OutStart = 0;
        OutEnd = NumFrames;

        const int32 BlockFrames = EnvelopeBlockFrames(In.SampleRate);
        TArray<double> Envelope;
        BuildEnvelope(In, 0, NumFrames, BlockFrames, Envelope);
        if (Envelope.Num() < 2)
        {
            return;
        }

        double PeakLevel = 0.0;
        for (const double Level : Envelope)
        {
            PeakLevel = FMath::Max(PeakLevel, Level);
        }
        if (PeakLevel <= 0.0)
        {
            return;
        }

        const double Floor = PeakLevel * DbToLinear(PwAudioAnalysisLimits::EnvelopeFloorDb);
        int32 FirstBlock = INDEX_NONE;
        int32 LastBlock = INDEX_NONE;
        for (int32 Block = 0; Block < Envelope.Num(); ++Block)
        {
            if (Envelope[Block] >= Floor)
            {
                if (FirstBlock == INDEX_NONE)
                {
                    FirstBlock = Block;
                }
                LastBlock = Block;
            }
        }
        if (FirstBlock == INDEX_NONE)
        {
            return;
        }
        OutStart = FirstBlock * BlockFrames;
        OutEnd = FMath::Min(NumFrames, (LastBlock + 1) * BlockFrames);
    }

    /** What one measurement window says. Peak, RMS and low-band RMS are linear, not dB. */
    struct FWindowMeasure
    {
        double Peak = 0.0;
        double Rms = 0.0;
        double LowBandRms = 0.0;
        int32 Clipped = 0;

        /** Loudest envelope block of the window over its quietest, dB. Large = a real gap. */
        double GapDepthDb = 0.0;
        bool bGapMeasured = false;
    };

    void MeasureWindow(const FPwAudioBuffer& Wide, const FPwAudioBuffer& LowBand,
        int32 Start, int32 Count, FWindowMeasure& Out)
    {
        Out = FWindowMeasure();
        if (Count <= 0)
        {
            return;
        }

        double SumSq = 0.0;
        const int32 End = FMath::Min(Start + Count, Wide.NumFrames());
        for (int32 Index = FMath::Max(0, Start); Index < End; ++Index)
        {
            const double Left = Wide.Left[Index];
            const double Right = Wide.Right[Index];
            SumSq += 0.5 * (Left * Left + Right * Right);
            Out.Peak = FMath::Max(Out.Peak, FMath::Max(FMath::Abs(Left), FMath::Abs(Right)));
            Out.Clipped += (FMath::Abs(Left) >= PwAudioAnalysisLimits::ClipThreshold ? 1 : 0)
                         + (FMath::Abs(Right) >= PwAudioAnalysisLimits::ClipThreshold ? 1 : 0);
        }
        Out.Rms = FMath::Sqrt(SumSq / static_cast<double>(Count));
        Out.LowBandRms = BlockLevel(LowBand, Start, Count);

        // Gap depth on a per-period envelope. GapBlocksPerPeriod rather than the report's fixed
        // 10 ms block, because at 20 Hz a 10 ms block is a fifth of the period being examined.
        const int32 GapBlockFrames = FMath::Max(1, Count / GapBlocksPerPeriod);
        TArray<double> Envelope;
        BuildEnvelope(Wide, Start, Count, GapBlockFrames, Envelope);
        if (Envelope.Num() < 2)
        {
            return;
        }
        double MaxLevel = Envelope[0];
        double MinLevel = Envelope[0];
        for (const double Level : Envelope)
        {
            MaxLevel = FMath::Max(MaxLevel, Level);
            MinLevel = FMath::Min(MinLevel, Level);
        }
        if (MaxLevel <= 0.0)
        {
            return;
        }
        Out.bGapMeasured = true;
        Out.GapDepthDb = (MinLevel <= 0.0) ? -DbFloor : (LinearToDb(MaxLevel) - LinearToDb(MinLevel));
    }

    /** NumRepeats copies of Trimmed at IntervalFrames spacing, long enough to hold a whole period
        after the last onset. */
    void BuildRetriggerBuffer(const FPwAudioBuffer& Trimmed, int32 IntervalFrames, int32 NumRepeats,
        FPwAudioBuffer& Out)
    {
        const int32 ActiveFrames = Trimmed.NumFrames();
        const int32 TotalFrames = (NumRepeats - 1) * IntervalFrames
            + FMath::Max(ActiveFrames, IntervalFrames);

        Out = FPwAudioBuffer();
        Out.SampleRate = Trimmed.SampleRate;
        Out.SetNumFrames(TotalFrames);
        for (int32 Repeat = 0; Repeat < NumRepeats; ++Repeat)
        {
            Trimmed.MixInto(Out, 1.f, Repeat * IntervalFrames);
        }
    }

    /** Two cascaded Butterworth-Q lowpass stages at the low-band corner: -24 dB/octave, steep
        enough that mid content does not leak into a measurement about mud. */
    FPwAudioBuffer MakeLowBand(const FPwAudioBuffer& In)
    {
        FPwAudioBuffer LowBand = CopyRange(In, 0, In.NumFrames());
        ApplyBiquad(LowBand, Audio::EBiquadFilter::Lowpass, LowBandCornerHz);
        ApplyBiquad(LowBand, Audio::EBiquadFilter::Lowpass, LowBandCornerHz);
        return LowBand;
    }

    /** Ascending, with near-duplicates collapsed so a crossing search sees a strict ladder. */
    void SortAndDedupeRates(TArray<double>& Rates)
    {
        Rates.Sort();
        for (int32 Index = Rates.Num() - 1; Index > 0; --Index)
        {
            if (FMath::IsNearlyEqual(Rates[Index], Rates[Index - 1], 1e-9))
            {
                Rates.RemoveAt(Index);
            }
        }
    }

    // -----------------------------------------------------------------------------------------
    // Playback-condition internals
    // -----------------------------------------------------------------------------------------

    /** Everything one rendering contributes, measured identically for every condition. */
    struct FConditionMeasure
    {
        double MeanSquare = 0.0;
        double Peak = 0.0;
        double BlockPeak = 0.0;
        double AttackStep = 0.0;

        /** AttackStep over the rendering's own RMS - the attack's prominence against its body. */
        double AttackProminence = 0.0;
    };

    void MeasureCondition(const FPwAudioBuffer& In, FConditionMeasure& Out)
    {
        Out = FConditionMeasure();
        const int32 NumFrames = In.NumFrames();
        if (NumFrames <= 0)
        {
            return;
        }

        double SumSq = 0.0;
        for (int32 Index = 0; Index < NumFrames; ++Index)
        {
            const double Left = In.Left[Index];
            const double Right = In.Right[Index];
            SumSq += 0.5 * (Left * Left + Right * Right);
            Out.Peak = FMath::Max(Out.Peak, FMath::Max(FMath::Abs(Left), FMath::Abs(Right)));
        }
        Out.MeanSquare = SumSq / static_cast<double>(NumFrames);

        // Block clamped to the buffer so a clip shorter than one 10 ms block still produces one
        // envelope point. Without the clamp it would produce none, and a real sound would be
        // reported as inaudible with no transient purely because it was short.
        const int32 BlockFrames = FMath::Min(EnvelopeBlockFrames(In.SampleRate), NumFrames);
        TArray<double> Envelope;
        BuildEnvelope(In, 0, NumFrames, BlockFrames, Envelope);
        for (const double Level : Envelope)
        {
            Out.BlockPeak = FMath::Max(Out.BlockPeak, Level);
        }
        Out.AttackStep = LargestEnvelopeRise(Envelope);

        const double Rms = FMath::Sqrt(Out.MeanSquare);
        Out.AttackProminence = (Rms > 0.0) ? (Out.AttackStep / Rms) : 0.0;
    }

    /** One channel as its own dual-mono buffer. PwAnalyzeBuffer's 0.5 * (L + R) downmix of the
        result is that channel again, bit for bit, so nothing about the spectral measurement
        changes except which signal it is taken on. */
    FPwAudioBuffer ChannelAsDualMono(const FPwAudioBuffer& In, const TArray<float>& Channel)
    {
        FPwAudioBuffer Out;
        Out.SampleRate = In.SampleRate;
        const int32 NumFrames = Channel.Num();
        if (NumFrames <= 0)
        {
            return Out;
        }
        Out.SetNumFrames(NumFrames, /*bZeroed=*/false);
        FMemory::Memcpy(Out.Left.GetData(), Channel.GetData(), NumFrames * sizeof(float));
        FMemory::Memcpy(Out.Right.GetData(), Channel.GetData(), NumFrames * sizeof(float));
        return Out;
    }

    /**
     * The spectral centroid of a rendering, measured on its two CHANNELS rather than on their sum.
     *
     * PwAnalyzeBuffer computes its spectral family from the 0.5 * (L + R) downmix, and that
     * downmix is exactly zero for the anti-phase pair this family exists to catch. Reading the
     * full-range reference off it would withhold the brightness of a signal that is perfectly
     * healthy on two speakers - and with it every row's delta, since each one is a difference
     * against that reference. So each channel is analysed on its own and the two centroids are
     * combined weighted by channel energy: the same two-channel rule every level in this file
     * already follows (see the envelope helpers above), and for correlated material it is the
     * number the downmix gives, because there each channel IS the downmix.
     *
     * A rendering with no energy in either channel - which is exactly what the mono fold-down of
     * an anti-phase pair is - leaves bOutMeasured false. That absence is the report, not 0 Hz.
     *
     * @return false only when PwAnalyzeBuffer itself failed, in which case OutCode and OutMessage
     *         carry its reason; a rendering with no measurable spectrum returns true.
     */
    bool MeasureCentroidHz(const FPwAudioBuffer& In, bool& bOutMeasured, double& OutCentroidHz,
        FString& OutCode, FString& OutMessage)
    {
        bOutMeasured = false;
        OutCentroidHz = 0.0;

        // Identical channels are the common case here, and re-analysing the same samples cannot
        // return a different centroid. Skipping the second pass is a shortcut over that identity,
        // not a second definition of the measurement.
        const TArray<float>* const Channels[2] = { &In.Left, &In.Right };
        const int32 NumChannelsToAnalyse = (In.Right == In.Left) ? 1 : 2;

        double SumWeight = 0.0;
        double SumWeightedCentroid = 0.0;
        for (int32 Index = 0; Index < NumChannelsToAnalyse; ++Index)
        {
            const TArray<float>& Channel = *Channels[Index];
            double Energy = 0.0;
            for (const float Sample : Channel)
            {
                Energy += static_cast<double>(Sample) * static_cast<double>(Sample);
            }
            if (!(Energy > 0.0))
            {
                continue;
            }

            FPwAudioAnalysis Analysis;
            if (!PwAnalyzeBuffer(ChannelAsDualMono(In, Channel), Analysis, OutCode, OutMessage))
            {
                return false;
            }
            if (!Analysis.Spectral.State.bMeasured)
            {
                continue;
            }
            SumWeightedCentroid += Energy * Analysis.Spectral.CentroidHz;
            SumWeight += Energy;
        }

        if (!(SumWeight > 0.0))
        {
            return true;
        }
        OutCentroidHz = SumWeightedCentroid / SumWeight;
        bOutMeasured = true;
        return true;
    }

    /**
     * Render one condition. These are cheap deterministic approximations of a playback chain, not
     * device models - see PwAudioContextLimits for the corners and the basis they were picked on.
     *
     * The phone lowpass is SKIPPED when its corner sits above what the analysed rate can carry.
     * That is not an approximation of the model, it is the model applied exactly: there is no
     * content above Nyquist for the stage to remove.
     *
     * @return false only when a required highpass corner is unrealisable at this sample rate,
     *         which needs a rate under about 900 Hz.
     */
    bool RenderCondition(const FPwAudioBuffer& In, EPwPlaybackCondition Condition,
        FPwAudioBuffer& Out)
    {
        Out = CopyRange(In, 0, In.NumFrames());
        const int32 SampleRate = In.SampleRate;

        switch (Condition)
        {
        case EPwPlaybackCondition::FullRange:
            return true;

        case EPwPlaybackCondition::LaptopSpeaker:
            if (!IsRealisableCutoff(LaptopHighpassHz, SampleRate))
            {
                return false;
            }
            ApplyBiquad(Out, Audio::EBiquadFilter::Highpass, LaptopHighpassHz);
            ApplyBiquad(Out, Audio::EBiquadFilter::Highpass, LaptopHighpassHz);
            return true;

        case EPwPlaybackCondition::PhoneSpeaker:
            if (!IsRealisableCutoff(PhoneHighpassHz, SampleRate))
            {
                return false;
            }
            ApplyBiquad(Out, Audio::EBiquadFilter::Highpass, PhoneHighpassHz);
            ApplyBiquad(Out, Audio::EBiquadFilter::Highpass, PhoneHighpassHz);
            if (IsRealisableCutoff(PhoneLowpassHz, SampleRate))
            {
                ApplyBiquad(Out, Audio::EBiquadFilter::Lowpass, PhoneLowpassHz);
            }
            return true;

        case EPwPlaybackCondition::Quiet:
        {
            const float Gain = static_cast<float>(DbToLinear(QuietGainDb));
            for (int32 Index = 0; Index < Out.NumFrames(); ++Index)
            {
                Out.Left[Index] *= Gain;
                Out.Right[Index] *= Gain;
            }
            return true;
        }

        case EPwPlaybackCondition::Mono:
            // 0.5 * (L + R), the same fold-down FPwAudioStereo::MonoCompatibility is defined on,
            // so a correlation of -1 there and a near-zero retained energy here are two views of
            // one fact rather than two independent claims.
            for (int32 Index = 0; Index < Out.NumFrames(); ++Index)
            {
                const float Sum = 0.5f * (Out.Left[Index] + Out.Right[Index]);
                Out.Left[Index] = Sum;
                Out.Right[Index] = Sum;
            }
            return true;
        }
        return false;
    }

    constexpr EPwPlaybackCondition AllConditions[] = {
        EPwPlaybackCondition::FullRange,
        EPwPlaybackCondition::LaptopSpeaker,
        EPwPlaybackCondition::PhoneSpeaker,
        EPwPlaybackCondition::Quiet,
        EPwPlaybackCondition::Mono
    };

    /** Cap on the retrigger points the summary form carries, mirroring MaxSummaryPeaks' role. */
    constexpr int32 MaxSummaryRetriggerPoints = 6;
}

// =============================================================================================
// Retrigger
// =============================================================================================

TArray<double> PwGetDefaultRetriggerRates()
{
    return TArray<double>({ 2.0, 5.0, 10.0, 20.0 });
}

bool PwAnalyzeRetrigger(const FPwAudioBuffer& In, const TArray<double>& RatesHz,
                        FPwRetriggerResult& Out, FString& OutErrorCode, FString& OutError)
{
    using namespace PwAudioContextInternal;
    using namespace PwAudioContextLimits;

    // Failure is the default: a caller that ignored the bool cannot read a stale point.
    Out = FPwRetriggerResult();
    OutErrorCode.Reset();
    OutError.Reset();

    if (!ValidateInput(In, OutErrorCode, OutError))
    {
        Out.UnmeasuredReason = OutError;
        return false;
    }

    if (RatesHz.Num() == 0)
    {
        Out.UnmeasuredReason = TEXT("no retrigger rates were requested");
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
            TEXT("No retrigger rates were requested. There is no default that can be assumed here ")
            TEXT("- pass the rates the sound will actually be fired at, or the standard set 2, 5, ")
            TEXT("10 and 20 Hz (menu spam, footsteps, weapon fire, impact spam)."));
    }

    // Rate validation before anything is synthesized, and non-finite before the range test for the
    // same reason non-finite precedes silence: NaN fails every comparison, so a NaN rate would
    // slip through a `Rate < Min || Rate > Max` test as if it were in range.
    for (const double Rate : RatesHz)
    {
        if (!FMath::IsFinite(Rate))
        {
            Out.UnmeasuredReason = TEXT("a requested retrigger rate was not a finite number");
            return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
                TEXT("A requested retrigger rate is not a finite number. Every rate must be a real ")
                TEXT("repetitions-per-second value; a NaN rate would silently pass a range test."));
        }
        if (Rate < MinRetriggerRateHz || Rate > MaxRetriggerRateHz)
        {
            Out.UnmeasuredReason = FString::Printf(
                TEXT("retrigger rate %g Hz is outside the measurable window"), Rate);
            return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("Retrigger rate %g Hz is outside the window [%g, %g] Hz. ")
                    TEXT("Below the floor a repetition is not a repetition; above the ceiling the ")
                    TEXT("repetition rate is itself an audio frequency and this family no longer ")
                    TEXT("answers the question being asked."),
                    Rate, MinRetriggerRateHz, MaxRetriggerRateHz));
        }
    }

    int32 ActiveStart = 0;
    int32 ActiveEnd = In.NumFrames();
    FindActiveRegion(In, ActiveStart, ActiveEnd);
    const int32 ActiveFrames = ActiveEnd - ActiveStart;
    if (ActiveFrames <= 0)
    {
        Out.UnmeasuredReason = TEXT("the buffer has no active region above the envelope floor");
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
            TEXT("The buffer carries samples but no block rises above the envelope floor, so there ")
            TEXT("is no sound to repeat."));
    }
    const FPwAudioBuffer Trimmed = CopyRange(In, ActiveStart, ActiveFrames);

    // Total two-channel energy of one copy, used for the tail-overlap ratio. Guaranteed positive:
    // digital silence was rejected and the active region is above the floor.
    double ActiveEnergy = 0.0;
    for (int32 Index = 0; Index < ActiveFrames; ++Index)
    {
        const double Left = Trimmed.Left[Index];
        const double Right = Trimmed.Right[Index];
        ActiveEnergy += 0.5 * (Left * Left + Right * Right);
    }
    if (!(ActiveEnergy > 0.0))
    {
        Out.UnmeasuredReason = TEXT("the active region carries no energy");
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
            TEXT("The active region carries no energy, so no accumulation can be measured."));
    }

    TArray<double> Rates = RatesHz;
    SortAndDedupeRates(Rates);

    TArray<double> GapDepths;
    GapDepths.Reserve(Rates.Num());
    TArray<bool> GapMeasured;
    GapMeasured.Reserve(Rates.Num());

    for (const double Rate : Rates)
    {
        const int32 IntervalFrames = FMath::Max(1,
            FMath::RoundToInt32(static_cast<double>(In.SampleRate) / Rate));

        // Copies that can be sounding at once once the train has settled. The measurement window
        // is the period starting at the LAST onset, so rendering Overlap + SteadyMarginRepeats
        // copies guarantees every predecessor that can still be sounding is present, with two
        // complete steady periods of lead-in for the low-band biquads to settle in.
        const int32 Overlap = FMath::Max(1, FMath::DivideAndRoundUp(ActiveFrames, IntervalFrames));
        const int32 NumRepeats = Overlap + SteadyMarginRepeats;

        const int64 SampleWrites = static_cast<int64>(NumRepeats) * static_cast<int64>(ActiveFrames);
        const int64 TotalFrames = static_cast<int64>(NumRepeats - 1) * IntervalFrames
            + FMath::Max(ActiveFrames, IntervalFrames);
        if (SampleWrites > MaxSyntheticSampleWrites || TotalFrames > MaxSyntheticSampleWrites)
        {
            Out = FPwRetriggerResult();
            Out.UnmeasuredReason = FString::Printf(
                TEXT("retrigger at %g Hz over this source is past the synthesis cap"), Rate);
            return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("Retrigger at %g Hz over a %.0f ms source needs %d ")
                    TEXT("overlapping copies (%lld sample writes, %lld frames), past the %lld cap. ")
                    TEXT("Reaching steady state is not optional - a truncated build would report a ")
                    TEXT("buildup that never settled - so shorten the source or lower the rate."),
                    Rate, 1000.0 * ActiveFrames / In.SampleRate, NumRepeats,
                    SampleWrites, TotalFrames, MaxSyntheticSampleWrites));
        }

        // The reference is the IDENTICAL pipeline with exactly one copy in it, measured over the
        // same window length at the same offset from an onset. That is what makes a 0.0 dB delta
        // the measured statement "retriggering this changes nothing" (§1) rather than an artefact
        // of comparing two differently shaped windows.
        FPwAudioBuffer Single;
        BuildRetriggerBuffer(Trimmed, IntervalFrames, /*NumRepeats=*/1, Single);
        const FPwAudioBuffer SingleLowBand = MakeLowBand(Single);
        FWindowMeasure Reference;
        MeasureWindow(Single, SingleLowBand, 0, IntervalFrames, Reference);

        FPwAudioBuffer Train;
        BuildRetriggerBuffer(Trimmed, IntervalFrames, NumRepeats, Train);
        const FPwAudioBuffer TrainLowBand = MakeLowBand(Train);
        FWindowMeasure Steady;
        MeasureWindow(Train, TrainLowBand, (NumRepeats - 1) * IntervalFrames, IntervalFrames, Steady);

        FPwRetriggerPoint Point;
        Point.RateHz = Rate;
        Point.PeakDb = LinearToDb(Steady.Peak) - LinearToDb(Reference.Peak);
        Point.RmsDb = LinearToDb(Steady.Rms) - LinearToDb(Reference.Rms);
        Point.LowBandDb = LinearToDb(Steady.LowBandRms) - LinearToDb(Reference.LowBandRms);
        Point.ClippedSamples = Steady.Clipped;

        // Energy arriving more than one interval after the sound's own onset. Derived from the
        // reference window's own RMS rather than from a second scan, so the two cannot disagree
        // about where the first interval ended.
        const double FirstIntervalEnergy = Reference.Rms * Reference.Rms * IntervalFrames;
        Point.TailOverlapRatio = FMath::Clamp(1.0 - FirstIntervalEnergy / ActiveEnergy, 0.0, 1.0);

        Point.bEffectivelyContinuous = Steady.bGapMeasured && Steady.GapDepthDb <= ContinuousGapDb;

        Out.Points.Add(Point);
        GapDepths.Add(Steady.GapDepthDb);
        GapMeasured.Add(Steady.bGapMeasured);
    }

    // Derive the continuity threshold from the measured points. Gap depth falls as the rate rises,
    // so the lowest adjacent pair that straddles ContinuousGapDb brackets the crossing; the rate
    // is interpolated in log frequency, which is the axis rates are perceived on. Extrapolating
    // past the measured range would be a fabricated number, so an unbracketed crossing leaves
    // bContinuousRateMeasured false and the field omitted.
    for (int32 Index = 0; Index + 1 < Out.Points.Num(); ++Index)
    {
        if (!GapMeasured[Index] || !GapMeasured[Index + 1])
        {
            continue;
        }
        if (Out.Points[Index].bEffectivelyContinuous || !Out.Points[Index + 1].bEffectivelyContinuous)
        {
            continue;
        }
        const double Deep = GapDepths[Index];
        const double Shallow = GapDepths[Index + 1];
        const double Span = Deep - Shallow;
        if (!(Span > UE_DOUBLE_KINDA_SMALL_NUMBER))
        {
            continue;
        }
        const double Fraction = FMath::Clamp((Deep - ContinuousGapDb) / Span, 0.0, 1.0);
        const double LowRate = Out.Points[Index].RateHz;
        const double HighRate = Out.Points[Index + 1].RateHz;
        Out.ContinuousAboveRateHz = FMath::Exp(FMath::Loge(LowRate)
            + Fraction * (FMath::Loge(HighRate) - FMath::Loge(LowRate)));
        Out.bContinuousRateMeasured = true;
        break;
    }

    Out.bMeasured = true;
    Out.UnmeasuredReason.Reset();
    return true;
}

// =============================================================================================
// Playback conditions
// =============================================================================================

const TCHAR* PwPlaybackConditionName(EPwPlaybackCondition Condition)
{
    switch (Condition)
    {
    case EPwPlaybackCondition::FullRange:     return TEXT("fullRange");
    case EPwPlaybackCondition::LaptopSpeaker: return TEXT("laptopSpeaker");
    case EPwPlaybackCondition::PhoneSpeaker:  return TEXT("phoneSpeaker");
    case EPwPlaybackCondition::Quiet:         return TEXT("quiet");
    case EPwPlaybackCondition::Mono:          return TEXT("mono");
    }
    return TEXT("unknown");
}

bool PwAnalyzePlaybackConditionSet(const FPwAudioBuffer& In, FPwPlaybackConditionSet& Out,
                                   FString& OutErrorCode, FString& OutError)
{
    using namespace PwAudioContextInternal;
    using namespace PwAudioContextLimits;

    Out = FPwPlaybackConditionSet();
    OutErrorCode.Reset();
    OutError.Reset();

    if (!ValidateInput(In, OutErrorCode, OutError))
    {
        Out.UnmeasuredReason = OutError;
        return false;
    }

    // The full-range reference first: every other row is a difference against it, so a set whose
    // reference could not be measured has nothing to publish.
    FConditionMeasure Full;
    MeasureCondition(In, Full);
    if (!(Full.MeanSquare > 0.0))
    {
        Out.UnmeasuredReason = TEXT("the full-range reference carries no energy");
        return Fail(OutErrorCode, OutError, ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
            TEXT("The full-range reference carries no energy, so no condition can be compared ")
            TEXT("against it."));
    }

    // Centroid through PwAnalyzeBuffer rather than a private copy of the spectral code, so
    // centroidHzDelta is a difference of two numbers that mean what spectral.centroidHz means -
    // taken per channel, because that family's own analysis signal is the downmix a fold-down
    // condition destroys (MeasureCentroidHz states the case).
    FString AnalysisCode;
    FString AnalysisMessage;
    if (!MeasureCentroidHz(In, Out.bReferenceCentroidMeasured, Out.ReferenceCentroidHz,
            AnalysisCode, AnalysisMessage))
    {
        Out.UnmeasuredReason = AnalysisMessage;
        return Fail(OutErrorCode, OutError, *AnalysisCode,
            FString::Printf(TEXT("The full-range reference could not be analysed: %s"),
                *AnalysisMessage));
    }

    Out.ReferencePeakDb = LinearToDb(Full.Peak);
    Out.ReferenceBlockPeakDb = LinearToDb(Full.BlockPeak);

    for (const EPwPlaybackCondition Condition : AllConditions)
    {
        FPwAudioBuffer Rendered;
        if (!RenderCondition(In, Condition, Rendered))
        {
            Out = FPwPlaybackConditionSet();
            Out.UnmeasuredReason = FString::Printf(
                TEXT("the %s condition cannot be rendered at %d Hz"),
                PwPlaybackConditionName(Condition), In.SampleRate);
            return Fail(OutErrorCode, OutError, ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("The %s condition needs a highpass corner Audio::FBiquadFilter ")
                    TEXT("cannot realise at a %d Hz sample rate (its window is [%g Hz, %g x rate]). ")
                    TEXT("The filter would clamp silently and the row would describe a chain nobody ")
                    TEXT("asked for, so the whole set is withheld."),
                    PwPlaybackConditionName(Condition), In.SampleRate,
                    MinFilterCutoffHz, NyquistFraction));
        }

        FConditionMeasure Measure;
        MeasureCondition(Rendered, Measure);

        bool bRenderedCentroidMeasured = false;
        double RenderedCentroidHz = 0.0;
        if (!MeasureCentroidHz(Rendered, bRenderedCentroidMeasured, RenderedCentroidHz,
                AnalysisCode, AnalysisMessage))
        {
            Out = FPwPlaybackConditionSet();
            Out.UnmeasuredReason = AnalysisMessage;
            return Fail(OutErrorCode, OutError, *AnalysisCode,
                FString::Printf(TEXT("The %s rendering could not be analysed: %s"),
                    PwPlaybackConditionName(Condition), *AnalysisMessage));
        }

        FPwConditionResult Result;
        Result.Condition = Condition;
        Result.bMeasured = true;
        Result.RetainedEnergyRatio = Measure.MeanSquare / Full.MeanSquare;
        Result.PeakDbDelta = LinearToDb(Measure.Peak) - Out.ReferencePeakDb;

        Result.bCentroidMeasured = Out.bReferenceCentroidMeasured && bRenderedCentroidMeasured;
        Result.CentroidHzDelta = Result.bCentroidMeasured
            ? (RenderedCentroidHz - Out.ReferenceCentroidHz) : 0.0;

        Result.bAudible = LinearToDb(Measure.BlockPeak) > AudibilityFloorDb;

        // Two stated comparisons, both required. The absolute leg asks whether the attack itself
        // is still above the audibility floor; the relative leg asks whether it is still as
        // prominent against the sound's own body as it was at full range, which is what separates
        // "the whole thing got quieter" from "the attack's frequency content was removed".
        const bool bAttackAboveFloor = LinearToDb(Measure.AttackStep) > AudibilityFloorDb;
        const bool bProminenceRetained = (Full.AttackProminence > 0.0)
            && (Measure.AttackProminence >= TransientRetentionFraction * Full.AttackProminence);
        Result.bTransientSurvives = bAttackAboveFloor && bProminenceRetained;

        Out.Conditions.Add(Result);
    }

    Out.bMeasured = true;
    Out.UnmeasuredReason.Reset();
    return true;
}

bool PwAnalyzePlaybackConditions(const FPwAudioBuffer& In, TArray<FPwConditionResult>& Out,
                                 FString& OutErrorCode, FString& OutError)
{
    Out.Reset();

    FPwPlaybackConditionSet Set;
    if (!PwAnalyzePlaybackConditionSet(In, Set, OutErrorCode, OutError))
    {
        return false;
    }
    Out = MoveTemp(Set.Conditions);
    return true;
}

// =============================================================================================
// Attaching to a descriptor report
// =============================================================================================

bool PwAnalyzeAudioContext(const FPwAudioBuffer& In, const FPwAudioContextRequest& Request,
                           FPwAudioAnalysis& InOut, FString& OutErrorCode, FString& OutError)
{
    OutErrorCode.Reset();
    OutError.Reset();

    bool bAllMeasured = true;

    if (Request.bRetrigger)
    {
        const TArray<double> Rates = Request.RetriggerRatesHz.Num() > 0
            ? Request.RetriggerRatesHz : PwGetDefaultRetriggerRates();

        FPwRetriggerResult Retrigger;
        FString Code;
        FString Message;
        if (!PwAnalyzeRetrigger(In, Rates, Retrigger, Code, Message))
        {
            bAllMeasured = false;
            if (OutErrorCode.IsEmpty())
            {
                OutErrorCode = Code;
                OutError = Message;
            }
        }
        // Attached either way: a requested family that failed is present-and-unmeasured, which
        // serializes under `unmeasured` with its reason. Only a family nobody asked for is absent.
        InOut.Retrigger = MoveTemp(Retrigger);
    }

    if (Request.bPlaybackConditions)
    {
        FPwPlaybackConditionSet Conditions;
        FString Code;
        FString Message;
        if (!PwAnalyzePlaybackConditionSet(In, Conditions, Code, Message))
        {
            bAllMeasured = false;
            if (OutErrorCode.IsEmpty())
            {
                OutErrorCode = Code;
                OutError = Message;
            }
        }
        InOut.PlaybackConditions = MoveTemp(Conditions);
    }

    return bAllMeasured;
}

void PwSerializeAudioContext(const FPwAudioAnalysis& In, bool bFullDetail,
                             const TSharedPtr<FJsonObject>& Root,
                             const TSharedPtr<FJsonObject>& Unmeasured)
{
    using namespace PwAudioContextInternal;

    if (!Root.IsValid() || !Unmeasured.IsValid())
    {
        return;
    }

    // ---- retrigger ------------------------------------------------------------------------
    if (In.Retrigger.IsSet())
    {
        const FPwRetriggerResult& Retrigger = In.Retrigger.GetValue();
        if (Retrigger.bMeasured)
        {
            const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();

            // The summary form thins a large rate set the same way the descriptor report thins a
            // per-frame series, and SAYS SO - a silently thinned ladder and a short one look
            // identical. The default four rates are never thinned.
            const int32 TotalPoints = Retrigger.Points.Num();
            const int32 Stride = bFullDetail
                ? 1
                : FMath::Max(1, FMath::DivideAndRoundUp(TotalPoints, MaxSummaryRetriggerPoints));

            TArray<TSharedPtr<FJsonValue>> Points;
            for (int32 Index = 0; Index < TotalPoints; Index += Stride)
            {
                const FPwRetriggerPoint& Point = Retrigger.Points[Index];
                const TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                SetNum(Entry, TEXT("rateHz"), Point.RateHz, 3);
                SetNum(Entry, TEXT("peakDbDelta"), Point.PeakDb, 2);
                SetNum(Entry, TEXT("rmsDbDelta"), Point.RmsDb, 2);
                SetNum(Entry, TEXT("lowBandDbDelta"), Point.LowBandDb, 2);
                Entry->SetNumberField(TEXT("clippedSamples"), Point.ClippedSamples);
                SetNum(Entry, TEXT("tailOverlapRatio"), Point.TailOverlapRatio, 4);
                Entry->SetBoolField(TEXT("effectivelyContinuous"), Point.bEffectivelyContinuous);
                Points.Add(MakeShared<FJsonValueObject>(Entry));
            }
            Object->SetArrayField(TEXT("points"), MoveTemp(Points));
            if (Stride > 1)
            {
                Object->SetNumberField(TEXT("totalPoints"), TotalPoints);
                Object->SetNumberField(TEXT("stride"), Stride);
            }

            // Omitted, not zeroed, when the measured rates do not bracket the crossing: 0 Hz would
            // read as "continuous at any rate", which is the opposite of what an unbracketed
            // ladder of all-discrete points means.
            if (Retrigger.bContinuousRateMeasured)
            {
                SetNum(Object, TEXT("continuousAboveRateHz"), Retrigger.ContinuousAboveRateHz, 2);
            }
            Root->SetObjectField(TEXT("retrigger"), Object);
        }
        else
        {
            Unmeasured->SetStringField(TEXT("retrigger"),
                Retrigger.UnmeasuredReason.IsEmpty() ? FString(TEXT("not measured"))
                                                     : Retrigger.UnmeasuredReason);
        }
    }

    // ---- playbackConditions -----------------------------------------------------------------
    if (In.PlaybackConditions.IsSet())
    {
        const FPwPlaybackConditionSet& Set = In.PlaybackConditions.GetValue();
        if (Set.bMeasured)
        {
            const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();

            // The reference travels with the deltas so the caller can un-difference them (§6).
            const TSharedPtr<FJsonObject> Reference = MakeShared<FJsonObject>();
            SetNum(Reference, TEXT("peakDb"), Set.ReferencePeakDb, 2);
            SetNum(Reference, TEXT("blockPeakDb"), Set.ReferenceBlockPeakDb, 2);
            if (Set.bReferenceCentroidMeasured)
            {
                SetNum(Reference, TEXT("centroidHz"), Set.ReferenceCentroidHz, 1);
            }
            Object->SetObjectField(TEXT("reference"), Reference);

            TArray<TSharedPtr<FJsonValue>> Rows;
            for (const FPwConditionResult& Condition : Set.Conditions)
            {
                const TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("condition"),
                    PwPlaybackConditionName(Condition.Condition));
                SetNum(Entry, TEXT("retainedEnergyRatio"), Condition.RetainedEnergyRatio, 6);
                SetNum(Entry, TEXT("peakDbDelta"), Condition.PeakDbDelta, 2);
                // Omitted rather than zeroed when the rendering has no spectrum to take a centroid
                // from: a 0.0 delta would read as "brightness unchanged" for a signal that is gone.
                if (Condition.bCentroidMeasured)
                {
                    SetNum(Entry, TEXT("centroidHzDelta"), Condition.CentroidHzDelta, 1);
                }
                Entry->SetBoolField(TEXT("transientSurvives"), Condition.bTransientSurvives);
                Entry->SetBoolField(TEXT("audible"), Condition.bAudible);
                Rows.Add(MakeShared<FJsonValueObject>(Entry));
            }
            Object->SetArrayField(TEXT("conditions"), MoveTemp(Rows));
            Root->SetObjectField(TEXT("playbackConditions"), Object);
        }
        else
        {
            Unmeasured->SetStringField(TEXT("playbackConditions"),
                Set.UnmeasuredReason.IsEmpty() ? FString(TEXT("not measured"))
                                               : Set.UnmeasuredReason);
        }
    }
}
