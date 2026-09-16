// Copyright (c) 2026 Alexander Penkin. MIT License.

// PCM extraction from an imported USoundWave asset.
//
// This is the only decode seam in the audio-generation subsystem: everything the
// analyzer needs to reach is already a USoundWave, so there is deliberately no
// file-reading path here. An audio file on disk is imported into the project as a
// SoundWave asset first, and then goes through this function like anything else.
//
// USoundWave::GetImportedSoundWaveData is the most version-stable way in: it is
// ENGINE_API, present unchanged on UE 5.3-5.8, and hands back interleaved int16
// plus the parsed header. Nothing here uses a symbol newer than 5.3, so this file
// owes no row in docs/engine-version-support.md.

#pragma once

#include "CoreMinimal.h"

#include "AudioGen/PwAudioBuffer.h"

class USoundWave;

/**
 * Decode an imported USoundWave into deinterleaved float PCM.
 *
 * Mono duplicates into both channels; stereo splits. Out is emptied on entry and
 * only populated on a fully successful decode, so a rejected or partial parse can
 * never look like a success (rpc-design.md §1).
 *
 * NOT CHEAP. USoundWave::FEditorAudioBulkData::GetPayload() returns a TFuture that
 * the engine immediately waits on ("Will block." - SoundWave.cpp:1784), so a cold
 * asset pays a bulk-data load, and the Oodle-packed payload is unpacked on the way
 * out. Call it once per wave and cache the buffer; do not call it per frame.
 *
 * Game thread only. UAudioAnalyzerNRT::AnalyzeAudio asserts this outright
 * (AudioAnalyzerNRT.cpp:132) because the payload is read under the wave's
 * RawDataCriticalSection; this function reports the violation instead of asserting.
 *
 * @param Wave       Source asset. Null is a failure, not a silent empty result.
 * @param Out        Receives the deinterleaved buffer. Emptied on every failure path.
 * @param OutError   Human-readable reason on failure; empty on success.
 * @return           True only when Out holds at least one fully decoded frame.
 */
bool PwDecodeSoundWave(const USoundWave* Wave, FPwAudioBuffer& Out, FString& OutError);

/**
 * As PwDecodeSoundWave, but also reports which failure occurred as a registered
 * error code, so a caller (an RPC handler) branches on the code and never parses
 * the message (rpc-design.md §7). Opposite failures get distinct codes:
 *
 *   ERR_AUDIO_PROCEDURAL_UNSUPPORTED     - bProcedural wave; there is no imported payload at all
 *   ERR_AUDIO_MULTICHANNEL_UNSUPPORTED   - >2 channels (deinterleaved-surround layout)
 *   ERR_AUDIO_EMPTY_BUFFER               - the payload decoded but holds zero complete frames
 *   ERR_DECODE_FAILED                    - the payload exists but could not be read or parsed
 *   ERR_INVALID_PARAMS                   - null wave (a caller bug, not a missing asset)
 *   ERR_INVALID_STATE                    - called off the game thread
 *
 * DECODE_FAILED is the shared spelling on purpose: ErrorCodes.h states that the audio
 * verbs reuse DECODE_FAILED / ENCODE_FAILED / FILE_NOT_FOUND rather than adding
 * AUDIO_-prefixed synonyms of them.
 *
 * @param OutErrorCode  The registered code on failure; empty on success.
 */
bool PwDecodeSoundWaveWithCode(const USoundWave* Wave, FPwAudioBuffer& Out,
    FString& OutErrorCode, FString& OutError);
