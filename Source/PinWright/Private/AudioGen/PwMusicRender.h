// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwMusicRender.h - renders an FPwMusicScore (AudioGen/PwMusicScore.h) to one PCM stem per
// track, seamless by construction, on the SAME synth kernel the SFX side uses
// (AudioGen/PwSynthDsp.h). PwMusicScore.h owns the SCHEMA and the note MATERIALIZATION;
// this header owns turning materialized notes into samples.
//
// ---------------------------------------------------------------------------------------
// WHY THIS IS A RENDER LOOP AND NOT ONE RECIPE
// ---------------------------------------------------------------------------------------
// The obvious implementation - build one FPwSynthRecipe with a layer per note and hand it to
// PwRenderRecipe - is unwritable. PwSynthLimits::MaxLayers is 8 and
// PwMusicLimits::MaxNotesPerTrack is 512, and that cap is a validation ERROR rather than a
// clamp (PwSynthRecipe.h), so there is no route around it: a 9-note bar would be rejected.
//
// So a track renders as a LOOP over its notes. Each note becomes a ONE-LAYER recipe - the
// track's instrument at that note's pitch, for that note's length - rendered into a scratch
// buffer and mixed into the track's stem at the note's start frame, scaled by the note's
// velocity. The 8-layer cap is then a per-note limit that no score can reach, and the
// per-note render is the only expensive part, so the recipe SKELETON (fx chains, parameter
// bags, envelopes) is built once per track and only the three pitch-dependent fields - seed,
// durationMs and the pitch itself - are rewritten per note.
//
// ---------------------------------------------------------------------------------------
// SEAMLESS LOOPS ARE A CONSTRUCTION PROPERTY
// ---------------------------------------------------------------------------------------
// Bare USoundWave loop metadata is inert at runtime: FSoundWaveCuePoint and GetLoopRegions()
// serialize into the asset but no decoder or mixer source reads them - only the MetaSound Wave
// Player honours loop points. A music bed that relies on them does not loop. So the stem has
// to BE seamless.
//
// The construction, in one sentence: render LONGER than the loop, then WRAP the overhang back
// onto the head.
//
//   stem[i % LoopFrames] += stem[i]     for every i >= LoopFrames
//
// That is exactly one period of the score repeated forever - sum over k of s(t - k*Frames) is
// periodic with period Frames, and one period of it is the modulo fold above - so a note tail
// that crosses the loop point continues correctly into the next repetition instead of being
// cut. Nothing is faded. A fade at the boundary is NOT an equivalent fix: it removes the
// discontinuity by removing the audio, which ducks the bed once per cycle, and it leaves the
// head silent where the wrapped tail belongs.
//
// HOW MUCH OVERHANG. Not a guess - it is computed. Each note's rendered voice length is
//   max(note duration, ampEnvelope end) + the instrument's fx tail
// and the overhang is the largest (note start + voice length) - Frames across the track,
// floored at zero. The fx tail is summed from the tail-producing effects' own parameters
// (reverb preDelay+decay, delay time x its -60 dB repeat count, chorus/flanger depth). Two
// honest gaps in that estimate, stated rather than hidden:
//   - `convolve`'s tail is the impulse response's length, which is not knowable without
//     loading the asset, so it is not allowed for and its tail is truncated at the voice end.
//   - The tail is only added when the instrument's ampEnvelope ENDS AT EXACTLY 0. An envelope
//     that holds a non-zero last value (or is absent entirely) never silences the generator -
//     PwSynthDsp's ApplyAmpEnvelope holds the last point's value forever - so extra frames
//     would be more note, not a tail, and lengthening the voice would change what the score
//     says rather than let it decay.
//
// INEXACT TEMPO IS REPORTED, NEVER ROUNDED AWAY. PwScoreLoopLength is the one place the loop's
// frame count is computed, and it reports bExact / signed ResidualFrames / NearestExactBpm when
// the tempo does not divide the sample rate evenly. This renderer uses the nearest whole frame
// count and passes FPwScoreLoop through in its report untouched, so the caller can accept the
// sub-frame drift or take the suggested tempo. Second caveat, because an exact loop is
// NECESSARY BUT NOT SUFFICIENT: block-based compression (Vorbis/Opus/ADPCM/Bink) does not
// preserve a sample-exact boundary either - its frame count pads up to a whole block and the
// decoder's first block ramps in - so a stem that must loop seamlessly has to stay PCM, or be
// given real loop points a MetaSound Wave Player will honour.
//
// ---------------------------------------------------------------------------------------
// WHERE A NOTE'S PITCH LANDS - one row per generator kind, none of them silent
// ---------------------------------------------------------------------------------------
// A note resolves to a MIDI number (PwScoreResolveNoteMidi) and then to a frequency
// (PwScoreMidiToFrequencyHz). Where that frequency is written depends on the generator,
// because the six kinds do not share a pitch parameter:
//
//   osc      `frequencyHz` set ABSOLUTELY to the note's frequency.
//   formant  `f0Hz` set ABSOLUTELY. Its spec range is 20..2000 Hz, so a note above ~B6 is an
//            ERROR naming the range rather than a silently clamped vowel.
//   modal    a constant pitchEnvelope offset of 12*log2(f / modeFreqsHz[0]) semitones, so
//            mode 0 lands exactly on the note and every mode RATIO - which is the timbre - is
//            preserved. PwGenModal already multiplies the whole bank by the envelope.
//   sample   a constant pitchEnvelope offset of (note MIDI - the track's tonic MIDI).
//   granular A recorded source has no knowable pitch, so the track's tonic is the only
//            available reference: the asset is taken to BE the tonic. Consequence worth
//            knowing: the track's `octave` moves the tonic and the note together, so it does
//            not transpose a sample track the way it transposes an osc track.
//   noise    the same tonic-relative offset - which means PwGenNoise's own rejection fires on
//            any note off the tonic ("the 'noise' generator has no pitch for a pitchEnvelope
//            to act on"). That is deliberate: a percussion track on `noise` writes its notes
//            at degree 0, and silently discarding the pitch of every other note would be
//            indistinguishable from a broken degree resolver (rpc-design.md §3).
//
// Ranges are checked against the generator's OWN FPwSynthParamSpec row rather than against
// numbers copied into this file, so a spec table edit cannot leave a second bound behind.
//
// ---------------------------------------------------------------------------------------
// LEVEL, PAN AND WHAT IS NOT NORMALIZED
// ---------------------------------------------------------------------------------------
// `track.pan` is written into the one-layer recipe's layer, so PwRenderRecipe's own
// equal-power law places the track exactly once and this file carries no second pan law to
// drift from it. `track.gainDb` is applied to the finished STEM, not per note, for a measured
// reason: PwRenderRecipe clamps its output to +/-1, so a positive gain applied per note would
// clip note by note before the stem ever existed.
//
// Nothing normalizes a stem. Per PwSynthDsp.h's OUTPUT LEVEL block the six generators do not
// share an output-level convention, so `gainDb` is a relative trim against six different
// references and there is no correct absolute level to normalize to. FPwStemResult::PeakDb is
// the MEASURED peak of the finished stem and is allowed to read above 0 dBFS; a caller that
// wants a level target applies it knowingly.
//
// ---------------------------------------------------------------------------------------
// DETERMINISM
// ---------------------------------------------------------------------------------------
// Mirrors PwRenderRecipe's layer derivation exactly. The score's seed is the root:
//
//   track substream  FPwSeededRandom(Score.Seed).Derive(TrackIndex)
//   note substream   TrackSubstream.Derive(NoteIndex), whose initial seed becomes the
//                    one-layer recipe's `seed`
//
// Derive() is a pure hash of the parent's INITIAL seed and the index and advances nothing, so
// stream N is the same stream whether or not 0..N-1 were derived first. Editing track 2
// therefore cannot move a sample of track 1, and re-rendering one track reproduces it
// byte-for-byte. Note that PwScoreGenerateTrackNotes uses the same
// FPwSeededRandom(Score.Seed).Derive(TrackIndex) derivation for the notes themselves, so the
// notes and the voices that play them are keyed off one root.
//
// ---------------------------------------------------------------------------------------
// FAILURE DIRECTION
// ---------------------------------------------------------------------------------------
// Failure is the default (rpc-design.md §1): both entry points clear their out-parameters on
// entry and write them only on the single full-success path, so a partial render is never
// observable and `bMeasured` stays false on every failure exit. Every count in the report is
// read back off the work that happened - NotesDropped is FPwAudioBuffer::MixInto's own return
// being zero, never an assumption - and non-finite samples are named BEFORE a stem is
// described as silent, because NaN compares false against every threshold and would otherwise
// measure as digital silence.

