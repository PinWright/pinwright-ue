// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwMusicScore.h - the LLM-facing music-score contract, rendered by the SAME synth
// kernel as the SFX side (AudioGen/PwSynthRecipe.h + AudioGen/PwSynthDsp.h).
//
// FPwMusicScore is a FIXED topology, exactly as FPwSynthRecipe is:
//
//   score
//     version, seed, sampleRate
//     bpm, timeSignature { numerator, denominator }
//     key { root, scale }
//     bars                              total length, in whole bars
//     sections[]                        (cap PwMusicLimits::MaxSections)
//       name, startBar, bars, intensityFloor, intensityCeil
//     tracks[]                          (cap PwMusicLimits::MaxTracks) - one stem each
//       name, role, octave, gainDb, pan
//       instrument                      an FPwSynthLayer - THE SAME TYPE the SFX
//                                       recipe uses for a layer. Not a parallel model.
//       generation { mode, params }     explicit notes OR a generative rule
//       notes[]                         (cap PwMusicLimits::MaxNotesPerTrack)
//         startBeat, durationBeats, degree|midi, velocity
//
// -----------------------------------------------------------------------------------
// WHY THE INSTRUMENT IS AN FPwSynthLayer AND NOT A NEW TYPE
// -----------------------------------------------------------------------------------
// A music voice is a synth layer: a generator, an amp/pitch envelope, a modulation
// block and an fx chain. Defining a second instrument model would give the project two
// vocabularies for the same DSP, two validators that drift, and two describe_schema
// surfaces. `instrument` is therefore parsed and serialized THROUGH ParseSynthRecipe /
// SerializeSynthRecipe (wrapped as a one-layer recipe), so a generator added to the SFX
// side is instantly playable as an instrument with no edit here.
//
// Three layer fields are REJECTED inside an instrument because the score already owns
// them, and two owners of one quantity is the state PwSynthDsp.h's mono-generator rule
// exists to make unrepresentable:
//   startMs -> owned by notes[].startBeat  (when a voice starts)
//   gainDb  -> owned by the track's gainDb (how loud the stem is)
//   pan     -> owned by the track's pan    (where the stem sits)
// The remaining five keys - generator, ampEnvelope, pitchEnvelope, modulation, fx - are
// the instrument. An instrument's envelopes are validated against
// PwSynthLimits::MaxDurationMs, which is the longest single voice the kernel renders.
//
// -----------------------------------------------------------------------------------
// THE BEAT CONVENTION - stated first because it is what music code gets wrong
// -----------------------------------------------------------------------------------
// One BEAT is one `timeSignature.denominator` note, and `bpm` counts THOSE per minute.
// A bar is therefore exactly `numerator` beats, with no 4/denominator correction
// anywhere. In 6/8 at 90 bpm an eighth note lasts 666.67 ms and a bar lasts 4 s.
//
// This is the convention UE's own Quartz clock uses: FQuartzTimeSignature is
// { NumBeats, BeatType } where BeatType IS the denominator
// (Sound/QuartzQuantizationUtilities.h:78-88,109-125 on 5.8), and
// EQuartzCommandQuantization::Beat is documented as "dependent on time signature". The
// alternative convention (beat == quarter note always, as in the MIDI tempo meta-event)
// was rejected because a score written here is meant to be handed to Quartz, and a
// score that means one thing here and another there is worse than either choice.
//
// `denominator` is restricted to the five values Quartz can express (2,4,8,16,32);
// EQuartzTimeSignatureQuantization has no whole-note beat type, so a `/1` score could
// not be scheduled and is rejected rather than accepted and stranded.
//
// There is NO tempo map in v1. One `bpm` governs the whole score, which is why
// PwScoreBeatToMs needs only the score and a beat. A tempo map would change that
// signature, so it is a schema revision, not an added optional field.
//
// -----------------------------------------------------------------------------------
// SEAMLESS LOOPS ARE A CONSTRUCTION PROPERTY, NOT METADATA
// -----------------------------------------------------------------------------------
// Bare USoundWave loop metadata is inert at runtime: FSoundWaveCuePoint and
// GetLoopRegions() serialize into the asset, but no decoder or mixer source reads them -
// only the MetaSound Wave Player honours loop points. A music bed that relies on them
// does not loop. So a loop has to BE an exact number of frames.
//
// PwScoreLoopLength is the one place that number is computed. A score is an integer
// number of bars by construction (`bars` is an int32), so the only question is whether
// bars x beats x 60000/bpm x sampleRate/1000 lands on a whole frame. It frequently does
// not, and the function REPORTS that (bExact=false, signed ResidualFrames, and the
// nearest tempo that would be exact) rather than rounding behind the caller's back -
// a rounded loop is a click every cycle, arriving at render time with no explanation.
//
// Second caveat, for the consumer that encodes the stem: block-based compression
// (Vorbis/Opus/ADPCM/Bink) does not preserve a sample-exact boundary either. Its frame
// count is padded up to a whole block and the decoder's first block ramps in. An exact
// FPwScoreLoop is a necessary condition for a seamless loop, not a sufficient one; the
// stem must also stay PCM, or the encoder must be given real loop points a MetaSound
// Wave Player will honour.
//
// -----------------------------------------------------------------------------------
// MUSIC THEORY COMES FROM THE ENGINE, NOT FROM A TABLE HERE
// -----------------------------------------------------------------------------------
// EPwMusicScale mirrors Audio::EMusicalScale::Scale (DSP/MidiNoteQuantizer.h) 1:1 - all
// 30 enumerators, same order, verified identical on 5.3 through 5.8 - and degree
// resolution reads Audio::FMidiNoteQuantizer::ScaleDegreeSetMap, the engine's own scale
// table. That is the same table MetaSound's MIDI-note-quantizer node uses, so a score
// authored here and a graph built there agree about what "phrygian" means.
//
// Audio::FMidiNoteQuantizer::QuantizeMidiNote is deliberately NOT applied to an authored
// `midi` note. It SNAPS an arbitrary note into the scale, and silently moving a pitch the
// caller wrote down is the §3 failure this schema exists to prevent: an out-of-key note
// is either intentional or a bug, and neither is improved by relocating it without
// saying so. Degrees quantize through the table; `midi` is absolute and verbatim.
//
// -----------------------------------------------------------------------------------
// DESIGN RULES (docs/rpc-design.md), identical to the ones PwSynthRecipe.h states
// -----------------------------------------------------------------------------------
//   §1 failure is the default - ParseMusicScore writes `Out` only on full success, so a
//      partially-parsed score is never observable, and the error always names the exact
//      field path ("tracks[2].notes[17].degree") because that is what the LLM patches.
//   §3 no safe default => required - an unrecognised scale, pitch class, role or
//      generation mode is an ERROR naming the valid set, never a fallback to
//      major/lead/explicit. Caps (tracks, sections, notes, bars) are validation errors,
//      not clamps, because a silently clamped score does not round-trip.
//   §2 the zero value is a failure - every enum here has Unspecified = 0, so a
//      default-constructed track cannot serialize as a plausible "lead playing C major".
//
// Round-trip is a hard requirement: Serialize(Parse(x)) == x for any valid score, modulo
// key ordering and defaults being made explicit.
//
// DETERMINISM: every generative mode draws from FPwSeededRandom(score.seed).Derive(trackIndex),
// a pure hash of the score seed and the track's index. A score therefore materializes the
// same notes whatever order tracks are walked in, in this session and after an editor
// restart - the same contract PwSynthDsp.h states for the sample path.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Array.h"
#include "Misc/Optional.h"

