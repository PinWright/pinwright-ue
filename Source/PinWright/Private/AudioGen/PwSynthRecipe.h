// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwSynthRecipe.h - the LLM-facing procedural-audio recipe contract.
//
// FPwSynthRecipe is a FIXED signal-chain topology, deliberately neither a free
// node graph nor a closed template enum:
//
//   recipe
//     version, seed, sampleRate, durationMs
//     layers[]                     (cap PwSynthLimits::MaxLayers)
//       startMs, gainDb, pan
//       generator { kind, params }              MONO source
//       ampEnvelope   [{ timeMs, value, curve }]   piecewise, per-segment curve
//       pitchEnvelope [{ timeMs, semitones }]      piecewise, linear segments
//       modulation { fm|am|ring: depth, rateHz, source }
//       fx[]                       (cap PwSynthLimits::MaxLayerFx, ordered)
//     master
//       fx[]                       (cap PwSynthLimits::MaxMasterFx, ordered)
//       normalize { mode: peak|lufs, target }
//       fadeInMs, fadeOutMs
//     targets                      optional metric ranges, all optional
//
// Generators are mono; the mix bus is stereo and each layer's `pan` places it.
// No generator carries stereo fields.
//
// Per-kind parameters are a SCHEMA-VALIDATED KEY/VALUE BAG (FPwSynthParams),
// not a tagged union and not 21 hand-written structs. One static table per kind
// (FPwSynthKindSpec::Params) is the single source of truth for three consumers:
//   1. ParseSynthRecipe   - validation (type, range, required-ness, closed sets)
//   2. SerializeSynthRecipe - canonical key spelling and emission order
//   3. audio.synth.describe_schema - the documentation the LLM reads
// A wave-2 agent adding a generator's real DSP adds rows to its table and reads
// values back through the typed accessors; no type in this header reshapes.
//
// Design rules this file is built to (docs/rpc-design.md):
//   §1 failure is the default - ParseSynthRecipe writes `Out` only on full
//      success, so a partially-parsed recipe is never observable, and OutError
//      always names the offending field path (e.g. "layers[2].fx[1].kind").
//   §3 no safe default => required - an unrecognised generator kind, fx kind,
//      filter type, curve or normalize mode is an ERROR, never a silent fallback
//      to "default oscillator" / "no-op effect". Caps are validation errors, not
//      clamps, because a silently clamped recipe does not round-trip.
//   §2 the zero value is a failure - EPwSynth{Generator,Fx}Kind::Unspecified is 0,
//      so a default-constructed generator cannot serialize as a plausible "osc".
//
// Round-trip is a hard requirement: Serialize(Parse(x)) == x for any valid
// recipe, modulo key ordering and defaults being made explicit. Parse
// materializes every defaulted parameter into the bag and Serialize emits the
// kind's full spec table, so serialize is lossless and re-parsing is idempotent.
// Wave 3's `patch` verb applies JSON Patch to serialized output and re-parses it.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Map.h"
#include "Misc/Optional.h"
#include "UObject/NameTypes.h"

class FJsonObject;

// ---------------------------------------------------------------------------
// Limits. Every one of these is enforced as a validation ERROR, never a clamp.
// ---------------------------------------------------------------------------
namespace PwSynthLimits
{
    // Recipe schema revision. A recipe carrying any other value is rejected
    // rather than best-effort parsed (an unknown revision is an unknown enum).
    inline constexpr int32 RecipeVersion = 1;

    inline constexpr int32 MaxLayers = 8;
    inline constexpr int32 MaxLayerFx = 4;
    inline constexpr int32 MaxMasterFx = 6;
    inline constexpr int32 MinEnvelopePoints = 1;
    inline constexpr int32 MaxEnvelopePoints = 64;

    inline constexpr double MinDurationMs = 1.0;
    inline constexpr double MaxDurationMs = 60000.0;

    inline constexpr int32 MinSampleRate = 8000;
    inline constexpr int32 MaxSampleRate = 192000;

    // Documented safe defaults (rpc-design §3): a value that is never wrong in
    // the field may default; everything else is required.
    inline constexpr int32 DefaultSeed = 0;
    inline constexpr int32 DefaultSampleRate = 48000;
    inline constexpr double DefaultLayerStartMs = 0.0;
    inline constexpr double DefaultLayerGainDb = 0.0;
    inline constexpr double DefaultLayerPan = 0.0;
    inline constexpr double DefaultFadeMs = 0.0;

    inline constexpr double MinGainDb = -96.0;
    inline constexpr double MaxGainDb = 24.0;
}

// ---------------------------------------------------------------------------
// Closed vocabularies. Every FromString returns false on an unrecognised value;
// no caller is allowed to degrade an unknown token into a default.
// ---------------------------------------------------------------------------

