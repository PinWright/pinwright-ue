// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "AudioGen/PwAudioExport.h"

#include "AudioGen/PwAudioBuffer.h"

#include "Audio.h"
#include "AudioDeviceManager.h"
#include "Compat/EngineVersionCompat.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Memory/SharedBuffer.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Sound/SampleBufferIO.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundWave.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

// Named (not anonymous) namespace: the main module builds with Unity on, and two
// anonymous namespaces merged into one TU collide by name. See CLAUDE.md > Building.
namespace PwAudioExportInternal
{
    // Deinterleaved stereo float -> interleaved float, clamped to [-1, 1].
    //
    // The clamp belongs HERE and not to the conversion that follows it: the
    // TSampleBuffer<int16> = TSampleBuffer<float> assignment runs
    // Audio::ArrayFloatToPcm16, which multiplies by 32767 and truncates with no
    // range check at all (FloatArrayMath.cpp:2471-2491), so a sample at 1.5 wraps
    // to a large negative int16 - a loud click at the exact moment the caller
    // over-drove the signal. TSampleBuffer's direct float->int16 CONSTRUCTOR does
    // clamp (SampleBuffer.h:89), but the writer needs the assignment path, so the
    // clamp cannot be inherited from it.
    //
    // Returns false for an empty or inconsistent buffer; Out is emptied first so a
    // rejected buffer cannot leave a partially-filled array behind.
    bool BuildInterleavedClamped(const FPwAudioBuffer& In, TArray<float>& Out)
    {
        Out.Reset();

        const int32 NumFrames = In.NumFrames();
        if (NumFrames <= 0 || !In.IsValid())
        {
            return false;
        }

        Out.SetNumUninitialized(NumFrames * PwExportChannels);

        const float* RESTRICT LeftData = In.Left.GetData();
        const float* RESTRICT RightData = In.Right.GetData();
        float* RESTRICT OutData = Out.GetData();

        for (int32 Frame = 0; Frame < NumFrames; ++Frame)
        {
            OutData[Frame * PwExportChannels] = FMath::Clamp(LeftData[Frame], -1.0f, 1.0f);
            OutData[Frame * PwExportChannels + 1] = FMath::Clamp(RightData[Frame], -1.0f, 1.0f);
        }
        return true;
    }

    // Interleaved clamped float -> the int16 TSampleBuffer the engine writer accepts.
    // Routed through TSampleBuffer<float> + the converting assignment operator, which
    // is the only conversion the engine itself exposes for this direction.
    bool BuildInt16SampleBuffer(const FPwAudioBuffer& In, Audio::TSampleBuffer<int16>& Out)
    {
        TArray<float> Interleaved;
        if (!BuildInterleavedClamped(In, Interleaved))
        {
            return false;
        }

        const Audio::TSampleBuffer<float> FloatBuffer(
            Interleaved.GetData(), Interleaved.Num(), PwExportChannels, In.SampleRate);
        Out = FloatBuffer;
        return Out.GetNumSamples() > 0;
    }

    // "/Game/Audio/Fx" -> "Audio/Fx"; "/Game" -> "". Anything outside /Game is
    // rejected rather than passed on: SynchronouslyWriteSoundWave prepends "/Game/"
    // to whatever it is handed (SampleBufferIO.cpp:344-348) and, failing that, drops
    // the asset at the Content root with a log warning and no error return (:358-364).
    // Either way the caller would be told nothing and the asset would be somewhere
    // else - rpc-design.md §1's "write landed in a slot the verb did not name".
    bool MakeGameRelativeFolder(const FString& PackagePath, FString& OutRelative, FString& OutError)
    {
        FString Folder = PackagePath;
        Folder.ReplaceInline(TEXT("\\"), TEXT("/"));
        while (Folder.EndsWith(TEXT("/")))
        {
            Folder.LeftChopInline(1);
        }

        if (Folder.Equals(TEXT("/Game"), ESearchCase::CaseSensitive))
        {
            OutRelative.Empty();
            return true;
        }

        if (!Folder.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive))
        {
            OutError = FString::Printf(
                TEXT("SoundWave assets can only be created under /Game; '%s' is not. The engine ")
                TEXT("writer hardcodes a /Game/ prefix, so any other mount root would silently ")
                TEXT("land the asset somewhere else."),
                *PackagePath);
            return false;
        }