#pragma once

#include "CoreMinimal.h"

#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwMusicScore.h"

/**
 * One track's rendered stem plus everything that was measured about producing it.
 *
 * `bMeasured` is false on a default-constructed result and is set only after the stem was
 * rendered, wrapped and scanned, so a caller cannot read a clean zero the renderer never
 * looked at.
 */
struct FPwStemResult
{
    FString TrackName;

    /** Exactly FPwScoreLoop::Frames frames long, at the score's sample rate, after the wrap. */
    FPwAudioBuffer Buffer;

    bool bMeasured = false;

    /** Notes that reached the stem, counted from MixInto's non-zero returns. */
    int32 NotesRendered = 0;

    /**
     * Notes whose MixInto returned 0 - i.e. contributed nothing at all. Counted, not assumed.
     * A dropped note is the §1 defect class this field exists to make visible: it looks
     * identical to a quiet note in the audio and identical to a rendered note in a
     * success-only report.
     */
    int32 NotesDropped = 0;

    /**
     * Peak of the finished stem in dBFS, after track gain and pan. May exceed 0 dBFS - nothing
     * here normalizes. A fully silent stem reports PwSynthRender::SilenceFloorDb rather than
     * -inf, matching the recipe renderer.
     */
    double PeakDb = 0.0;
};