#include "AudioGen/PwSynthRecipe.h"

class FJsonObject;

// ---------------------------------------------------------------------------
// Limits. Every one of these is enforced as a validation ERROR, never a clamp.
// ---------------------------------------------------------------------------
namespace PwMusicLimits
{
    // Score schema revision. A score carrying any other value is rejected rather than
    // best-effort parsed (an unknown revision is an unknown enum).
    inline constexpr int32 ScoreVersion = 1;

    // One stem per track, so the track cap is also the stem-file cap.
    inline constexpr int32 MaxTracks = 16;
    inline constexpr int32 MaxSections = 32;
    inline constexpr int32 MaxNotesPerTrack = 512;

    inline constexpr int32 MinBars = 1;
    inline constexpr int32 MaxBars = 512;

    inline constexpr double MinBpm = 20.0;
    inline constexpr double MaxBpm = 300.0;

    inline constexpr int32 MinTimeSignatureNumerator = 1;
    inline constexpr int32 MaxTimeSignatureNumerator = 32;

    // Track and section names become stem filenames and dynamic-music lookup keys.
    inline constexpr int32 MinNameLength = 1;
    inline constexpr int32 MaxNameLength = 64;

    // MIDI note numbers a track's register may span. -1 puts C at MIDI 0.
    inline constexpr int32 MinOctave = -1;
    inline constexpr int32 MaxOctave = 9;
    inline constexpr int32 MinMidi = 0;
    inline constexpr int32 MaxMidi = 127;

