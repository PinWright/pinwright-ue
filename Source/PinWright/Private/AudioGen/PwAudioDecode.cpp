// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "AudioGen/PwAudioDecode.h"

#include "Handlers/ErrorCodes.h"
#include "Sound/SoundWave.h"

// Named (not anonymous) namespace: the main module builds with Unity on, and two
// anonymous namespaces merged into one TU collide by name. See CLAUDE.md > Building.
namespace PwAudioDecodeInternal
{
    // int16 full-scale is -32768..32767; dividing by 32768 keeps the negative rail
    // at exactly -1.0 and costs one ulp at the positive rail, which is the standard
    // asymmetric-but-non-clipping choice.
    constexpr float Pcm16ToFloatScale = 1.0f / 32768.0f;

    // Interleaved int16 -> deinterleaved float, one pass, no scratch buffer.
    // Audio::ArrayPcm16ToFloat (DSP/FloatArrayMath.h:251) converts a *contiguous*
    // run and cannot deinterleave, so using it would mean a full-size float scratch
    // buffer plus a second strided copy - and SignalProcessing is not a module
    // dependency of PinWright, so it would also mean editing Build.cs. The strided
    // loop below is smaller and does the same arithmetic.
    //
    // Mono duplicates into both channels; stereo splits. Caller has already sized
    // OutBuffer to NumFrames and validated NumChannels is 1 or 2.
    void DeinterleavePcm16(const int16* Samples, int32 NumFrames, int32 NumChannels,
        FPwAudioBuffer& OutBuffer)
    {
        float* RESTRICT LeftData = OutBuffer.Left.GetData();
        float* RESTRICT RightData = OutBuffer.Right.GetData();

        if (NumChannels == 1)
        {
            for (int32 Frame = 0; Frame < NumFrames; ++Frame)
            {
                const float Value = static_cast<float>(Samples[Frame]) * Pcm16ToFloatScale;
                LeftData[Frame] = Value;
                RightData[Frame] = Value;
            }
        }
        else
        {
            for (int32 Frame = 0; Frame < NumFrames; ++Frame)
            {
                LeftData[Frame] = static_cast<float>(Samples[Frame * 2]) * Pcm16ToFloatScale;
                RightData[Frame] = static_cast<float>(Samples[Frame * 2 + 1]) * Pcm16ToFloatScale;
            }
        }
    }

    // Name the wave in failure messages the way the engine's own audio warnings do.
    FString DescribeWave(const USoundWave* Wave)
    {
        return Wave ? Wave->GetFullName() : FString(TEXT("<null>"));
    }
}