enum class EPwSynthGeneratorKind : uint8
{
    Unspecified = 0,    // zero value is a failure (rpc-design §2)
    Osc,
    Noise,
    Modal,
    Formant,
    Granular,
    Sample,
    Count
};

enum class EPwSynthFxKind : uint8
{
    Unspecified = 0,    // zero value is a failure (rpc-design §2)
    Filter,
    Distort,
    Delay,
    Reverb,
    Chorus,
    Flanger,
    Phaser,
    RingMod,
    PitchShift,
    Compressor,
    Eq,
    Gain,
    Width,
    Reverse,
    Convolve,
    Count
};

// Per-segment interpolation shape of an amplitude envelope. Attached to the
// point that STARTS the segment; the last point's curve is unused.
enum class EPwSynthCurve : uint8
{
    Linear = 0,         // documented default
    Exp,
    Log,
    Step,
    SCurve,
    Count
};

enum class EPwSynthNormalizeMode : uint8
{
    None = 0,           // no normalize block present
    Peak,
    Lufs,
    Count
};

enum class EPwSynthModulationRouting : uint8
{
    None = 0,           // no modulation block present
    Fm,
    Am,
    Ring,
    Count
};

// Wave shape of the modulator feeding fm/am/ring.
enum class EPwSynthModSource : uint8
{
    Sine = 0,           // documented default
    Triangle,
    Saw,
    Square,
    Noise,
    Count
};

// Measurable properties of a rendered buffer that `targets` can constrain.
enum class EPwSynthMetric : uint8
{
    PeakDb = 0,
    RmsDb,
    Lufs,
    CrestDb,
    DurationMs,
    CentroidHz,
    RolloffHz,
    Flatness,
    ZeroCrossingRate,
    AttackMs,
    DecayMs,
    DcOffset,
    ClippedSamples,
    StereoCorrelation,
    Count
};

// ---------------------------------------------------------------------------
// Per-kind parameter schema + value bag.
// ---------------------------------------------------------------------------

enum class EPwSynthParamType : uint8
{
    Number,
    Integer,
    Boolean,
    String,         // free-form text (asset paths)
    Enum,           // closed set; EnumValues holds the pipe-separated vocabulary
    NumberArray
};

// One row of a kind's parameter table. All char pointers are string literals
// owned by the table, so this struct is trivially copyable and allocation-free.
struct FPwSynthParamSpec
{
    // Lookup key. FName comparison is case-insensitive, so a caller may spell a
    // key in any case; Serialize always emits DisplayName, so output is canonical.
    FName Name = NAME_None;
    EPwSynthParamType Type = EPwSynthParamType::Number;

    const TCHAR* DisplayName = nullptr;     // canonical JSON key spelling
    const TCHAR* Unit = nullptr;            // "hz" / "db" / "ms" / ... nullptr when dimensionless
    const TCHAR* EnumValues = nullptr;      // pipe-separated closed set (Enum only)
    const TCHAR* Meaning = nullptr;         // one-line description for describe_schema

    bool bRequired = false;

    bool bHasMin = false;
    bool bHasMax = false;
    double Min = 0.0;                       // element bound for NumberArray
    double Max = 0.0;

    double DefaultNumber = 0.0;             // Number / Integer / Boolean default
    const TCHAR* DefaultString = nullptr;   // String / Enum default

    int32 MinArrayNum = 0;                  // NumberArray only
    int32 MaxArrayNum = 0;
};

struct FPwSynthParamValue
{
    EPwSynthParamType Type = EPwSynthParamType::Number;
    double Number = 0.0;
    bool bBool = false;
    FString String;                         // String and Enum both land here
    TArray<double> Numbers;
};

// A kind's parameter values. After a successful parse this bag holds EVERY row
// of the kind's spec table - required values as supplied, optional values
// materialized from their documented defaults - so serialize is lossless.
struct FPwSynthParams
{
    TMap<FName, FPwSynthParamValue> Values;

    bool Has(FName Name) const;
    double GetNumber(FName Name, double Default = 0.0) const;
    int32 GetInt(FName Name, int32 Default = 0) const;
    bool GetBool(FName Name, bool bDefault = false) const;
    FString GetString(FName Name, const FString& Default = FString()) const;
    const TArray<double>* GetNumbers(FName Name) const;
};

// Cross-parameter invariant a single-row spec cannot express (e.g. modal's three
// mode arrays must be the same length). Returns false and fills OutParamName /
// OutMessage on violation. OutParamName is appended to the params path so the
// caller still gets a precise field path.
using FPwSynthParamsValidator = bool (*)(const FPwSynthParams& Params, FString& OutParamName, FString& OutMessage);

// One generator or effect kind: its wire name, its one-line summary, its
// parameter table, and any cross-parameter invariant.
struct FPwSynthKindSpec
{
    const TCHAR* Name = nullptr;
    const TCHAR* Summary = nullptr;
    const TArray<FPwSynthParamSpec>* Params = nullptr;
    FPwSynthParamsValidator Validator = nullptr;

