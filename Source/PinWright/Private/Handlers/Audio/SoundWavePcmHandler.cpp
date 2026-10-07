// Copyright (c) 2026 Alexander Penkin. MIT License.

// SoundWavePcmHandler.cpp - audio.authoring.create_sound_wave_from_pcm
//
// The way generated PCM gets INTO the project. Everything else in the audio-gen
// subsystem produces an FPwAudioBuffer; this is the verb that turns one into a
// USoundWave asset.
//
// INLINE SAMPLES ARE THE INTERIM SHAPE. The samples arrive as a JSON float array,
// which is the only transport available until the candidate registry ships a
// candidate_id form of this verb. That puts a hard ceiling on it: the gateway
// rejects request bodies over 1 MB (UPinWrightSettings), and a JSON float costs
// ~10-13 bytes, so the body limit bites at roughly the same place as the explicit
// PwMaxInlineSamples cap below. Both are reported as errors naming the successor
// verb rather than being silently truncated.
//
// THE VERIFICATION IS THE POINT (rpc-design.md §4). Two genuinely independent
// subsystems touch the same bytes: Audio::FSoundWavePCMWriter writes the RIFF
// payload into USoundWave::RawData, and USoundWave::GetImportedSoundWaveData (via
// PwDecodeSoundWave) parses it back out. So after creating the asset the handler
// decodes it again and compares frame count, sample rate, channel count and an
// RMS + peak signature against the source buffer. Nothing in the response is a
// read-back of a field the writer just set.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "PinWrightHelpers.h"
#include "Utils/AssetCreatePolicy.h"
#include "Utils/PathUtils.h"

#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwAudioDecode.h"
#include "AudioGen/PwAudioExport.h"
#include "AudioGen/PwAudioFeatures.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "Sound/SoundWave.h"

// Named (not anonymous) namespace: the main module builds with Unity on, and two
// anonymous namespaces merged into one TU collide by name. See AGENTS.md > Building.
namespace PwSoundWavePcmHandlerInternal
{
    // Ceiling on the inline JSON sample array. 96000 interleaved values is one second
    // of 48 kHz stereo, and at ~10-13 bytes per serialized float it is already at the
    // gateway's 1 MB body limit - so this cap is what the caller hits first, with a
    // message that names the successor verb instead of a transport-level truncation.
    constexpr int32 PwMaxInlineSamples = 96000;

    // Highest sample rate accepted. Above this the value is far more likely to be a
    // frame count or a byte count pasted into the wrong field than a real rate.
    constexpr int32 PwMaxSampleRate = 192000;

    // Absolute tolerance for the decode-back RMS/peak comparison, in normalized float.
    //
    // Derived, not guessed. The encode is Audio::ArrayFloatToPcm16 - multiply by 32767
    // and truncate (FloatArrayMath.cpp:2490) - and the decode is PwAudioDecode's
    // divide by 32768. That is at most one LSB of truncation (1/32767 = 3.05e-5) plus
    // the 32767/32768 scale gap (<= 3.05e-5), so a correct round trip stays inside
    // ~6.2e-5 per sample. Both RMS and peak are 1-Lipschitz in the per-sample error
    // (Minkowski for the 2-norm, trivially for the max), so the same bound applies to
    // the signatures. 1e-3 is ~16x that - loose enough to survive a change of scale
    // convention on either side, and still three or more orders of magnitude away from
    // every defect worth catching: a silent asset, a half-length asset, the wrong
    // buffer, or a channel that did not make it.
    constexpr double PwRoundTripToleranceAbs = 1.0e-3;

    struct FPwChannelSignature
    {
        double Rms = 0.0;
        double Peak = 0.0;
    };

    FPwChannelSignature MeasureChannel(const TArray<float>& Samples)
    {
        FPwChannelSignature Out;
        if (Samples.Num() <= 0)
        {
            return Out;
        }

        double SumOfSquares = 0.0;
        double Peak = 0.0;
        for (const float Sample : Samples)
        {
            const double Value = static_cast<double>(Sample);
            SumOfSquares += Value * Value;
            Peak = FMath::Max(Peak, FMath::Abs(Value));
        }

        Out.Rms = FMath::Sqrt(SumOfSquares / static_cast<double>(Samples.Num()));
        Out.Peak = Peak;
        return Out;
    }

    // Emits the per-channel half of the verification block and returns whether both
    // signatures agree within tolerance. Deltas are always emitted, so a failure says
    // by how much rather than only that it failed.
    bool AddChannelComparison(const TSharedPtr<FJsonObject>& Parent, const TCHAR* FieldName,
        const FPwChannelSignature& Source, const FPwChannelSignature& Decoded)
    {
        const double RmsDelta = FMath::Abs(Source.Rms - Decoded.Rms);
        const double PeakDelta = FMath::Abs(Source.Peak - Decoded.Peak);

        TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
        Block->SetNumberField(TEXT("sourceRms"), Source.Rms);
        Block->SetNumberField(TEXT("decodedRms"), Decoded.Rms);
        Block->SetNumberField(TEXT("rmsDelta"), RmsDelta);
        Block->SetNumberField(TEXT("sourcePeak"), Source.Peak);
        Block->SetNumberField(TEXT("decodedPeak"), Decoded.Peak);
        Block->SetNumberField(TEXT("peakDelta"), PeakDelta);
        Parent->SetObjectField(FieldName, Block);

        return RmsDelta <= PwRoundTripToleranceAbs && PeakDelta <= PwRoundTripToleranceAbs;
    }