    // Scale degrees. Wide enough for +/- 5 octaves of the sparsest scale in the engine
    // table (5 degrees per octave), which is more range than MIDI has.
    inline constexpr int32 MinDegree = -63;
    inline constexpr int32 MaxDegree = 63;

    inline constexpr double MinBeat = 0.0;
    // A single note may not outlast the longest score expressible above.
    inline constexpr double MaxBeats = static_cast<double>(MaxBars * MaxTimeSignatureNumerator);

    // Documented safe defaults (rpc-design §3): a value that is never wrong in the field
    // may default; everything else is required.
    inline constexpr int32 DefaultSeed = 0;
    inline constexpr int32 DefaultSampleRate = PwSynthLimits::DefaultSampleRate;
    inline constexpr double DefaultTrackGainDb = 0.0;
    inline constexpr double DefaultTrackPan = 0.0;
    // Unity, exactly as a layer's gainDb defaults to 0 dB. A note that says nothing about
    // its velocity is a note at full strength, which is never a surprising reading.
    inline constexpr double DefaultVelocity = 1.0;
    // A section that constrains nothing spans the whole intensity range.
    inline constexpr double DefaultIntensityFloor = 0.0;
    inline constexpr double DefaultIntensityCeil = 1.0;
}

// ---------------------------------------------------------------------------
// Closed vocabularies. Every FromString returns false on an unrecognised value; no
// caller is allowed to degrade an unknown token into a default.
// ---------------------------------------------------------------------------

// Pitch class of the key's root. Sharps only, and flats are NOT accepted aliases:
// canonicalizing "db" to "c#" on the way in would make Serialize(Parse(x)) differ from x,
// and a byte-stable round trip is worth more than accepting a second spelling. The error
// names the twelve accepted tokens, which is what an LLM needs to patch it.
enum class EPwMusicPitchClass : uint8
{
    Unspecified = 0,    // zero value is a failure (rpc-design §2)
    C,
    CSharp,
    D,
    DSharp,
    E,
    F,
    FSharp,
    G,
    GSharp,
    A,
    ASharp,
    B,
    Count
};

// 1:1 mirror of Audio::EMusicalScale::Scale (DSP/MidiNoteQuantizer.h), in the engine's
// declaration order, offset by one so Unspecified can occupy zero. Verified enumerator
// for enumerator against the 5.3 and 5.8 headers; the engine's own comment requires any
// addition to be mirrored into MetasoundMidiNoteQuantizerNode.cpp, so this list is also
// the MetaSound vocabulary.
enum class EPwMusicScale : uint8
{
    Unspecified = 0,    // zero value is a failure (rpc-design §2)

