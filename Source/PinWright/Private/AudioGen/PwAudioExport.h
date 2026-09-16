// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwAudioExport - the only way an FPwAudioBuffer becomes a USoundWave.
//
// The mirror of PwAudioDecode (the only way in). There is deliberately NO file
// path here: this plugin authors Unreal audio ASSETS, so the only outputs are a
// project USoundWave and a transient one for audition. Nothing writes a .wav to
// disk.
//
// WHY Audio::FSoundWavePCMWriter AND NOT A HAND-ROLLED SERIALIZER.
// SynchronouslyWriteSoundWave (SampleBufferIO.h:182) is the engine's own
// buffer -> asset path: it builds the RIFF payload with SerializeWaveFile, plants
// it in USoundWave::RawData, and sets the fields the engine derives from that
// payload. Reimplementing it would duplicate the derived-field policy at a second
// call site, which is exactly what rpc-design.md §5a warns about. Its signature is
// byte-identical on UE 5.3-5.8 once the trailing (5.6+) transformation-target
// parameter is omitted, which this file does - so nothing here owes a row in
// docs/engine-version-support.md.
//
// THE TRAPS IT CARRIES, all handled in the .cpp, all verified against
// C:/UE_5.8/Engine/Source/Runtime/Engine/Private/SampleBufferIO.cpp:
//   1. TSampleBuffer<> defaults to int16, not float, and the writer accepts only
//      that form (:328). Conversion goes through TSampleBuffer<float>, which is
//      also where the clamp has to happen: the converting assignment operator runs
//      Audio::ArrayFloatToPcm16 (SampleBuffer.h:170), which multiplies by 32767 and
//      truncates with NO clamp (FloatArrayMath.cpp:2471-2491), so an out-of-range
//      sample wraps to the opposite rail instead of limiting.
//   2. It does not save. There is no SavePackage anywhere in that file - only a
//      deferred FAssetRegistryModule::AssetCreated + MarkPackageDirty (:581-584).
//      Persistence is this file's job.
//   3. Those two deferred calls go through AsyncTask(ENamedThreads::GameThread),
//      which does NOT run inline when the caller is already the game thread, so
//      neither has happened by the time the writer returns. Both are re-issued
//      synchronously here so the save below sees a dirty, registered package.
//   4. Its SetSoundAssetCompressionType(PCM) sits behind an IsInAudioThread() guard
//      (:757-760) that does not fire on the thread-pool worker the task runs on, so
//      the compression type is set here instead.
//   5. Anything above 2 channels is silently downmixed to stereo with no clamp and
//      no report (:381-385). Unreachable here by construction: FPwAudioBuffer is
//      deinterleaved stereo, so PwExportChannels is the only width this layer emits.
//   6. It hardcodes a "/Game/" prefix onto the path it is given (:344-348) and
//      relocates a path it considers invalid to the Content root with only a log
//      warning (:358-364). Both are silent-wrong-target hazards, so the folder is
//      validated and converted to a /Game-relative form before the call.
//
// NONE OF THOSE TRAPS APPLY TO AN IN-PLACE UPDATE, because the writer is not on
// that path at all. When a USoundWave already occupies the target path its payload
// is written onto the existing object instead - see PwCreateSoundWaveAsset below
// for why, and PwAudioExport.cpp's UpdateSoundWaveInPlace for what that writes.
//
// rpc-design.md §5a: Duration, TotalSamples and RawPCMDataSize are never derived
// twice. On the create path the engine's own writer assigns them; on the transient
// path FAsyncAudioDecompressWorker derives them from the payload on first precache
// (AudioDecompress.cpp:897-900). The in-place path is the one place this file
// assigns them itself, because there is no writer there to do it and a value left
// over from the previous payload describes audio the asset no longer holds.

#pragma once

#include "CoreMinimal.h"

struct FPwAudioBuffer;
class FJsonObject;
class USoundWave;

// Channel count of every wave this layer produces. FPwAudioBuffer is deinterleaved
// stereo by contract (a mono source duplicates its single channel into both sides),
// so there is no input shape that could ask for anything else - which is what makes
// the writer's unreported >2-channel downmix unreachable rather than merely unused.
inline constexpr int32 PwExportChannels = 2;

// What a write did, measured rather than echoed back from what was asked for, so a
// call site cannot publish an outcome it never observed (rpc-design.md §2). Passed by
// reference rather than returned: the failure paths return nullptr and the caller is
// still left holding the zeroed state the function resets at entry.
struct FPwSoundWaveWriteReport
{
    // Verdict of SaveAssetToDiskReportingPresence, which fails a throttle-skipped save
    // whose stale .uasset would satisfy a bare existence probe. False whenever no save
    // was requested.
    bool bSavedToDisk = false;

    // True when a USoundWave already occupied the path and its payload was rewritten
    // onto that same object. False on a create, including the create a caller reaches
    // by deleting the occupant first.
    bool bUpdatedInPlace = false;