    // set_sound_wave_gain: how far the decoded level may sit from the requested target, dB. The
    // write quantizes to 16-bit PCM, which moves a peak by at most one LSB - 0.0003 dB at full
    // scale, ~0.03 dB at -40 dBFS - and BS.1770 gating can shift a gated integral by a few
    // hundredths when every sample scales; 0.1 dB clears both and is still ten times finer than
    // the 1 LU delivery tolerance a loudness review works to.
    constexpr double PwGainToleranceDb = 0.1;

    double LinearToDbOrFloor(double Linear)
    {
        return Linear > 0.0 ? 20.0 * FMath::LogX(10.0, Linear) : -200.0;
    }

    struct FPwLevelReading
    {
        double PeakLinear = 0.0;
        double LeftRms = 0.0;
        double RightRms = 0.0;
        bool bLufsMeasured = false;
        double IntegratedLufs = 0.0;
    };

    // Peak and per-side RMS always; integrated loudness when the buffer is long and loud enough
    // for the analyzer, with its refusal kept for a caller that asked for "lufs".
    FPwLevelReading ReadLevel(const FPwAudioBuffer& Buffer, FString& OutLufsCode,
                              FString& OutLufsError)
    {
        FPwLevelReading Out;
        const FPwChannelSignature Left = MeasureChannel(Buffer.Left);
        const FPwChannelSignature Right = MeasureChannel(Buffer.Right);
        Out.PeakLinear = FMath::Max(Left.Peak, Right.Peak);
        Out.LeftRms = Left.Rms;
        Out.RightRms = Right.Rms;

        FPwLoudnessResult Loudness;
        if (PwComputeLoudness(Buffer, Loudness, OutLufsCode, OutLufsError) && Loudness.bMeasured)
        {
            Out.bLufsMeasured = true;
            Out.IntegratedLufs = Loudness.IntegratedLufs;
        }
        return Out;
    }

    TSharedPtr<FJsonObject> LevelToJson(const FPwLevelReading& In)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetNumberField(TEXT("peakDb"), LinearToDbOrFloor(In.PeakLinear));
        if (In.bLufsMeasured)
        {
            Out->SetNumberField(TEXT("integratedLufs"), In.IntegratedLufs);
        }
        return Out;
    }
}