    // Modes
    Major,
    MinorDorian,
    Phrygian,
    Lydian,
    Dominant7thMixolydian,
    NaturalMinorAeolian,
    HalfDiminishedLocrian,

    // Non-diatonic
    Chromatic,
    WholeTone,
    DiminishedWholeTone,

    // Pentatonic
    MajorPentatonic,
    MinorPentatonic,
    Blues,

    // Bebop
    BebopMajor,
    BebopMinor,
    BebopMinorNumber2,
    BebopDominant,

    // Common major / minors
    HarmonicMajor,
    HarmonicMinor,
    MelodicMinor,
    SixthModeOfHarmonicMinor,

    // Lydian / augmented
    LydianAugmented,
    LydianDominant,
    Augmented,

    // Diminished
    Diminished,
    DiminishedBeginWithHalfStep,
    DiminishedBeginWithWholeStep,
    HalfDiminishedLocrianNumber2,

    // Other
    SpanishOrJewish,
    Hindu,

    Count
};

// What a track is FOR. This is metadata for the stem consumer - stem naming, and the
// layer selection a later dynamic-music verb will key on - and carries no DSP meaning
// whatsoever: two tracks with the same instrument and different roles render identically.
// It is a closed set anyway, because a role nobody downstream recognises is a stem nobody
// mixes in, and a typo that silently became "lead" would be discovered by ear.
enum class EPwMusicTrackRole : uint8
{
    Unspecified = 0,    // zero value is a failure (rpc-design §2)
    Drone,              // sustained bed, no articulation
    Pad,                // slow harmonic bed
    Bass,               // low-register foundation
    Lead,               // foreground melodic line
    Texture,            // noise / atmosphere
    Percussion,         // rhythmic, usually unpitched
    Fx,                 // transitional accents, risers, impacts
    Count
};

// How a track's notes come to exist. There is no default: a track that does not say is an
// error, because "explicit" and "drone" produce wildly different music from the same
// document and neither is a safe guess.
enum class EPwMusicGenerationMode : uint8
{
    Unspecified = 0,    // zero value is a failure (rpc-design §2)

    // notes[] is authoritative and required; nothing is generated. Takes no parameters.
    Explicit,

    // One sustained chord per section (or per score, when there are no sections). Consumes
    // NO randomness at all, so a drone track is seed-invariant by construction - which is a
    // property worth having rather than a gap: a bed that changes when an unrelated track's
    // seed moves is not a bed.
    Drone,

    // Overlapping swells drawn from a degree pool. The workhorse of an ambient bed.
    EvolvingPad,

    // Occasional seeded notes from a degree pool, at a stated events-per-bar density.
    SparseEvents,

    Count
};

// ---------------------------------------------------------------------------
// Score topology.
// ---------------------------------------------------------------------------

// numerator beats per bar; denominator names what a beat IS (see the header note).
struct FPwMusicTimeSignature
{
    int32 Numerator = 4;
    int32 Denominator = 4;
};

struct FPwMusicKey
{
    EPwMusicPitchClass Root = EPwMusicPitchClass::Unspecified;
    EPwMusicScale Scale = EPwMusicScale::Unspecified;
};

// A named span of whole bars. When `sections` is present it must TILE the score exactly -
// first section at bar 0, each following one starting where the previous ended, the last
// ending at `bars` - so "which section is bar N in" always has exactly one answer. A gap
// is almost always an editing mistake and is rejected rather than silently played through.
struct FPwMusicSection
{
    FString Name;
    int32 StartBar = 0;
    int32 Bars = 0;

    // The intensity band this section occupies, 0..1, floor <= ceil. In v1 this REMAPS
    // generated velocity into the band (a note drawn at velocity v lands at
    // floor + v*(ceil-floor)), which is what makes a quiet section quiet. It does NOT touch
    // authored notes on an `explicit` track: those velocities were decided by the author and
    // rescaling them behind their back is the same class of silent repair §3 forbids.
    double IntensityFloor = PwMusicLimits::DefaultIntensityFloor;
    double IntensityCeil = PwMusicLimits::DefaultIntensityCeil;
};