/**
 * Whole-score measurement. `bMeasured` is set on the single full-success path at the very end
 * of PwRenderScoreStems; a render that aborted reports false and carries no stems at all, so
 * half a report cannot be read as a whole one.
 */
struct FPwMusicRenderReport
{
    bool bMeasured = false;

    /**
     * Passed through from PwScoreLoopLength untouched. Read `bExact` before treating
     * `Frames` as a sample-exact loop length: when the tempo does not divide evenly this
     * carries the signed ResidualFrames and the NearestExactBpm that would make it exact.
     */
    FPwScoreLoop Loop;

    /** One per track, in track order. */
    TArray<FPwStemResult> Stems;

    /** Notes materialized and rendered across every track. */
    int32 TotalNotes = 0;
};

/**
 * Renders every track of `Score` to its own seamless stem.
 *
 * On ANY failure returns false, leaves `Out` unmeasured and empty of stems, and fills
 * OutErrorCode with an ErrorCodes::ERR_* literal plus a sentence naming the track and note
 * that failed. Single-threaded by contract, for the determinism reason above.
 */
bool PwRenderScoreStems(const FPwMusicScore& Score, FPwMusicRenderReport& Out,
                        FString& OutErrorCode, FString& OutError);

/**
 * Sums stems into one buffer at unity gain - the mixdown of an already-balanced set, not a
 * mixer: track gain and pan were baked into each stem by PwRenderScoreStems.
 *
 * `Out` is sized to the longest stem and takes its sample rate from the first. Stems that
 * disagree on sample rate are an ERROR naming both rates, never a resample and never a skip:
 * FPwAudioBuffer::MixInto no-ops on a rate mismatch and returns 0, so a mismatched stem would
 * otherwise vanish from the mixdown while every stage reported success. An empty stem list is
 * an error rather than a zero-length success, for the same reason PwRenderRecipe rejects a
 * layerless recipe.
 */
bool PwMixStems(const TArray<FPwStemResult>& Stems, FPwAudioBuffer& Out,
                FString& OutErrorCode, FString& OutError);