REGISTER_RPC_HANDLER("audio.authoring.create_sound_wave_from_pcm", "audio.authoring",
    "Create a USoundWave asset from raw interleaved float PCM sent inline as JSON, then verify it by decoding the created asset back and comparing frames, sample rate, channel count and an RMS/peak signature against the source. The wave keeps the input's channel count: channels:1 writes a mono wave, channels:2 a stereo one. Idempotent: re-running against an existing SoundWave rewrites its payload in place and keeps every property that is not the payload (SoundClass, attenuation, concurrency, submix and bus sends, modulation, loading behaviour, compression type, looping, volume, sound group), refreshing only the state parsed out of the old audio (duration, format, cue points, channel layout, timecode); verification.propertiesPreserved measures that and a failure fails the call. The response reports routing.soundClass / routing.attenuationSettings as they stand after the write - a newly created wave has neither, and an unrouted wave escapes every SoundMix and plays at full level at any distance. Errors ASSET_ALREADY_EXISTS when a different asset class occupies the path, VERIFICATION_FAILED when the decode-back disagrees with the source. Inline samples are capped; a candidate_id form of this verb is the route for longer audio.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Asset name without extension, e.g. 'SW_Beep'."),
        RPC_PARAM_REQ("path", "path", "Content-browser folder for the new asset; must be under /Game, e.g. /Game/Audio."),
        RPC_PARAM_REQ("samples", "array", "Interleaved float PCM in [-1,1]. Length must be an exact multiple of channels and at most 96000 values (one second of 48 kHz stereo, which is also about where the 1 MB request-body limit lands). Values outside [-1,1] are clamped and counted in clampedSamples."),
        RPC_PARAM_REQ("sampleRate", "number", "Sample rate of the supplied PCM in Hz, 1..192000."),
        RPC_PARAM_REQ("channels", "number", "Interleave width of samples: 1 (mono) or 2 (stereo). Required rather than defaulted because guessing wrong silently garbles the audio instead of failing. The created wave has the same channel count, so channels:1 produces a mono wave (half the memory and decode of a duplicated stereo one)."),
        RPC_PARAM_DEF("save", "boolean", "Write the .uasset to disk. false marks the package dirty only, and the response reports saved:false / pendingFlush:true.", "true"),
        RPC_PARAM_DEF("overwrite", "boolean", "Delete and recreate an existing SoundWave instead of rewriting it in place. Rejected with ASSET_IN_USE when other packages reference it.", "false")
    ))
{
    using namespace PwSoundWavePcmHandlerInternal;

    FString Name;
    if (!Ctx.RequireString(TEXT("name"), Name)) return true;
    FString FolderPath;
    if (!Ctx.RequireString(TEXT("path"), FolderPath)) return true;

    // Reject a name the sanitizer would rewrite rather than quietly creating an asset
    // under a different name than the caller asked for (rpc-design.md §1).
    const FString SanitizedName = SanitizeAssetName(Name);
    if (SanitizedName.IsEmpty() || SanitizedName != Name)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PATH,
            FString::Printf(TEXT("Invalid asset name '%s': contains characters that cannot be used "
                                 "in asset names. Valid name would be: '%s'"),
                *Name, *SanitizedName));
        return true;
    }

    FString ValidatedPath;
    FString PathError;
    if (!ValidateAssetCreationPath(FolderPath, Name, ValidatedPath, PathError))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PATH, PathError);
        return true;
    }

    // PwCreateSoundWaveAsset refuses a non-/Game root too (the engine writer hardcodes
    // the prefix), but rejecting here keeps the failure at argument-validation time,
    // before anything has been allocated.
    const FString ValidatedFolder = FPackageName::GetLongPackagePath(ValidatedPath);
    if (!ValidatedFolder.Equals(TEXT("/Game")) && !ValidatedFolder.StartsWith(TEXT("/Game/")))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PATH,
            FString::Printf(TEXT("SoundWave assets can only be created under /Game; '%s' resolves "
                                 "to '%s'."), *FolderPath, *ValidatedFolder));
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* SampleValues = nullptr;
    if (!Ctx.RequireArray(TEXT("samples"), SampleValues)) return true;

    // Existence before threshold (rpc-design.md §7): an empty array is its own outcome,
    // not "a length that failed the multiple-of-channels test".
    const int32 NumValues = SampleValues->Num();
    if (NumValues == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
            TEXT("'samples' is empty. An empty render and a silent one are different outcomes, so "
                 "this is an error rather than a zero-length asset."));
        return true;
    }
    if (NumValues > PwMaxInlineSamples)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("'samples' holds %d values; the inline cap is %d (one second of "
                                 "48 kHz stereo, which is also about where the 1 MB request-body "
                                 "limit lands). Longer audio goes through the candidate_id form of "
                                 "this verb, not inline JSON."),
                NumValues, PwMaxInlineSamples));
        return true;
    }

    int32 SourceChannels = 0;
    if (!Ctx.RequireInt(TEXT("channels"), SourceChannels)) return true;
    if (SourceChannels > PwExportChannels)
    {
        Ctx.SendError(ErrorCodes::ERR_AUDIO_MULTICHANNEL_UNSUPPORTED,
            FString::Printf(TEXT("channels=%d. FPwAudioBuffer is deinterleaved stereo, so anything "
                                 "above %d has no lossless landing place; the verb refuses rather "
                                 "than dropping channels the caller cannot see were dropped."),
                SourceChannels, PwExportChannels));
        return true;
    }
    if (SourceChannels < 1)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("channels=%d is not a channel count; pass 1 (mono) or 2 (stereo)."),
                SourceChannels));
        return true;
    }
    if ((NumValues % SourceChannels) != 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("'samples' holds %d values, which is not a whole number of %d-channel "
                                 "frames. Interleaved PCM must supply every channel of every frame."),
                NumValues, SourceChannels));
        return true;
    }

    int32 SampleRate = 0;
    if (!Ctx.RequireInt(TEXT("sampleRate"), SampleRate)) return true;
    if (SampleRate <= 0 || SampleRate > PwMaxSampleRate)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("sampleRate=%d is outside 1..%d Hz."), SampleRate, PwMaxSampleRate));
        return true;
    }

    // Deinterleave into the subsystem's currency. Out-of-range values are clamped here
    // rather than at export time so the verification below compares against exactly the
    // samples that were encoded; the count is reported so the clamp is never silent.
    const int32 NumFrames = NumValues / SourceChannels;
    FPwAudioBuffer Source;
    Source.SampleRate = SampleRate;
    Source.SetNumFrames(NumFrames, /*bZeroed=*/false);

    int32 ClampedSamples = 0;
    for (int32 Frame = 0; Frame < NumFrames; ++Frame)
    {
        const int32 LeftIndex = Frame * SourceChannels;
        const int32 RightIndex = (SourceChannels == 2) ? LeftIndex + 1 : LeftIndex;

        double Left = 0.0;
        double Right = 0.0;
        // A non-numeric element would otherwise read back as 0.0 and land in the asset as
        // a silent dropout the caller never hears about.
        if (!(*SampleValues)[LeftIndex]->TryGetNumber(Left) ||
            !(*SampleValues)[RightIndex]->TryGetNumber(Right))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("'samples' holds a non-numeric element at or near index %d; "
                                     "every element must be a JSON number."), LeftIndex));
            return true;
        }

        if (Left < -1.0 || Left > 1.0) { ++ClampedSamples; }
        if (SourceChannels == 2 && (Right < -1.0 || Right > 1.0)) { ++ClampedSamples; }

        Source.Left[Frame] = static_cast<float>(FMath::Clamp(Left, -1.0, 1.0));
        Source.Right[Frame] = static_cast<float>(FMath::Clamp(Right, -1.0, 1.0));
    }

    const bool bSave = Ctx.GetBool(TEXT("save"), true);
    const bool bOverwrite = Ctx.GetBool(TEXT("overwrite"), false);

    // Mandatory before any create path. IAssetTools/CreatePackage on an occupied path
    // can otherwise reach a modal overwrite prompt that wedges the game thread for every
    // client; Resolve guarantees no dialog on any branch. bRequireExactClass because a
    // USoundWaveProcedural / USoundSourceBus at the path passes IsA(USoundWave) but is a
    // different object layout - reconstructing one as a plain USoundWave is not an
    // update, it is corruption.
    const AssetCreatePolicy::FResolution Resolution = AssetCreatePolicy::Resolve(
        ValidatedPath, Name, USoundWave::StaticClass(), bOverwrite, /*bRequireExactClass=*/true);
    if (Resolution.IsRejected())
    {
        return AssetCreatePolicy::SendRejection(Ctx, Resolution);
    }

    // Both Create and UpdateInPlace go through the same call: the engine writer's
    // NewObject reconstructs an existing same-class wave in place, which is what makes a
    // retried request converge instead of accumulating assets (rpc-design.md §8). The
    // payload keeps the caller's width; a mono Source holds the same samples on both sides,
    // so the writer's mean of the two is the input itself, bit for bit.
    FString ExportError;
    FPwSoundWaveWriteReport WriteReport;
    USoundWave* Wave = PwCreateSoundWaveAsset(
        Source, ValidatedFolder, Name, bSave, WriteReport, ExportError, SourceChannels);
    if (!Wave)
    {
        Ctx.SendError(ErrorCodes::ERR_CREATION_FAILED, ExportError);
        return true;
    }

    const FString AssetPath = Wave->GetPathName();

    // ---------------------------------------------------------------------------
    // Verification (rpc-design.md §4): decode the asset back through the OTHER
    // subsystem and compare. Nothing below reads a field the writer assigned.
    // ---------------------------------------------------------------------------
    FPwAudioBuffer Decoded;
    FString DecodeCode;
    FString DecodeError;
    if (!PwDecodeSoundWaveWithCode(Wave, Decoded, DecodeCode, DecodeError))
    {
        TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
        AssetCreatePolicy::AddCreateReport(Failure, Resolution);
        AddAssetVerification(Failure, Wave);
        // After AddAssetVerification, never before: it writes its own top-level
        // "assetPath" (ResolveVerificationAssetPath, AssetUtils.cpp:1346) which for a
        // top-level asset is the bare PACKAGE path, silently replacing the object path
        // this verb publishes. Callers chain audio.analysis.* on this handle, and those
        // resolve an object path.
        Failure->SetStringField(TEXT("assetPath"), AssetPath);
        Ctx.SendError(DecodeCode,
            FString::Printf(TEXT("The asset was written but could not be decoded back, so nothing "
                                 "about its contents is verified: %s"), *DecodeError),
            Failure);
        return true;
    }

    // The channel count is the one thing the deinterleaved buffer cannot carry (mono is
    // duplicated into both sides), so it is read straight off the payload header - still
    // the parse side, never the write side.
    TArray<uint8> PayloadPcm;
    uint32 PayloadSampleRate = 0;
    uint16 PayloadChannels = 0;
    const bool bReadPayloadHeader =
        Wave->GetImportedSoundWaveData(PayloadPcm, PayloadSampleRate, PayloadChannels);

    const FPwChannelSignature SourceLeft = MeasureChannel(Source.Left);
    const FPwChannelSignature SourceRight = MeasureChannel(Source.Right);
    const FPwChannelSignature DecodedLeft = MeasureChannel(Decoded.Left);
    const FPwChannelSignature DecodedRight = MeasureChannel(Decoded.Right);

    const bool bFramesMatch = Decoded.NumFrames() == Source.NumFrames();
    const bool bRateMatch = Decoded.SampleRate == Source.SampleRate;
    const bool bChannelsMatch = bReadPayloadHeader &&
        static_cast<int32>(PayloadChannels) == SourceChannels;

    TSharedPtr<FJsonObject> Verification = MakeShared<FJsonObject>();
    Verification->SetBoolField(TEXT("measured"), true);
    Verification->SetStringField(TEXT("method"),
        TEXT("decoded back through USoundWave::GetImportedSoundWaveData (PwDecodeSoundWave), "
             "which parses the RIFF payload independently of the FSoundWavePCMWriter path that "
             "wrote it"));
    Verification->SetNumberField(TEXT("toleranceAbs"), PwRoundTripToleranceAbs);
    Verification->SetBoolField(TEXT("framesMatch"), bFramesMatch);
    Verification->SetNumberField(TEXT("sourceFrames"), Source.NumFrames());
    Verification->SetNumberField(TEXT("decodedFrames"), Decoded.NumFrames());
    Verification->SetBoolField(TEXT("sampleRateMatch"), bRateMatch);
    Verification->SetNumberField(TEXT("sourceSampleRate"), Source.SampleRate);
    Verification->SetNumberField(TEXT("decodedSampleRate"), Decoded.SampleRate);
    Verification->SetBoolField(TEXT("channelsMatch"), bChannelsMatch);
    Verification->SetNumberField(TEXT("expectedChannels"), SourceChannels);
    Verification->SetNumberField(TEXT("payloadChannels"), bReadPayloadHeader ? PayloadChannels : 0);
    Verification->SetBoolField(TEXT("payloadHeaderRead"), bReadPayloadHeader);

    const bool bLeftMatch = AddChannelComparison(Verification, TEXT("left"), SourceLeft, DecodedLeft);
    const bool bRightMatch = AddChannelComparison(Verification, TEXT("right"), SourceRight, DecodedRight);

    // A rewrite that moved a property the payload does not own fails the verb: the audio can be
    // exact and the asset still be broken, because a wave that lost its SoundClass escapes every
    // SoundMix and one that lost its attenuation plays at full level at any distance - neither of
    // which a frames/rate/RMS comparison can see (board
    // B-synth-export-wipes-soundclass-attenuation).
    const bool bPropertiesPreserved = WriteReport.ChangedProperties.Num() == 0;

    const bool bVerified = bFramesMatch && bRateMatch && bChannelsMatch && bLeftMatch &&
        bRightMatch && bPropertiesPreserved;
    Verification->SetBoolField(TEXT("pass"), bVerified);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetName"), Name);
    // Every one of these is read off the decode, not off the wave's own fields: Duration,
    // TotalSamples and RawPCMDataSize are engine-derived and this verb never writes or
    // reports them (rpc-design.md §5a).
    Result->SetNumberField(TEXT("frames"), Decoded.NumFrames());
    Result->SetNumberField(TEXT("sampleRate"), Decoded.SampleRate);
    Result->SetNumberField(TEXT("channels"), bReadPayloadHeader ? PayloadChannels : 0);
    Result->SetNumberField(TEXT("durationSeconds"), Decoded.DurationSeconds());
    Result->SetNumberField(TEXT("sourceChannels"), SourceChannels);
    if (ClampedSamples > 0)
    {
        Result->SetNumberField(TEXT("clampedSamples"), ClampedSamples);
    }
    Result->SetObjectField(TEXT("verification"), Verification);
    // "routing" on the result, "propertiesPreserved" on the verification. Emitted on a create
    // too: an empty soundClass there is not a loss, but it is the same unrouted asset at the end
    // of it, and the response is the only place the caller sees it.
    PwAddSoundWaveWriteReport(Result, Verification, WriteReport);

    // Same {saveRequested, saved, pendingFlush} triple from both families, so a caller
    // never has to know which one ran (rpc-design.md §5).
    if (bSave)
    {
        AddAssetSaveReport(Result, /*bSaveRequested=*/true, WriteReport.bSavedToDisk);
    }
    else
    {
        AddMarkDirtySaveReport(Result, Wave, /*bSaveRequested=*/false);
    }
    AssetCreatePolicy::AddCreateReport(Result, Resolution);
    AddAssetVerification(Result, Wave);
    // Same ordering rule as the decode-failure branch above: AddAssetVerification's own
    // top-level "assetPath" is the bare package path, so the object path this verb
    // reports has to be written after it, not before.
    Result->SetStringField(TEXT("assetPath"), AssetPath);

    if (!bVerified)
    {
        // Two failure shapes share one code, so the message names which one happened.
        const FString PropertyClause = bPropertiesPreserved
            ? FString()
            : FString::Printf(
                TEXT(" The rewrite also changed %d propert%s the payload does not own (%s); see "
                     "verification.changedProperties."),
                WriteReport.ChangedProperties.Num(),
                WriteReport.ChangedProperties.Num() == 1 ? TEXT("y") : TEXT("ies"),
                *FString::JoinBy(WriteReport.ChangedProperties, TEXT(", "),
                    [](const FName& PropertyName) { return PropertyName.ToString(); }));

        Ctx.SendError(ErrorCodes::ERR_VERIFICATION_FAILED,
            FString::Printf(TEXT("'%s' was written but the decode-back disagrees with the source "
                                 "buffer (frames %d vs %d, %d Hz vs %d Hz, %d channels vs %d). The "
                                 "asset exists; its contents are not what was requested.%s"),
                *AssetPath, Source.NumFrames(), Decoded.NumFrames(),
                Source.SampleRate, Decoded.SampleRate,
                SourceChannels, bReadPayloadHeader ? static_cast<int32>(PayloadChannels) : 0,
                *PropertyClause),
            Result);
        return true;
    }

    Ctx.SendSuccess(Result);
    return true;
}