        OutRelative = Folder.RightChop(6);  // strip the leading "/Game/"
        return true;
    }

    // Plant the RIFF payload the editor decode path actually reads. Shared shape with
    // FSoundWavePCMWriter::SerializeSoundWaveToAsset (SampleBufferIO.cpp:563-577), which
    // is why the transient wave and the asset wave decode identically.
    //
    // Deliberately does NOT assign Duration / TotalSamples / RawPCMDataSize
    // (rpc-design.md §5a): FAsyncAudioDecompressWorker derives all three from this
    // payload on first precache (AudioDecompress.cpp:897-900). SampleRate and NumChannels
    // ARE set, because the precache needs them to pick a decoder before it can derive
    // anything.
    void ApplyWavPayload(USoundWave* Wave, const Audio::TSampleBuffer<int16>& Samples)
    {
        TArray<uint8> WavData;
        SerializeWaveFile(WavData,
            reinterpret_cast<const uint8*>(Samples.GetData()),
            Samples.GetNumSamples() * static_cast<int32>(sizeof(int16)),
            Samples.GetNumChannels(),
            Samples.GetSampleRate());

        Wave->RawData.UpdatePayload(FSharedBuffer::Clone(WavData.GetData(), WavData.Num()), Wave);
        Wave->SetSampleRate(static_cast<uint32>(Samples.GetSampleRate()));
        Wave->NumChannels = Samples.GetNumChannels();
    }

    // The UPROPERTYs an in-place rewrite owns, on USoundBase and USoundWave both. Everything
    // else on a USoundWave belongs to whoever authored the asset - mix routing, concurrency,
    // submix and bus sends, modulation, loading behaviour, subtitles, curves, user data - and a
    // rewrite must leave it alone.
    //
    // One list, used twice: UpdateSoundWaveInPlace assigns exactly these, and
    // CaptureNonPayloadProperties excludes exactly these from the before/after diff. That
    // pairing is what makes the omission direction safe (rpc-design.md §4) - a field this list
    // forgets that the update writes anyway surfaces as a FAILED verification naming the field,
    // never as a silent loss the caller learns about from a later asset.dump.
    //
    // RawData is absent because it is not a UPROPERTY (SoundWave.h:957 - editor bulk data,
    // serialized by hand), and so is CompressedDataGuid (:1119), which InvalidateCompressedData
    // rerolls. Neither is ever seen by the reflection walk below.
    const TSet<FName>& PayloadOwnedProperties()
    {
        static const TSet<FName> Names = {
            // USoundBase - derived from the sample data or from the source file's timecode.
            FName(TEXT("Duration")), FName(TEXT("TotalSamples")), FName(TEXT("TimecodeOffset")),
            // USoundWave - the payload's own format.
            FName(TEXT("NumChannels")), FName(TEXT("SampleRate")), FName(TEXT("ImportedSampleRate")),
            // USoundWave - source state parsed out of the payload.
            FName(TEXT("ChannelOffsets")), FName(TEXT("ChannelSizes")), FName(TEXT("bIsAmbisonics")),
            FName(TEXT("CuePoints")), FName(TEXT("CuePointOrigin")), FName(TEXT("TimecodeInfo"))
        };
        return Names;
    }

    // Exported text of every UPROPERTY the payload does not own, keyed by name.
    //
    // Text rather than a duplicated object: StaticDuplicateObject would copy the bulk payload
    // and run the asset's duplication hooks to answer a question that is only "did any of these
    // values change". Transient properties are skipped because they are runtime bookkeeping that
    // the DDC cache kicked off below is entitled to move under us.
    //
    // Index 0 only. USoundWave declares no static array (ArrayDim > 1) property; one appearing
    // later would be compared on its first element, which is a weaker check, not a wrong one.
    TMap<FName, FString> CaptureNonPayloadProperties(const USoundWave* Wave)
    {
        TMap<FName, FString> Values;
        if (!Wave)
        {
            return Values;
        }

        for (TFieldIterator<FProperty> It(Wave->GetClass()); It; ++It)
        {
            const FProperty* Property = *It;
            if (Property->HasAnyPropertyFlags(CPF_Transient) ||
                PayloadOwnedProperties().Contains(Property->GetFName()))
            {
                continue;
            }

            FString Text;
            Property->ExportText_InContainer(0, Text, Wave, /*Delta=*/nullptr,
                const_cast<USoundWave*>(Wave), PPF_None);
            Values.Add(Property->GetFName(), MoveTemp(Text));
        }
        return Values;
    }

    /** Names whose exported value no longer matches Before, sorted so the report is stable. */
    TArray<FName> DiffNonPayloadProperties(const TMap<FName, FString>& Before,
                                           const USoundWave* Wave)
    {
        const TMap<FName, FString> After = CaptureNonPayloadProperties(Wave);

        TArray<FName> Changed;
        for (const TPair<FName, FString>& Pair : Before)
        {
            const FString* Now = After.Find(Pair.Key);
            if (!Now || *Now != Pair.Value)
            {
                Changed.Add(Pair.Key);
            }
        }
        Changed.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
        return Changed;
    }

    // Rewrite Wave's audio WITHOUT reconstructing the object, which is what keeps every
    // non-payload property (see PwAudioExport.h). Assigns only what the payload owns.
    void UpdateSoundWaveInPlace(USoundWave* Wave, const Audio::TSampleBuffer<int16>& Samples)
    {
        // Source state parsed out of the OLD payload. Left behind it would describe audio the
        // asset no longer holds: markers pointing past the end of a shorter take, a multichannel
        // layout for a stereo payload, a timecode from a file that is no longer the source.
        // This is the set the engine's own reimport-over-an-existing-wave path resets while
        // preserving everything else (SoundFactory.cpp:657-763).
        Wave->ChannelOffsets.Reset();
        Wave->ChannelSizes.Reset();
        Wave->bIsAmbisonics = false;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        Wave->SetSoundWaveCuePoints(TArray<FSoundWaveCuePoint>());
#else
        // USoundWave gained SetSoundWaveCuePoints in 5.6. Before that CuePoints is a protected
        // editor-only UPROPERTY with no accessor, so clear the reflected field: the same property
        // the 5.6 setter assigns, and the one the engine re-derives the published
        // FSoundWaveData copy from when platform data is next updated.
        if (const FProperty* CuePointsProperty =
                USoundWave::StaticClass()->FindPropertyByName(TEXT("CuePoints")))
        {
            CuePointsProperty->ContainerPtrToValuePtr<TArray<FSoundWaveCuePoint>>(Wave)->Reset();
        }
#endif
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        Wave->SetCuePointOrigin(ESoundWaveCuePointOrigin::MarkerTransformation);
#elif UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        // USoundWave gained SetCuePointOrigin in 5.7. On 5.6 the CuePointOrigin UPROPERTY is
        // protected with no accessor, so write the reflected field: same property, same value the
        // 5.7 setter assigns. Before 5.6 neither the property nor ESoundWaveCuePointOrigin
        // exists - those engines have no cue-point origin at all - so nothing is written, which
        // costs nothing on an emptied cue-point set.
        if (const FProperty* CuePointOriginProperty =
                USoundWave::StaticClass()->FindPropertyByName(TEXT("CuePointOrigin")))
        {
            *CuePointOriginProperty->ContainerPtrToValuePtr<ESoundWaveCuePointOrigin>(Wave) =
                ESoundWaveCuePointOrigin::MarkerTransformation;
        }
#endif
        Wave->SetTimecodeInfo(FSoundWaveTimecodeInfo{});

        // 0 means "not imported from a file", which is what a wave this layer writes is.
        // USoundWave::PostLoad re-derives it from the payload header when it is 0
        // (SoundWave.cpp:2309-2319); a rate left over from an earlier import would instead
        // rescale this payload's cue points and drive the cook's resample decision from a
        // number that no longer describes the audio. It is also what the create path leaves.
        Wave->SetImportedSampleRate(0);

        ApplyWavPayload(Wave, Samples);

        // The fields FSoundWavePCMWriter::ApplyBufferToSoundWave assigns on the create path
        // (SampleBufferIO.cpp:534-539), assigned here for the same reason: they are read before
        // any precache derives them, and the previous payload's values are simply wrong.
        Wave->Duration = static_cast<float>(Samples.GetNumFrames()) /
                         static_cast<float>(Samples.GetSampleRate());
        Wave->TotalSamples = static_cast<float>(Samples.GetNumSamples());
        Wave->RawPCMDataSize = Samples.GetNumSamples() * static_cast<int32>(sizeof(int16));

        // RawPCMData is the decompressed copy of the OLD audio, which an editor preview would
        // otherwise play back over the new payload. The writer frees it too (:766).
        if (Wave->RawPCMData)
        {
            FMemory::Free(Wave->RawPCMData);
            Wave->RawPCMData = nullptr;
        }

        // Same call the writer makes after its own asset write (SampleBufferIO.cpp:400): the DDC
        // entry, the streaming chunks and the decompressed resources all describe the old
        // payload. SetSoundAssetCompressionType is deliberately NOT re-forced to PCM here the
        // way the create path forces it - the codec is the asset author's choice, not payload,
        // and re-forcing it would be exactly the kind of silent reset this path exists to stop.
        Wave->InvalidateCompressedData(/*bFreeResources=*/true);
    }
}