// One note. Exactly one of Degree / Midi is set - both, or neither, is a parse error,
// because two pitch sources let one note say two things.
//
// Degree is an index into the key's scale, 0 = the root at the TRACK's octave, negative
// below it, and it wraps octaves at the scale's own degree count (degree 7 in a 5-degree
// pentatonic is the third degree an octave up). Midi is absolute, ignores the track's
// octave, and is never snapped to the key.
struct FPwMusicNote
{
    double StartBeat = 0.0;
    double DurationBeats = 0.0;
    TOptional<int32> Degree;
    TOptional<int32> Midi;
    double Velocity = PwMusicLimits::DefaultVelocity;

    bool HasPitch() const { return Degree.IsSet() != Midi.IsSet(); }
};

// Mode plus its schema-validated parameter bag - the same FPwSynthParams machinery the
// SFX generators and effects use, validated by the same code (ParseSynthParamBag) against
// a per-mode FPwSynthKindSpec table. Adding a generation parameter is a one-line table
// edit that updates validation, serialization and the published schema together.
struct FPwMusicGeneration
{
    EPwMusicGenerationMode Mode = EPwMusicGenerationMode::Unspecified;
    FPwSynthParams Params;
};

// One track renders to one stem.
struct FPwMusicTrack
{
    // Becomes the stem's filename, so the charset is restricted to [A-Za-z0-9_-] and
    // uniqueness across tracks is checked CASE-INSENSITIVELY: "Pad" and "pad" are one file
    // on Windows, and the second stem would silently overwrite the first.
    FString Name;

    EPwMusicTrackRole Role = EPwMusicTrackRole::Unspecified;

    // Register. Anchors degree 0 for this track: root pitch class at this octave, in the
    // C4 = MIDI 60 convention (so octave 3 puts C at MIDI 48). Required and never defaulted:
    // register is the difference between a bass bed and a lead, and no value is right for
    // both. Notes that carry `midi` instead of `degree` are absolute and ignore it.
    int32 Octave = 0;

    double GainDb = PwMusicLimits::DefaultTrackGainDb;
    double Pan = PwMusicLimits::DefaultTrackPan;    // -1 hard left .. +1 hard right

    // The voice. See the header note on why this is the SFX layer type verbatim.
    FPwSynthLayer Instrument;

    FPwMusicGeneration Generation;

    // Populated and required only when Generation.Mode == Explicit. A generative track
    // carrying notes is an error, not a merge: the renderer would have two sources of truth
    // for the same stem.
    TArray<FPwMusicNote> Notes;
};

struct FPwMusicScore
{
    int32 Version = PwMusicLimits::ScoreVersion;
    int32 Seed = PwMusicLimits::DefaultSeed;
    int32 SampleRate = PwMusicLimits::DefaultSampleRate;

    double Bpm = 0.0;                   // required; 0 is not a valid score
    FPwMusicTimeSignature TimeSignature;
    FPwMusicKey Key;
    int32 Bars = 0;                     // required; 0 is not a valid score

    TArray<FPwMusicSection> Sections;
    TArray<FPwMusicTrack> Tracks;
};

// The recipe chunk's error struct is reused verbatim rather than cloned: the codes, the
// field-path convention and the ToString() shape are identical, an instrument's parse
// errors arrive in it already, and a second struct would be a second style for one job.
using FPwMusicScoreError = FPwSynthRecipeError;

// ---------------------------------------------------------------------------
// Vocabulary accessors. Every enum here starts at 1 (index 0 is Unspecified), so iterate
//   for (uint8 i = 1; i < (uint8)EPwMusicScale::Count; ++i)
// ---------------------------------------------------------------------------

const TCHAR* PwMusicPitchClassToString(EPwMusicPitchClass Value);
bool PwMusicPitchClassFromString(const FString& In, EPwMusicPitchClass& Out);
// Semitones above C, 0..11. Returns 0 for Unspecified, which callers must not reach:
// parse rejects Unspecified before any resolution runs.
int32 PwMusicPitchClassSemitone(EPwMusicPitchClass Value);