bool PwDecodeSoundWaveWithCode(const USoundWave* Wave, FPwAudioBuffer& Out,
    FString& OutErrorCode, FString& OutError)
{
    using namespace PwAudioDecodeInternal;

    // Failure is the default (rpc-design.md §1): the caller's buffer is emptied
    // before any check runs, so every early-out below - and any future one - leaves
    // an empty result rather than whatever the caller happened to pass in.
    Out = FPwAudioBuffer();
    OutErrorCode.Reset();
    OutError.Reset();

    // UAudioAnalyzerNRT::AnalyzeAudio opens with check(IsInGameThread())
    // (AudioAnalyzerNRT.cpp:132). Report it rather than take the editor down.
    //
    // INVALID_STATE, not DECODE_FAILED: the caller's remedy is to re-dispatch onto the
    // game thread, whereas DECODE_FAILED means give up on this asset. §7 wants those
    // two answers to carry different codes.
    if (!IsInGameThread())
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_STATE;
        OutError = TEXT("PwDecodeSoundWave must run on the game thread: the imported payload is ")
                   TEXT("read under the sound wave's RawDataCriticalSection.");
        return false;
    }

    // A null pointer is a caller bug, not a missing asset: a handler that failed to
    // resolve an asset path reports ASSET_NOT_FOUND before it ever gets here.
    if (!Wave)
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_PARAMS;
        OutError = TEXT("No sound wave supplied to decode.");
        return false;
    }

    // Guard copied from UAudioAnalyzerNRT::AnalyzeAudio (AudioAnalyzerNRT.cpp:139-144).
    // A procedural wave synthesizes its samples at playback time and has no imported
    // payload to read, so this is a different problem from a corrupt one and gets its
    // own code (rpc-design.md §7).
    if (Wave->bProcedural)
    {
        OutErrorCode = ErrorCodes::ERR_AUDIO_PROCEDURAL_UNSUPPORTED;
        OutError = FString::Printf(
            TEXT("Sound wave '%s' is procedural: it generates samples at playback time and carries ")
            TEXT("no imported PCM payload to decode."),
            *DescribeWave(Wave));
        return false;
    }

    // Guard copied from UAudioAnalyzerNRT::AnalyzeAudio (AudioAnalyzerNRT.cpp:147-152)
    // and USoundWave::BakeFFTAnalysis (SoundWave.cpp:3006-3010). A non-empty
    // ChannelSizes is the engine's own tell for the >2-channel layout, where RawData
    // holds N concatenated mono RIFF files rather than one interleaved stream.
    if (Wave->ChannelSizes.Num() > 0)
    {
        OutErrorCode = ErrorCodes::ERR_AUDIO_MULTICHANNEL_UNSUPPORTED;
        OutError = FString::Printf(
            TEXT("Sound wave '%s' has multi-channel audio (more than 2 channels); only mono and ")
            TEXT("stereo can be decoded."),
            *DescribeWave(Wave));
        return false;
    }

    TArray<uint8> RawPcm;
    uint32 SampleRate = 0;
    uint16 NumChannels = 0;

    // Blocks on the bulk-data payload future (SoundWave.cpp:1784). Output is
    // interleaved int16 regardless of what the source file was.
    if (!Wave->GetImportedSoundWaveData(RawPcm, SampleRate, NumChannels))
    {
        OutErrorCode = ErrorCodes::ERR_DECODE_FAILED;
        OutError = FString::Printf(
            TEXT("Could not read imported sound wave data from '%s'; the asset has no imported ")
            TEXT("payload or its wave header failed to parse."),
            *DescribeWave(Wave));
        return false;
    }

    // §7: zero is not a small number. The emptiness check runs before any header
    // validation so a wave with no samples is reported as empty, not as one with a
    // malformed rate or channel count.
    if (RawPcm.Num() == 0)
    {
        OutErrorCode = ErrorCodes::ERR_AUDIO_EMPTY_BUFFER;
        OutError = FString::Printf(
            TEXT("Sound wave '%s' decoded to zero bytes of PCM."),
            *DescribeWave(Wave));
        return false;
    }

    // §3: no unsafe defaults. A missing rate or channel count is an error, never a
    // guessed 48000/mono - the engine makes the same call at AudioAnalyzerNRT.cpp:166
    // and SoundWave.cpp:3028.
    if (SampleRate == 0 || NumChannels == 0)
    {
        OutErrorCode = ErrorCodes::ERR_DECODE_FAILED;
        OutError = FString::Printf(
            TEXT("Failed to parse the raw imported data for '%s': sampleRate=%u, numChannels=%u."),
            *DescribeWave(Wave), SampleRate, static_cast<uint32>(NumChannels));
        return false;
    }

    // ChannelSizes above catches the surround layout the engine knows about; this
    // catches a wave whose parsed header still reports more than two channels.
    if (NumChannels > 2)
    {
        OutErrorCode = ErrorCodes::ERR_AUDIO_MULTICHANNEL_UNSUPPORTED;
        OutError = FString::Printf(
            TEXT("Sound wave '%s' decoded to %u channels; only mono and stereo can be decoded."),
            *DescribeWave(Wave), static_cast<uint32>(NumChannels));
        return false;
    }

    // GetImportedSoundWaveData already drops any trailing partial frame, so this
    // floor division only rounds when the payload is shorter than a single frame.
    const int32 NumSamples = RawPcm.Num() / static_cast<int32>(sizeof(int16));
    const int32 NumFrames = NumSamples / static_cast<int32>(NumChannels);

    if (NumFrames <= 0)
    {
        OutErrorCode = ErrorCodes::ERR_AUDIO_EMPTY_BUFFER;
        OutError = FString::Printf(
            TEXT("Sound wave '%s' decoded to %d byte(s) at %u channel(s), which is less than one ")
            TEXT("complete frame."),
            *DescribeWave(Wave), RawPcm.Num(), static_cast<uint32>(NumChannels));
        return false;
    }

    // Build into a local and hand it over on the last line, so no partially filled
    // buffer is ever observable through Out.
    FPwAudioBuffer Decoded;
    Decoded.SampleRate = static_cast<int32>(SampleRate);
    Decoded.SetNumFrames(NumFrames, /*bZeroed=*/false);

    DeinterleavePcm16(reinterpret_cast<const int16*>(RawPcm.GetData()), NumFrames,
        static_cast<int32>(NumChannels), Decoded);

    Out = MoveTemp(Decoded);
    return true;
}

bool PwDecodeSoundWave(const USoundWave* Wave, FPwAudioBuffer& Out, FString& OutError)
{
    FString UnusedErrorCode;
    return PwDecodeSoundWaveWithCode(Wave, Out, UnusedErrorCode, OutError);
}