    // Human-readable statement of what Validator enforces. Published by
    // describe_schema so a cross-parameter rule is discoverable before it fires.
    const TCHAR* Constraint = nullptr;

    // Operates on the stereo mix bus. Layers are mono, so a master-only effect
    // in a layer chain is a silent no-op and is rejected instead (rpc-design §3).
    bool bMasterOnly = false;
};

struct FPwSynthMetricSpec
{
    const TCHAR* Name = nullptr;
    const TCHAR* Unit = nullptr;
    const TCHAR* Meaning = nullptr;
};

// ---------------------------------------------------------------------------
// Recipe topology.
// ---------------------------------------------------------------------------

struct FPwSynthGenerator
{
    EPwSynthGeneratorKind Kind = EPwSynthGeneratorKind::Unspecified;
    FPwSynthParams Params;
};

struct FPwSynthFx
{
    EPwSynthFxKind Kind = EPwSynthFxKind::Unspecified;
    FPwSynthParams Params;
};

// Amplitude envelope breakpoint. `TimeMs` is relative to the layer's StartMs.
// `Curve` shapes the segment from this point to the next one.
struct FPwSynthEnvelopePoint
{
    double TimeMs = 0.0;
    double Value = 0.0;                     // linear amplitude, 0..1
    EPwSynthCurve Curve = EPwSynthCurve::Linear;
};

// Pitch envelope breakpoint. `TimeMs` is relative to the layer's StartMs.
// Segments are linear in semitones; there is no per-segment curve.
struct FPwSynthPitchPoint
{
    double TimeMs = 0.0;
    double Semitones = 0.0;
};

struct FPwSynthModulation
{
    EPwSynthModulationRouting Routing = EPwSynthModulationRouting::None;
    double Depth = 0.0;
    double RateHz = 0.0;
    EPwSynthModSource Source = EPwSynthModSource::Sine;

    bool IsSet() const { return Routing != EPwSynthModulationRouting::None; }
};

struct FPwSynthLayer
{
    double StartMs = PwSynthLimits::DefaultLayerStartMs;
    double GainDb = PwSynthLimits::DefaultLayerGainDb;
    double Pan = PwSynthLimits::DefaultLayerPan;    // -1 hard left .. +1 hard right

    FPwSynthGenerator Generator;

    // Empty means "no shaping": the layer runs at unity for its whole span,
    // which can click at the edges. Documented, not silently repaired.
    TArray<FPwSynthEnvelopePoint> AmpEnvelope;
    TArray<FPwSynthPitchPoint> PitchEnvelope;

    FPwSynthModulation Modulation;
    TArray<FPwSynthFx> Fx;
};

struct FPwSynthNormalize
{
    EPwSynthNormalizeMode Mode = EPwSynthNormalizeMode::None;
    // dBFS when Mode == Peak, LUFS when Mode == Lufs. Required whenever a
    // normalize block is present: -1 dBFS and -16 LUFS are not interchangeable.
    double Target = 0.0;

    bool IsSet() const { return Mode != EPwSynthNormalizeMode::None; }
};

struct FPwSynthMaster
{
    TArray<FPwSynthFx> Fx;
    FPwSynthNormalize Normalize;
    double FadeInMs = PwSynthLimits::DefaultFadeMs;
    double FadeOutMs = PwSynthLimits::DefaultFadeMs;
};

// One optional metric range. At least one of Min / Max is always set: an empty
// range object constrains nothing and is rejected rather than accepted as a
// no-op.
struct FPwSynthTargetRange
{
    EPwSynthMetric Metric = EPwSynthMetric::PeakDb;
    TOptional<double> Min;
    TOptional<double> Max;
};

struct FPwSynthRecipe
{
    int32 Version = PwSynthLimits::RecipeVersion;
    int32 Seed = PwSynthLimits::DefaultSeed;
    int32 SampleRate = PwSynthLimits::DefaultSampleRate;
    double DurationMs = 0.0;                // required; 0 is not a valid recipe

    TArray<FPwSynthLayer> Layers;
    FPwSynthMaster Master;
    TArray<FPwSynthTargetRange> Targets;
};

// ---------------------------------------------------------------------------
// Parse failure. Code is one of ErrorCodes::ERR_{INVALID_RECIPE,
// UNKNOWN_GENERATOR, UNKNOWN_EFFECT, INVALID_PARAMS} so the handler can map a
// parse failure to a wire error code without re-deriving it from prose.
// ---------------------------------------------------------------------------
struct FPwSynthRecipeError
{
    FString Code;
    FString Field;          // JSON path, e.g. "layers[2].fx[1].kind"
    FString Message;