const TCHAR* PwMusicScaleToString(EPwMusicScale Value);
bool PwMusicScaleFromString(const FString& In, EPwMusicScale& Out);

const TCHAR* PwMusicTrackRoleToString(EPwMusicTrackRole Value);
bool PwMusicTrackRoleFromString(const FString& In, EPwMusicTrackRole& Out);
const TCHAR* PwMusicTrackRoleMeaning(EPwMusicTrackRole Value);

const TCHAR* PwMusicGenerationModeToString(EPwMusicGenerationMode Value);
bool PwMusicGenerationModeFromString(const FString& In, EPwMusicGenerationMode& Out);

// The mode's parameter table + summary + cross-parameter invariant, in the same shape the
// SFX generators and effects publish. Explicit's table is empty.
const FPwSynthKindSpec& PwMusicGenerationSpec(EPwMusicGenerationMode Mode);

// How many degrees the scale has per octave, read from the engine's own table. False when
// the engine table has no entry for the scale, which is a failure rather than a guess.
bool PwMusicScaleDegreeCount(EPwMusicScale Scale, int32& OutCount);

// The five denominators Quartz's EQuartzTimeSignatureQuantization can express.
const TArray<int32>& PwMusicTimeSignatureDenominators();

// ---------------------------------------------------------------------------
// Parse / serialize.
// ---------------------------------------------------------------------------

// Parses a score JSON object. On ANY failure returns false, leaves `Out` completely
// untouched, and fills OutError with the wire error code and the exact field path the
// caller must patch. Unknown object keys are rejected at every level, so a typo surfaces
// as a precise error instead of a silently dropped setting.
//
// Error-code split, mirroring the recipe's:
//   INVALID_PARAMS    - a VALUE problem: missing required value, wrong JSON type, out of
//                       range, or outside a closed vocabulary (unknown scale / root /
//                       role / generation mode all land here).
//   INVALID_RECIPE    - a SHAPE problem: unknown key, cap exceeded, missing container,
//                       ordering violation, duplicate name, section tiling gap, or a
//                       cross-field contradiction. The noun in the code says "recipe" for
//                       both documents; the method name is what says which document it was.
//   UNKNOWN_GENERATOR / UNKNOWN_EFFECT - forwarded unchanged from the instrument, because
//                       an instrument IS a synth layer and its failures are the layer's.
bool ParseMusicScore(const TSharedPtr<FJsonObject>& In, FPwMusicScore& Out, FPwMusicScoreError& OutError);

// Contract overload. OutError is "<field path>: <message>".
bool ParseMusicScore(const TSharedPtr<FJsonObject>& In, FPwMusicScore& Out, FString& OutError);

// Emits the canonical JSON form: every defaulted value made explicit, every key in its
// spec-table spelling. Serialize(Parse(x)) round-trips x.
TSharedPtr<FJsonObject> SerializeMusicScore(const FPwMusicScore& In);

// ---------------------------------------------------------------------------
// Time. ONE authoritative beat <-> time conversion, because stem rendering, loop points
// and Quartz scheduling all need it and two implementations drift.
// ---------------------------------------------------------------------------

// Beats in one bar. Equals TimeSignature.Numerator, by the convention at the top.
double PwScoreBeatsPerBar(const FPwMusicScore& Score);

// Beats in the whole score: Bars * BeatsPerBar.
double PwScoreTotalBeats(const FPwMusicScore& Score);

// Beat position -> milliseconds from the start of the score. Linear: there is no tempo map.
double PwScoreBeatToMs(const FPwMusicScore& Score, double Beat);

// Milliseconds -> beat position. Exact inverse of PwScoreBeatToMs up to double rounding.
double PwScoreMsToBeat(const FPwMusicScore& Score, double Ms);