    // UPROPERTYs that are not part of the payload and changed anyway across an in-place
    // rewrite, compared as exported text before and after. Empty by construction - the
    // in-place path assigns the payload fields and nothing else - so a non-empty array
    // is a defect report, and the verbs fold it into their verification verdict instead
    // of returning success beside it. Always empty on a create.
    TArray<FName> ChangedProperties;

    // The wave's mix routing as it stands AFTER the write; empty when it carries none.
    // Reported because an unrouted wave escapes every SoundMix, duck and class volume
    // and plays at full level at any distance, and nothing in the editor flags it
    // (board B-synth-export-wipes-soundclass-attenuation).
    FString SoundClassPath;
    FString AttenuationPath;
};

// Create - or rewrite in place - the USoundWave asset at PackagePath/AssetName so it
// holds In, then persist it according to bSaveToDisk.
//
// PackagePath is the CONTENT FOLDER ("/Game/Audio"), not the full asset path, and must
// be under /Game: the engine writer prepends "/Game/" itself and would silently
// relocate anything else (trap 6 above). AssetName is the leaf, with no extension.
//
// Idempotent (rpc-design.md §8), and the two halves of that take DIFFERENT paths:
//
//   - Nothing at the path: the engine writer creates the asset, with the six traps
//     above handled.
//   - A plain USoundWave already there: the payload is written onto THAT object and
//     the writer is not called at all, so the asset keeps its identity, its
//     referencers AND every property that is not the payload - SoundClass,
//     attenuation, concurrency, submix and bus sends, modulation, loading behaviour,
//     compression type, looping, volume, subtitles, curves, user data. Handing an
//     occupied path to SynchronouslyWriteSoundWave instead would reconstruct the
//     object through NewObject with the same name (SampleBufferIO.cpp:369), and a
//     reconstruct re-runs the constructor: every one of those resets to its CDO
//     default with nothing reported. That is the defect this shape exists to make
//     impossible rather than to remember not to cause.
//
// The in-place path does refresh the state parsed out of the OLD payload - format,
// duration, cue points, channel layout, timecode - because leaving it would describe
// audio the asset no longer holds. It mirrors the field set the engine's own
// reimport-over-an-existing-wave path resets (SoundFactory.cpp:657-763). The result is
// that after either path, the payload-owned fields read as they would on a fresh
// create of the same buffer and everything else is byte-identical to before;
// OutReport.ChangedProperties is the measurement of that second half.
//
// Callers must NOT use this to resolve an occupied path themselves -
// AssetCreatePolicy::Resolve owns that decision and its non-modal guarantees, and it
// is what rejects a USoundWaveProcedural / USoundSourceBus occupant before it gets
// here.
//
// OutReport is a required parameter rather than a defaulted out-pointer so a call site
// cannot forget it and report persistence, or preservation, it never measured.
//
// Game thread only, editor only. Returns nullptr with OutError set (always non-empty
// on failure) for a null/empty buffer, a folder outside /Game, an empty asset name, or
// a writer that did not reach the Succeeded state. OutError is emptied on success.
USoundWave* PwCreateSoundWaveAsset(const FPwAudioBuffer& In, const FString& PackagePath,
                                   const FString& AssetName, bool bSaveToDisk,
                                   FPwSoundWaveWriteReport& OutReport, FString& OutError);

// Publish a write report into a verb's response.
//   Result       gets "routing" { soundClass, attenuationSettings } - always, including
//                the empty strings of a freshly created wave, so a null routing is
//                visible in the response instead of only in a later asset.dump.
//   Verification gets "propertiesPreserved" on an in-place rewrite, plus
//                "changedProperties" whenever the diff found any.
// Either pointer may be invalid, in which case that half is skipped.
void PwAddSoundWaveWriteReport(const TSharedPtr<FJsonObject>& Result,
                               const TSharedPtr<FJsonObject>& Verification,
                               const FPwSoundWaveWriteReport& Report);

// Wrap In in a transient, package-less USoundWave for audition. Never saved, never
// registered with the asset registry, and collectable as soon as the last reference
// drops - PwAuditionBuffer relies on the editor's rooted preview component holding it.
//
// Hand-built rather than routed through the engine writer, because the writer's
// no-filename branch runs GenerateSoundWave, which fills RawPCMData and then frees it
// again at the end of the same DoWork (SampleBufferIO.cpp:762-767), leaving a wave with
// correct metadata and no audio. So it goes through ApplyWavPayload - the plugin's own
// SerializeWaveFile call, shared with the in-place update - which plants the same
// in-memory RIFF payload in RawData that the asset path gets from the writer, because
// that payload - not RawPCMData - is what the editor decode reads. Nothing is written
// to disk.
//
// Returns nullptr for an empty or inconsistent buffer.
USoundWave* PwCreateTransientSoundWave(const FPwAudioBuffer& In);
