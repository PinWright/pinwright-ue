// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "AudioGen/PwStft.h"

#include "DSP/AlignedBuffer.h"
#include "DSP/FFTAlgorithm.h"
#include "Handlers/ErrorCodes.h"
#include "Math/UnrealMathUtility.h"

// Named (not anonymous) namespace: this module builds with bUseUnity = true, and anonymous
// namespaces in merged TUs are the ODR-collision source the plugin's Build.cs comment warns about.
namespace PwStftInternal
{
    /**
     * Peak absolute sample below which the input is reported as silence instead of analysed.
     * 1e-9 is about -180 dBFS - roughly 36 dB below a 24-bit LSB - so no real recording lands
     * here, while a zero-filled or never-rendered buffer always does. Naming silence matters
     * because the alternative is a spectrogram of nothing: every bin clamps to the dB floor and
     * a vision model reads "very quiet audio" where the truth is "no audio".
     */
    constexpr float SilencePeak = 1e-9f;

    /**
     * Ceiling on the magnitude matrix, in floats. 64M floats = 256 MB. A 10-minute 48 kHz file
     * at hop 256 would otherwise ask for ~115M floats (460 MB) in one allocation and take the
     * editor down; this is a reachable failure, not a defensive one.
     */
    constexpr int64 MaxMagnitudeFloats = 64ll * 1024ll * 1024ll;

    bool IsPowerOfTwo(int32 Value)
    {
        return Value > 0 && (Value & (Value - 1)) == 0;
    }

    bool Fail(FPwStftError* OutError, const TCHAR* Code, const FString& Message)
    {
        if (OutError)
        {
            OutError->Code = Code;
            OutError->Message = Message;
        }
        return false;
    }
}

int32 PwStftFrameCount(int32 NumSamples, int32 FftSize, int32 HopSize)
{
    if (FftSize <= 0 || HopSize <= 0 || NumSamples < FftSize)
    {
        return 0;
    }
    return 1 + (NumSamples - FftSize) / HopSize;
}

float PwMagnitudeToDb(float Magnitude, float FloorDb)
{
    // The negated comparison also catches NaN, which would otherwise propagate into the image.
    if (!(Magnitude > 0.f))
    {
        return FloorDb;
    }
    return FMath::Max(20.f * FMath::LogX(10.f, Magnitude), FloorDb);
}