USoundWave* PwCreateSoundWaveAsset(const FPwAudioBuffer& In, const FString& PackagePath,
                                   const FString& AssetName, bool bSaveToDisk,
                                   FPwSoundWaveWriteReport& OutReport, FString& OutError)
{
    // Failure is the default on every early-out (rpc-design.md §2).
    OutReport = FPwSoundWaveWriteReport();
    OutError.Reset();

    if (!IsInGameThread())
    {
        OutError = TEXT("PwCreateSoundWaveAsset must run on the game thread: it creates a package "
                        "and touches the asset registry.");
        return nullptr;
    }

    if (!GIsEditor)
    {
        OutError = TEXT("SoundWave assets can only be authored in the editor; "
                        "FSoundWavePCMWriter's asset path is GIsEditor-gated "
                        "(SampleBufferIO.cpp:340).");
        return nullptr;
    }

    if (AssetName.IsEmpty())
    {
        OutError = TEXT("Asset name is empty.");
        return nullptr;
    }

    FString RelativeFolder;
    if (!PwAudioExportInternal::MakeGameRelativeFolder(PackagePath, RelativeFolder, OutError))
    {
        return nullptr;
    }

    // The int16 buffer and the writer share one lifetime through the engine's own
    // FAudioRecordingData pairing (SampleBufferIO.h:261). The write below is
    // synchronous, so nothing outlives this frame - the pairing is kept because the
    // writer copies the buffer by value and reading a stale InputBuffer afterwards
    // would be the trap this struct exists to close.
    Audio::FAudioRecordingData Recording;
    if (!PwAudioExportInternal::BuildInt16SampleBuffer(In, Recording.InputBuffer))
    {
        OutError = FString::Printf(
            TEXT("Audio buffer holds nothing to write (frames=%d, left=%d, right=%d, rate=%d Hz)."),
            In.NumFrames(), In.Left.Num(), In.Right.Num(), In.SampleRate);
        return nullptr;
    }

    const FString ExpectedPackage = RelativeFolder.IsEmpty()
        ? FString::Printf(TEXT("/Game/%s"), *AssetName)
        : FString::Printf(TEXT("/Game/%s/%s"), *RelativeFolder, *AssetName);

    // Whichever branch runs below, nothing may be reading this wave's payload while it changes.
    // Tearing down - or rewriting under - a USoundWave the audio device is still playing is the
    // hazard the engine guards in its own in-place overload
    // (BeginGeneratingSoundWaveFromBuffer, SampleBufferIO.cpp:142-146); the
    // SynchronouslyWriteSoundWave path has no such guard, so it is applied here.
    USoundWave* Existing = FindObject<USoundWave>(nullptr,
        *FString::Printf(TEXT("%s.%s"), *ExpectedPackage, *AssetName));
    if (Existing)
    {
        if (GEngine)
        {
            if (FAudioDeviceManager* DeviceManager = GEngine->GetAudioDeviceManager())
            {
                DeviceManager->StopSoundsUsingResource(Existing);
            }
        }

        // The audio device is not the only thing still holding this wave. A previous export left
        // an async DDC cook running on the asset thread pool (SetSoundAssetCompressionType /
        // InvalidateCompressedData -> CachePlatformData(bAsyncCache=true)), and FAudioCookInputs
        // holds Existing->RawDataCriticalSection / Existing->RawData by raw REFERENCE for that
        // cook's whole life (AudioDerivedData.cpp:1659-1664). Either branch below invalidates
        // that RawData underneath it, and the reconstruct branch trashes the object outright so
        // GC runs ~FCriticalSection while the pool worker is still inside FScopeLock at
        // AudioDerivedData.cpp:1817 - an access violation on a background thread. Join it first.
        Existing->FinishCachePlatformData();
    }

    USoundWave* Wave = nullptr;

    // The in-place half of the idempotence contract (see the header). Exact class only: a
    // USoundWaveProcedural / USoundSourceBus passes IsA(USoundWave) and is a different object
    // layout, so it falls through to the writer path, which is what every caller's
    // AssetCreatePolicy::Resolve(bRequireExactClass=true) has already refused to hand us.
    if (Existing && Existing->GetClass() == USoundWave::StaticClass())
    {
        OutReport.bUpdatedInPlace = true;

        // Snapshot BEFORE the write, diff after. The preservation is structural - the update
        // assigns only payload fields - so this measures the guarantee rather than implementing
        // it, and a future engine field that starts moving under the rewrite fails the verb's
        // verification instead of silently unrouting the asset.
        const TMap<FName, FString> PropertiesBefore =
            PwAudioExportInternal::CaptureNonPayloadProperties(Existing);

        PwAudioExportInternal::UpdateSoundWaveInPlace(Existing, Recording.InputBuffer);

        OutReport.ChangedProperties =
            PwAudioExportInternal::DiffNonPayloadProperties(PropertiesBefore, Existing);
        Wave = Existing;
    }
    else
    {
        Wave = Recording.Writer.SynchronouslyWriteSoundWave(
            Recording.InputBuffer, &AssetName, &RelativeFolder);

        // A non-null return is NOT proof: SynchronouslyWriteSoundWave hands back
        // CurrentSoundWave whatever the outcome, and SerializeSoundWaveToAsset bails to
        // Failed without touching it when the buffer is empty (SampleBufferIO.cpp:556-561).
        // (The engine spells the success state "Suceeded".)
        Audio::ESoundWavePCMWriterState WriterState = Audio::ESoundWavePCMWriterState::Idle;
        Recording.Writer.CheckStatus(&WriterState);
        if (!Wave || WriterState != Audio::ESoundWavePCMWriterState::Suceeded)
        {
            OutError = FString::Printf(
                TEXT("FSoundWavePCMWriter did not reach Succeeded for '%s' (state=%d, wave=%s)."),
                *ExpectedPackage, static_cast<int32>(WriterState),
                Wave ? TEXT("non-null") : TEXT("null"));
            return nullptr;
        }

        // Trap 6, measured rather than assumed: the writer relocates a path it dislikes to
        // the Content root and only logs. Compare where the asset actually landed against
        // where it was asked to go.
        const FString ActualPackage =
            Wave->GetOutermost() ? Wave->GetOutermost()->GetName() : FString();
        if (ActualPackage != ExpectedPackage)
        {
            OutError = FString::Printf(
                TEXT("FSoundWavePCMWriter wrote to '%s' but '%s' was requested; the asset was "
                     "relocated and is not at the path this call names."),
                *ActualPackage, *ExpectedPackage);
            return nullptr;
        }

        // Trap 4: the writer's own SetSoundAssetCompressionType call is behind an
        // IsInAudioThread() guard (SampleBufferIO.cpp:757-760) that is false on the
        // thread-pool worker DoWork runs on, so it never fires for this path. It is set only
        // on a create - on a rewrite the codec is the asset author's property, not payload.
        Wave->SetSoundAssetCompressionType(ESoundAssetCompressionType::PCM);
    }

    // Trap 3: the writer queues exactly these two calls - MarkPackageDirty +
    // FAssetRegistryModule::AssetCreated - through AsyncTask(ENamedThreads::GameThread)
    // (SampleBufferIO.cpp:581-584). Queued FROM the game thread, that does not run
    // inline, so without this the save below would see an unregistered, clean package.
    // McpSafeAssetSave is the plugin's name for the same pair, so it is reused rather
    // than open-coded; both calls are idempotent, so the writer's deferred copy
    // re-running later is harmless. It persists nothing on its own, which is why it
    // returns void. The in-place branch needs the same pair for a different reason: it
    // never went near the writer, so nothing has dirtied the package yet.
    McpSafeAssetSave(Wave);

    if (bSaveToDisk)
    {
        // Measured, not assumed: this fails a throttle-skipped save whose leftover
        // .uasset from an earlier write would satisfy a bare existence probe.
        OutReport.bSavedToDisk = SaveAssetToDiskReportingPresence(Wave, /*bForce=*/true);
    }

    // Read off the asset AFTER the write, so it reports where the routing actually ended up
    // rather than where the caller assumed it would.
    OutReport.SoundClassPath =
        Wave->SoundClassObject ? Wave->SoundClassObject->GetPathName() : FString();
    OutReport.AttenuationPath =
        Wave->AttenuationSettings ? Wave->AttenuationSettings->GetPathName() : FString();

    UE_LOG(LogPinWrightSubsystem, Verbose,
        TEXT("PwCreateSoundWaveAsset: %s (%d frames @ %d Hz, %d ch), updatedInPlace=%d "
             "changedProperties=%d saveRequested=%d saved=%d"),
        *ExpectedPackage, In.NumFrames(), In.SampleRate, PwExportChannels,
        OutReport.bUpdatedInPlace ? 1 : 0, OutReport.ChangedProperties.Num(),
        bSaveToDisk ? 1 : 0, OutReport.bSavedToDisk ? 1 : 0);

    return Wave;
}