// Whole-score duration in milliseconds.
double PwScoreDurationMs(const FPwMusicScore& Score);

// The measurement a seamless loop depends on. Nothing here is rounded silently: when the
// loop does not land on a whole frame, bExact is false and the caller is handed the signed
// residual plus the nearest tempo that WOULD be exact, so it can move the tempo, change the
// sample rate, or accept the click knowingly.
struct FPwScoreLoop
{
    // Bars * BeatsPerBar * 60000 / Bpm.
    double DurationMs = 0.0;

    // DurationMs * SampleRate / 1000, unrounded. The honest number.
    double ExactFrames = 0.0;

    // Nearest whole frame count. Read bExact BEFORE using it as a loop length.
    int64 Frames = 0;

    // ExactFrames - Frames, signed. Exactly 0.0 when bExact.
    double ResidualFrames = 0.0;

    // True only when the loop is a whole number of frames. Computed by integer
    // divisibility when Bpm is a whole number (the common case), and by a tight
    // floating-point residual test otherwise.
    bool bExact = false;

    // The tempo that makes this bar count land on exactly `Frames` frames at this sample
    // rate. Equals Bpm when bExact. 0.0 when Frames is 0, where no tempo helps.
    double NearestExactBpm = 0.0;
};

FPwScoreLoop PwScoreLoopLength(const FPwMusicScore& Score);

// The section containing a bar, or nullptr when the score has no sections (a scoreless
// span is legal - it simply means no intensity band applies). Sections tile the score, so
// a bar inside [0, Bars) always resolves when any section exists.
const FPwMusicSection* PwScoreSectionAtBar(const FPwMusicScore& Score, int32 Bar);

// ---------------------------------------------------------------------------
// Pitch. Degrees resolve through the engine's scale table; MIDI is verbatim.
// ---------------------------------------------------------------------------

// Resolves a scale degree against a key root at an octave. False - with a registered error
// code and a sentence - when the scale is missing from the engine table or the result falls
// outside MIDI 0..127. Never clamps: a note the caller cannot hear where they asked for it
// is a score defect, not something to relocate quietly.
bool PwScoreDegreeToMidi(EPwMusicScale Scale, EPwMusicPitchClass Root, int32 Octave, int32 Degree,
    int32& OutMidi, FString& OutErrorCode, FString& OutError);

// Resolves whichever pitch source the note carries. Mirrors PwScoreDegreeToMidi's failure
// contract; an absolute `midi` note is returned unchanged and unquantized.
bool PwScoreResolveNoteMidi(const FPwMusicScore& Score, const FPwMusicTrack& Track, const FPwMusicNote& Note,
    int32& OutMidi, FString& OutErrorCode, FString& OutError);

// Audio::GetFrequencyFromMidi (DSP/Dsp.h), A4 = 440 Hz. Wrapped here so no consumer
// hand-rolls the exponent and the whole subsystem agrees on tuning.
double PwScoreMidiToFrequencyHz(double Midi);

// ---------------------------------------------------------------------------
// Generation.
// ---------------------------------------------------------------------------

// Materializes the notes a track contributes: the authored array for `explicit`, and the
// rule's output for every generative mode. ONE entry point on purpose - a renderer that
// walks this cannot forget to handle the explicit case, and cannot accidentally use a
// different RNG derivation for one mode than another.
//
// Deterministic by construction: the stream is FPwSeededRandom(Score.Seed).Derive(TrackIndex),
// a pure hash of the score seed and the track index, so the same score yields byte-identical
// notes whatever order the tracks are materialized in.
//
// Failure is the default: OutNotes is emptied on entry and filled only on the single
// full-success path. The per-track note cap is enforced HERE as well as at parse - a rule
// that would expand past PwMusicLimits::MaxNotesPerTrack errors instead of truncating.
bool PwScoreGenerateTrackNotes(const FPwMusicScore& Score, int32 TrackIndex,
    TArray<FPwMusicNote>& OutNotes, FString& OutErrorCode, FString& OutError);