bool PwComputeStft(TArrayView<const float> Mono, int32 SampleRate,
                   const FPwStftSettings& Settings, FPwStftResult& Out,
                   FPwStftError* OutError)
{
    using namespace PwStftInternal;

    // Failure is the default: clear both out-parameters up front so no early return can leave a
    // partially populated result behind for a caller that ignored the bool.
    Out = FPwStftResult();
    if (OutError)
    {
        *OutError = FPwStftError();
    }

    // ---------------------------------------------------------------------------------------
    // Existence checks FIRST (rpc-design.md §7). A threshold check and an existence check can
    // both match here - a 0-sample buffer is also "shorter than one FFT window" - and answering
    // with the threshold code would point the caller at lengthening a buffer that has no audio
    // in it at all.
    // ---------------------------------------------------------------------------------------
    if (Mono.Num() == 0)
    {
        return Fail(OutError, ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
            TEXT("Input buffer contains 0 samples; there is nothing to transform."));
    }

    float PeakAbs = 0.f;
    for (const float Sample : Mono)
    {
        PeakAbs = FMath::Max(PeakAbs, FMath::Abs(Sample));
    }
    if (!(PeakAbs >= SilencePeak))
    {
        return Fail(OutError, ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
            FString::Printf(TEXT("Input buffer holds %d samples but its peak absolute amplitude is %.3e ")
                            TEXT("(below the %.0e silence floor, about -180 dBFS): the signal is digital silence, ")
                            TEXT("not quiet audio."),
                Mono.Num(), PeakAbs, SilencePeak));
    }

    // ---------------------------------------------------------------------------------------
    // Parameter and size validation.
    // ---------------------------------------------------------------------------------------
    if (SampleRate <= 0)
    {
        return Fail(OutError, ErrorCodes::ERR_STFT_FAILED,
            FString::Printf(TEXT("SampleRate must be positive; got %d."), SampleRate));
    }
    if (!IsPowerOfTwo(Settings.FftSize))
    {
        // Rejected rather than rounded: silently analysing at a size the caller did not ask for
        // would move every reported bin frequency without saying so.
        const int32 NextPowerOfTwo = 1 << Audio::CeilLog2(FMath::Clamp(Settings.FftSize, 32, 131072));
        return Fail(OutError, ErrorCodes::ERR_STFT_FAILED,
            FString::Printf(TEXT("FftSize must be a power of two; got %d. Next valid size up is %d."),
                Settings.FftSize, NextPowerOfTwo));
    }
    if (Settings.HopSize <= 0)
    {
        return Fail(OutError, ErrorCodes::ERR_STFT_FAILED,
            FString::Printf(TEXT("HopSize must be positive; got %d."), Settings.HopSize));
    }
    if (Mono.Num() < Settings.FftSize)
    {
        return Fail(OutError, ErrorCodes::ERR_STFT_FAILED,
            FString::Printf(TEXT("Signal is %d samples, shorter than one %d-sample analysis window; ")
                            TEXT("no whole frame fits."),
                Mono.Num(), Settings.FftSize));
    }

    Audio::FFFTSettings FftSettings;
    FftSettings.Log2Size = Audio::CeilLog2(Settings.FftSize);
    // MUST be true. FVectorFFTFactory::AreFFTSettingsSupported returns
    // `bIsMinSizeSupported && bArrays128BitAligned && bIsMaxSizeSupported` - it treats the flag
    // as a hard requirement even though its own Expects128BitAlignedArrays() returns false. With
    // false here no registered factory supports the settings and NewFFTAlgorithm returns null.
    // Every buffer handed to the algorithm below is therefore an Audio::FAlignedFloatBuffer.
    FftSettings.bArrays128BitAligned = true;
    FftSettings.bEnableHardwareAcceleration = false;

    if (!Audio::FFFTFactory::AreFFTSettingsSupported(FftSettings))
    {
        return Fail(OutError, ErrorCodes::ERR_STFT_FAILED,
            FString::Printf(TEXT("No registered FFT factory supports FftSize %d (log2 %d). ")
                            TEXT("The engine's FVectorFFT covers log2 5..17, i.e. 32..131072 samples."),
                Settings.FftSize, FftSettings.Log2Size));
    }

    TUniquePtr<Audio::IFFTAlgorithm> Fft = Audio::FFFTFactory::NewFFTAlgorithm(FftSettings);
    if (!Fft.IsValid())
    {
        return Fail(OutError, ErrorCodes::ERR_STFT_FAILED,
            FString::Printf(TEXT("FFFTFactory::NewFFTAlgorithm returned null for FftSize %d (log2 %d)."),
                Settings.FftSize, FftSettings.Log2Size));
    }

    const int32 NumBins = Settings.FftSize / 2 + 1;
    const int32 NumFrames = PwStftFrameCount(Mono.Num(), Settings.FftSize, Settings.HopSize);
    const int64 TotalFloats = static_cast<int64>(NumFrames) * static_cast<int64>(NumBins);
    if (TotalFloats > MaxMagnitudeFloats)
    {
        return Fail(OutError, ErrorCodes::ERR_STFT_FAILED,
            FString::Printf(TEXT("Requested spectrogram is %lld magnitudes (%d frames x %d bins, %.0f MB), ")
                            TEXT("over the %lld-magnitude cap. Increase HopSize or analyse a shorter span."),
                TotalFloats, NumFrames, NumBins,
                static_cast<double>(TotalFloats) * sizeof(float) / (1024.0 * 1024.0),
                MaxMagnitudeFloats));
    }

    // Periodic (DFT-even) window, not symmetric: it is the convention under which a tone landing
    // exactly on a bin centre leaks nothing into its neighbours, which is what makes the accuracy
    // assertions in TestPwStft.cpp exact. Audio::FSpectrumAnalyzer passes false here; the
    // difference is one sample in FftSize and does not survive the coherent-gain normalization,
    // but the periodic form is the correct one for spectral analysis.
    Audio::FWindow Window(Settings.Window, Settings.FftSize, /*InNumChannels*/ 1, /*bIsPeriodic*/ true);

    // Coherent gain = sum of the window samples. Measured by running the window over an all-ones
    // buffer rather than re-deriving it per window type: that way a window type added to
    // EWindowType in a later engine version is normalized correctly with no code change here.
    Audio::FAlignedFloatBuffer WindowProbe;
    WindowProbe.Init(1.f, Settings.FftSize);
    Window.ApplyToBuffer(WindowProbe.GetData());
    double WindowSum = 0.0;
    for (const float Value : WindowProbe)
    {
        WindowSum += Value;
    }
    if (!(WindowSum > 0.0))
    {
        return Fail(OutError, ErrorCodes::ERR_STFT_FAILED,
            FString::Printf(TEXT("Analysis window has non-positive coherent gain (%.6f); cannot normalize magnitudes."),
                WindowSum));
    }

    // See the scaling convention on FPwStftResult. GetPowerSpectrumScaling returns the factor for
    // a POWER spectrum, so the amplitude factor is its square root.
    //
    // The TARGET is MultipliedBySqrtFFTSize, not None. EFFTScaling is defined relative to an
    // ENERGY-PRESERVING transform - the enum documents None as "no scaling needed to maintain
    // equal energy" - and the unscaled DFT is not that one: Parseval gives
    // sum_k |X_k|^2 = FftSize * sum_n |x_n|^2, so the unscaled DFT is itself
    // "MultipliedBySqrtFFTSize" in this vocabulary, which is exactly why FVectorFFT - whose
    // output IS the unscaled DFT - reports that value. The window-sum division below already
    // assumes the unscaled DFT (a windowed tone's peak bin is A/2 * sum(w)), so asking for None
    // applied a SECOND 1/sqrt(FftSize) and every magnitude came out sqrt(FftSize) low: a
    // unit-amplitude tone read 1/64 = -36 dBFS at FftSize 4096 instead of the documented 1.0.
    // The call is kept rather than dropped so a factory reporting a different convention is
    // still converted; for FVectorFFT the resulting factor is exactly 1.
    const float PowerScale = Audio::GetPowerSpectrumScaling(
        Settings.FftSize, Fft->ForwardScaling(), Audio::EFFTScaling::MultipliedBySqrtFFTSize);
    const float AmplitudeScale = FMath::Sqrt(FMath::Max(PowerScale, UE_SMALL_NUMBER));
    const float InteriorBinScale = AmplitudeScale * (2.f / static_cast<float>(WindowSum));
    const float EdgeBinScale = AmplitudeScale * (1.f / static_cast<float>(WindowSum));

    Audio::FAlignedFloatBuffer FrameBuffer;
    FrameBuffer.SetNumUninitialized(Settings.FftSize);
    Audio::FAlignedFloatBuffer Spectrum;
    Spectrum.SetNumUninitialized(Fft->NumOutputFloats());

    TArray<float> Magnitudes;
    Magnitudes.SetNumUninitialized(static_cast<int32>(TotalFloats));

    const float* RESTRICT Source = Mono.GetData();
    for (int32 FrameIndex = 0; FrameIndex < NumFrames; ++FrameIndex)
    {
        FMemory::Memcpy(FrameBuffer.GetData(), Source + FrameIndex * Settings.HopSize,
            Settings.FftSize * sizeof(float));
        Window.ApplyToBuffer(FrameBuffer.GetData());
        Fft->ForwardRealToComplex(FrameBuffer.GetData(), Spectrum.GetData());

        // NumOutputFloats() == FftSize + 2 == 2 * NumBins interleaved (re, im) floats.
        const float* RESTRICT Complex = Spectrum.GetData();
        float* RESTRICT Row = Magnitudes.GetData() + static_cast<int64>(FrameIndex) * NumBins;
        for (int32 Bin = 0; Bin < NumBins; ++Bin)
        {
            const float Real = Complex[2 * Bin];
            const float Imaginary = Complex[2 * Bin + 1];
            const float Raw = FMath::Sqrt(Real * Real + Imaginary * Imaginary);
            const bool bEdgeBin = (Bin == 0) || (Bin == NumBins - 1);
            Row[Bin] = Raw * (bEdgeBin ? EdgeBinScale : InteriorBinScale);
        }
    }

    Out.Magnitudes = MoveTemp(Magnitudes);
    Out.NumFrames = NumFrames;
    Out.NumBins = NumBins;
    Out.BinHz = static_cast<float>(SampleRate) / static_cast<float>(Settings.FftSize);
    Out.FftSize = Settings.FftSize;
    Out.HopSize = Settings.HopSize;
    return Out.IsValid();
}