// Tick-unsafe for the reasons audio.authoring.create_sound_wave_from_pcm is listed in
// Dispatch/SafePoint.cpp: the same writer, the same in-place rewrite and save, plus the decode
// before and after.
REGISTER_RPC_HANDLER_TICK_UNSAFE("audio.authoring.set_sound_wave_gain", "audio.authoring",
    "Rescale an existing USoundWave's PCM in place to a peak (dBFS) or integrated-loudness (LUFS) target. One gain is applied to every channel, so the channel count, the stereo image and every non-payload property (SoundClass, attenuation, looping, volume and the rest) plus the cue points and timecode are left exactly as they were - the safe route for a loudness fix, where re-exporting through an audio.synth `sample` layer collapses the wave to mono. Verified by decoding the rewritten asset back: the measured level must land within toleranceDb of the target, each channel must have moved by the same gain, and verification.propertiesPreserved must hold, or the call fails with VERIFICATION_FAILED. A target that would push the peak past full scale is refused before anything is written, with the highest reachable target in the error payload. LUFS figures use audio.analysis' convention (a mono wave is measured as dual-mono), so a target read off audio.analysis.analyze means the same thing here.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "USoundWave to rescale, under /Game, e.g. /Game/Audio/SW_Impact."),
        RPC_PARAM_REQ("mode", "string", "What `target` measures: 'peak' (sample peak, dBFS) or 'lufs' (gated BS.1770 integrated loudness). Required: -16 dBFS and -16 LUFS are different requests. lufs needs about 0.5 s of audio."),
        RPC_PARAM_REQ("target", "number", "Level to reach, in the unit `mode` names. A peak target must be <= 0 dBFS."),
        RPC_PARAM_DEF("save", "boolean", "Write the .uasset to disk. false marks the package dirty only, and the response reports saved:false / pendingFlush:true.", "true")
    ))
{
    using namespace PwSoundWavePcmHandlerInternal;

    FString RawPath;
    if (!Ctx.RequireAssetPath(TEXT("assetPath"), RawPath)) return true;
    FString Mode;
    if (!Ctx.RequireString(TEXT("mode"), Mode)) return true;
    double Target = 0.0;
    if (!Ctx.RequireNumber(TEXT("target"), Target)) return true;

    const bool bPeakMode = Mode.Equals(TEXT("peak"), ESearchCase::IgnoreCase);
    if (!bPeakMode && !Mode.Equals(TEXT("lufs"), ESearchCase::IgnoreCase))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("mode='%s'; pass 'peak' or 'lufs'."), *Mode));
        return true;
    }
    if (!FMath::IsFinite(Target) || (bPeakMode && Target > 0.0))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("target=%g is not a reachable %s level%s."), Target,
                bPeakMode ? TEXT("peak") : TEXT("loudness"),
                bPeakMode ? TEXT(": a sample peak above 0 dBFS clips") : TEXT("")));
        return true;
    }

    FString ObjectPath;
    FString PathError;
    if (!NormalizeToObjectPath(RawPath, ObjectPath, PathError))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PATH, PathError);
        return true;
    }

    USoundWave* Wave =
        Cast<USoundWave>(StaticLoadObject(USoundWave::StaticClass(), nullptr, *ObjectPath));
    if (!Wave)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
            FString::Printf(TEXT("No USoundWave at '%s'."), *ObjectPath));
        return true;
    }
    // Exact class: a procedural wave or source bus owns no stored PCM, and the writer's in-place
    // path is only defined for a plain USoundWave.
    if (Wave->GetClass() != USoundWave::StaticClass())
    {
        Ctx.SendError(ErrorCodes::ERR_AUDIO_PROCEDURAL_UNSUPPORTED,
            FString::Printf(TEXT("'%s' is a %s, which carries no stored samples to rescale."),
                *ObjectPath, *Wave->GetClass()->GetName()));
        return true;
    }

    const FString PackageName = Wave->GetOutermost()->GetName();
    const FString Folder = FPackageName::GetLongPackagePath(PackageName);
    const FString Name = Wave->GetName();
    if (!Folder.Equals(TEXT("/Game")) && !Folder.StartsWith(TEXT("/Game/")))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PATH,
            FString::Printf(TEXT("'%s' is outside /Game; the SoundWave writer only reaches /Game "
                                 "content."), *ObjectPath));
        return true;
    }

    // Channel count off the payload header: the decoded buffer cannot carry it (mono duplicates
    // into both sides), and writing back at any other width would be the image change this verb
    // exists not to make.
    TArray<uint8> HeaderPcm;
    uint32 HeaderRate = 0;
    uint16 HeaderChannels = 0;
    FPwAudioBuffer Buffer;
    FString DecodeCode;
    FString DecodeError;
    if (!PwDecodeSoundWaveWithCode(Wave, Buffer, DecodeCode, DecodeError) ||
        !Wave->GetImportedSoundWaveData(HeaderPcm, HeaderRate, HeaderChannels))
    {
        Ctx.SendError(DecodeCode.IsEmpty() ? FString(ErrorCodes::ERR_AUDIO_UNSUPPORTED_FORMAT)
                                           : DecodeCode,
            DecodeError.IsEmpty()
                ? FString::Printf(TEXT("'%s' has no readable payload header."), *ObjectPath)
                : DecodeError);
        return true;
    }
    HeaderPcm.Empty();
    const int32 Channels = static_cast<int32>(HeaderChannels);

    FString LufsCode;
    FString LufsError;
    const FPwLevelReading Before = ReadLevel(Buffer, LufsCode, LufsError);
    if (Before.PeakLinear <= 0.0)
    {
        Ctx.SendError(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
            FString::Printf(TEXT("'%s' is digitally silent; no finite gain reaches a target."),
                *ObjectPath));
        return true;
    }
    if (!bPeakMode && !Before.bLufsMeasured)
    {
        Ctx.SendError(LufsCode.IsEmpty() ? FString(ErrorCodes::ERR_INVALID_ARGUMENT) : LufsCode,
            FString::Printf(TEXT("The loudness of '%s' could not be measured, so a LUFS target "
                                 "cannot be applied (use mode 'peak'): %s"), *ObjectPath, *LufsError));
        return true;
    }

    const double MeasuredDb = bPeakMode ? LinearToDbOrFloor(Before.PeakLinear) : Before.IntegratedLufs;
    const double GainDb = Target - MeasuredDb;
    const double GainLinear = FMath::Pow(10.0, GainDb / 20.0);

    // Refused, never clamped: a clamp would change the waveform, not only its level. Only a lufs
    // target can get here - a peak target <= 0 dBFS lands at or below 1.0 - and the slack keeps
    // a 0 dBFS peak target from being refused over the last bit of a double.
    const double ResultingPeak = Before.PeakLinear * GainLinear;
    if (ResultingPeak > 1.0 + 1.0e-9)
    {
        const double MaxTarget = Target - LinearToDbOrFloor(ResultingPeak);
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("assetPath"), ObjectPath);
        Data->SetObjectField(TEXT("before"), LevelToJson(Before));
        Data->SetNumberField(TEXT("gainDb"), GainDb);
        Data->SetNumberField(TEXT("resultingPeakDb"), LinearToDbOrFloor(ResultingPeak));
        Data->SetNumberField(TEXT("maxTarget"), MaxTarget);
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Reaching %.2f LUFS needs %+.2f dB, which puts the peak at %+.2f "
                                 "dBFS and would clip. The highest reachable target is %.2f LUFS; "
                                 "nothing was written."),
                Target, GainDb, LinearToDbOrFloor(ResultingPeak), MaxTarget),
            Data);
        return true;
    }

    for (float& Sample : Buffer.Left)
    {
        Sample = static_cast<float>(Sample * GainLinear);
    }
    for (float& Sample : Buffer.Right)
    {
        Sample = static_cast<float>(Sample * GainLinear);
    }

    // The policy decides, even though the occupant is the very wave just loaded: it is what keeps
    // every write path in this subsystem free of a modal overwrite prompt.
    const AssetCreatePolicy::FResolution Resolution = AssetCreatePolicy::Resolve(
        PackageName, Name, USoundWave::StaticClass(), /*bOverwriteRequested=*/false,
        /*bRequireExactClass=*/true);
    if (Resolution.IsRejected())
    {
        return AssetCreatePolicy::SendRejection(Ctx, Resolution);
    }

    const bool bSave = Ctx.GetBool(TEXT("save"), true);
    FString ExportError;
    FPwSoundWaveWriteReport WriteReport;
    // Same frames, same rate: cue points, timecode and the imported rate still describe this
    // audio, so they are kept - and then diffed with every other authored property.
    USoundWave* Written = PwCreateSoundWaveAsset(
        Buffer, Folder, Name, bSave, WriteReport, ExportError, Channels,
        /*bKeepSourceMarkers=*/true);
    if (!Written)
    {
        Ctx.SendError(ErrorCodes::ERR_CREATION_FAILED, ExportError);
        return true;
    }

    // ---------------------------------------------------------------------------
    // Verification: decode the rewritten asset and measure it again. The level is
    // compared with the TARGET, and each side's RMS with the side it came from, so a
    // wrong gain, a collapsed image and a changed width all fail independently.
    // ---------------------------------------------------------------------------
    FPwAudioBuffer Decoded;
    TArray<uint8> AfterPcm;
    uint32 AfterRate = 0;
    uint16 AfterChannels = 0;
    const bool bDecoded = PwDecodeSoundWaveWithCode(Written, Decoded, DecodeCode, DecodeError) &&
        Written->GetImportedSoundWaveData(AfterPcm, AfterRate, AfterChannels);
    AfterPcm.Empty();

    TSharedPtr<FJsonObject> Verification = MakeShared<FJsonObject>();
    Verification->SetBoolField(TEXT("measured"), bDecoded);
    Verification->SetStringField(TEXT("method"),
        TEXT("decoded back through USoundWave::GetImportedSoundWaveData and re-measured"));
    Verification->SetNumberField(TEXT("toleranceDb"), PwGainToleranceDb);

    FPwLevelReading After;
    bool bVerified = false;
    if (bDecoded)
    {
        After = ReadLevel(Decoded, LufsCode, LufsError);
        const double AchievedDb = bPeakMode ? LinearToDbOrFloor(After.PeakLinear)
                                            : After.IntegratedLufs;
        const bool bLevelMeasured = bPeakMode || After.bLufsMeasured;
        const bool bOnTarget = bLevelMeasured && FMath::Abs(AchievedDb - Target) <= PwGainToleranceDb;

        // Per-side gain, measured: equal on both sides is what "the image was left alone" means.
        // A silent side has no gain to measure and is not held against the call.
        const auto SideGainDb = [](double BeforeRms, double AfterRms)
        {
            return (BeforeRms > 0.0 && AfterRms > 0.0)
                ? 20.0 * FMath::LogX(10.0, AfterRms / BeforeRms) : 0.0;
        };
        const double LeftGainDb = SideGainDb(Before.LeftRms, After.LeftRms);
        const double RightGainDb = SideGainDb(Before.RightRms, After.RightRms);
        const bool bLeftOk = Before.LeftRms <= 0.0 || FMath::Abs(LeftGainDb - GainDb) <= PwGainToleranceDb;
        const bool bRightOk = Before.RightRms <= 0.0 || FMath::Abs(RightGainDb - GainDb) <= PwGainToleranceDb;

        const bool bFramesMatch = Decoded.NumFrames() == Buffer.NumFrames();
        const bool bChannelsMatch = static_cast<int32>(AfterChannels) == Channels;
        const bool bPropertiesPreserved = WriteReport.ChangedProperties.Num() == 0;

        if (bLevelMeasured)
        {
            Verification->SetNumberField(TEXT("achieved"), AchievedDb);
        }
        Verification->SetBoolField(TEXT("onTarget"), bOnTarget);
        Verification->SetNumberField(TEXT("leftGainDb"), LeftGainDb);
        Verification->SetNumberField(TEXT("rightGainDb"), RightGainDb);
        Verification->SetBoolField(TEXT("imagePreserved"), bLeftOk && bRightOk);
        Verification->SetBoolField(TEXT("framesMatch"), bFramesMatch);
        Verification->SetBoolField(TEXT("channelsMatch"), bChannelsMatch);
        bVerified = bOnTarget && bLeftOk && bRightOk && bFramesMatch && bChannelsMatch &&
            bPropertiesPreserved;
    }
    else
    {
        Verification->SetStringField(TEXT("errorCode"), DecodeCode);
        Verification->SetStringField(TEXT("error"), DecodeError);
    }
    Verification->SetBoolField(TEXT("pass"), bVerified);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("mode"), bPeakMode ? TEXT("peak") : TEXT("lufs"));
    Result->SetNumberField(TEXT("target"), Target);
    Result->SetNumberField(TEXT("gainDb"), GainDb);
    Result->SetNumberField(TEXT("channels"), Channels);
    Result->SetNumberField(TEXT("frames"), Buffer.NumFrames());
    Result->SetObjectField(TEXT("before"), LevelToJson(Before));
    if (bDecoded)
    {
        Result->SetObjectField(TEXT("after"), LevelToJson(After));
    }
    Result->SetObjectField(TEXT("verification"), Verification);
    PwAddSoundWaveWriteReport(Result, Verification, WriteReport);
    if (bSave)
    {
        AddAssetSaveReport(Result, /*bSaveRequested=*/true, WriteReport.bSavedToDisk);
    }
    else
    {
        AddMarkDirtySaveReport(Result, Written, /*bSaveRequested=*/false);
    }
    AddAssetVerification(Result, Written);
    // After AddAssetVerification, which writes the bare package path under the same key.
    Result->SetStringField(TEXT("assetPath"), Written->GetPathName());

    if (!bVerified)
    {
        // A decode-back that failed outright carries the decoder's own code, as in
        // audio.synth.export and audio.synth.render_metasound.
        Ctx.SendError((bDecoded || DecodeCode.IsEmpty())
                ? FString(ErrorCodes::ERR_VERIFICATION_FAILED) : DecodeCode,
            FString::Printf(TEXT("'%s' was rewritten but does not verify (see verification): the "
                                 "level, the per-channel gain, the width or a non-payload property "
                                 "is not what was requested."), *Written->GetPathName()),
            Result);
        return true;
    }

    Ctx.SendSuccess(FString::Printf(TEXT("Applied %+.2f dB to %s (%d ch)."),
        GainDb, *Written->GetPathName(), Channels), Result);
    return true;
}