    bool IsSet() const { return !Code.IsEmpty(); }
    FString ToString() const;
    void Reset();
};

// ---------------------------------------------------------------------------
// Vocabulary accessors. Kinds are iterated as
//   for (uint8 i = 1; i < (uint8)EPwSynthGeneratorKind::Count; ++i)
// (index 0 is Unspecified); Curve / ModSource / Metric start at 0.
// ---------------------------------------------------------------------------

const FPwSynthKindSpec& PwSynthGeneratorSpec(EPwSynthGeneratorKind Kind);
const FPwSynthKindSpec& PwSynthFxSpec(EPwSynthFxKind Kind);

const TCHAR* PwSynthGeneratorKindToString(EPwSynthGeneratorKind Kind);
bool PwSynthGeneratorKindFromString(const FString& In, EPwSynthGeneratorKind& Out);

const TCHAR* PwSynthFxKindToString(EPwSynthFxKind Kind);
bool PwSynthFxKindFromString(const FString& In, EPwSynthFxKind& Out);

const TCHAR* PwSynthCurveToString(EPwSynthCurve Curve);
bool PwSynthCurveFromString(const FString& In, EPwSynthCurve& Out);
const TCHAR* PwSynthCurveMeaning(EPwSynthCurve Curve);

const TCHAR* PwSynthNormalizeModeToString(EPwSynthNormalizeMode Mode);
bool PwSynthNormalizeModeFromString(const FString& In, EPwSynthNormalizeMode& Out);

const TCHAR* PwSynthModRoutingToString(EPwSynthModulationRouting Routing);
bool PwSynthModRoutingFromString(const FString& In, EPwSynthModulationRouting& Out);

const TCHAR* PwSynthModSourceToString(EPwSynthModSource Source);
bool PwSynthModSourceFromString(const FString& In, EPwSynthModSource& Out);

const FPwSynthMetricSpec& PwSynthMetricSpec(EPwSynthMetric Metric);
const TCHAR* PwSynthMetricToString(EPwSynthMetric Metric);
bool PwSynthMetricFromString(const FString& In, EPwSynthMetric& Out);

// Parameter table shared by fm / am / ring modulation blocks.
const TArray<FPwSynthParamSpec>& PwSynthModulationParamSpecs();

// Wire name of a parameter type, as published by describe_schema.
const TCHAR* PwSynthParamTypeToString(EPwSynthParamType Type);

// ---------------------------------------------------------------------------
// Parse / serialize.
// ---------------------------------------------------------------------------

// Parses a recipe JSON object. On ANY failure returns false, leaves `Out`
// completely untouched, and fills OutError with the wire error code and the
// exact field path the caller must patch. Unknown object keys are rejected at
// every level, so a typo surfaces as a precise error instead of a silently
// dropped setting.
bool ParseSynthRecipe(const TSharedPtr<FJsonObject>& In, FPwSynthRecipe& Out, FPwSynthRecipeError& OutError);

// Contract overload. OutError is "<field path>: <message>".
bool ParseSynthRecipe(const TSharedPtr<FJsonObject>& In, FPwSynthRecipe& Out, FString& OutError);

// Emits the canonical JSON form: every defaulted value made explicit, every key
// in its spec-table spelling and order. Serialize(Parse(x)) round-trips x.
TSharedPtr<FJsonObject> SerializeSynthRecipe(const FPwSynthRecipe& In);

// ---------------------------------------------------------------------------
// Parameter-bag reuse seam.
//
// The schema-validated bag (FPwSynthParamSpec table -> FPwSynthParams) is the
// reusable half of this file, and a sibling schema that needs it must go through
// THIS code rather than keep a second copy that can drift. AudioGen/PwMusicScore.h
// validates its per-generation-mode parameter blocks through these two calls, so a
// range, a closed vocabulary or a default is enforced identically on both sides.
// ---------------------------------------------------------------------------

// Validates ParamsObj against Kind's table, materializing every documented default
// into `Out` so serialization is lossless, then runs Kind's cross-parameter
// Validator. ParamsObj may be null - a kind whose parameters are all optional
// accepts an absent block. KindNoun is the word used in error prose ("generator",
// "effect", "generation mode"); ParamsPath is the JSON path of the block, which
// every error message is rooted at. Returns false with `Out` unusable on failure.
bool ParseSynthParamBag(const TSharedPtr<FJsonObject>& ParamsObj, const FPwSynthKindSpec& Kind,
    const TCHAR* KindNoun, const FString& ParamsPath, FPwSynthParams& Out, FPwSynthRecipeError& OutError);

// Emits a bag by walking Kind's spec table, so keys carry the canonical spelling in
// the canonical order regardless of how the caller spelled them.
TSharedPtr<FJsonObject> SerializeSynthParamBag(const FPwSynthParams& In, const FPwSynthKindSpec& Kind);