void PwAddSoundWaveWriteReport(const TSharedPtr<FJsonObject>& Result,
                               const TSharedPtr<FJsonObject>& Verification,
                               const FPwSoundWaveWriteReport& Report)
{
    if (Result.IsValid())
    {
        TSharedPtr<FJsonObject> Routing = MakeShared<FJsonObject>();
        // Always emitted, empty string when unset. A wave with no SoundClass escapes every
        // SoundMix, duck and class volume; one with no attenuation plays at full level at any
        // distance. Neither is flagged anywhere in the editor, so a caller who is not told has
        // no reason to look (board B-synth-export-wipes-soundclass-attenuation).
        Routing->SetStringField(TEXT("soundClass"), Report.SoundClassPath);
        Routing->SetStringField(TEXT("attenuationSettings"), Report.AttenuationPath);
        Result->SetObjectField(TEXT("routing"), Routing);
    }

    if (Verification.IsValid() && Report.bUpdatedInPlace)
    {
        // Only on a rewrite: on a create there was no prior state to preserve, and a
        // "preserved: true" there would be a claim about nothing.
        Verification->SetBoolField(TEXT("propertiesPreserved"),
            Report.ChangedProperties.Num() == 0);

        if (Report.ChangedProperties.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Names;
            Names.Reserve(Report.ChangedProperties.Num());
            for (const FName& Name : Report.ChangedProperties)
            {
                Names.Add(MakeShared<FJsonValueString>(Name.ToString()));
            }
            Verification->SetArrayField(TEXT("changedProperties"), Names);
        }
    }
}

USoundWave* PwCreateTransientSoundWave(const FPwAudioBuffer& In)
{
    if (!IsInGameThread())
    {
        UE_LOG(LogPinWrightSubsystem, Warning,
            TEXT("PwCreateTransientSoundWave must run on the game thread."));
        return nullptr;
    }

    Audio::TSampleBuffer<int16> Samples;
    if (!PwAudioExportInternal::BuildInt16SampleBuffer(In, Samples))
    {
        return nullptr;
    }

    USoundWave* Wave = NewObject<USoundWave>(GetTransientPackage(), NAME_None, RF_Transient);
    if (!Wave)
    {
        return nullptr;
    }

    PwAudioExportInternal::ApplyWavPayload(Wave, Samples);

    // bMarkDirty=false: the outer is the transient package, which is never saved, so
    // dirtying it would only be noise in the editor's unsaved-packages accounting.
    Wave->SetSoundAssetCompressionType(ESoundAssetCompressionType::PCM, /*bMarkDirty=*/false);

    // Drop any derived data inherited from the default object so the first precache
    // rebuilds it - and derives Duration / TotalSamples / RawPCMDataSize - from the
    // payload just planted.
    Wave->InvalidateCompressedData(/*bFreeResources=*/true);

    return Wave;
}
