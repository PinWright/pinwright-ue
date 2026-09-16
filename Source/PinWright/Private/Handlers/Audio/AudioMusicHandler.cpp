// Copyright (c) 2026 Alexander Penkin. MIT License.

// AudioMusicHandler.cpp - audio.music.describe_schema / render_stems / export_stems /
// build_interactive
//
// The whole music pipeline, in four calls:
//
//   describe_schema   the grammar, so a first score fails on taste rather than on structure
//   render_stems      score document -> one seamless PCM stem per track, as session candidates
//   export_stems      stem candidates -> one USoundWave asset per track, verified by decode-back
//   build_interactive stem ASSETS    -> one interactive MetaSound Source that crossfades them
//
// -------------------------------------------------------------------------------------------
// WHY THE PIPELINE HAS FOUR STAGES AND NOT ONE
// -------------------------------------------------------------------------------------------
// Each boundary is a place a caller genuinely has to make a decision:
//   - after render_stems the caller reads the LOOP REPORT and decides whether an inexact loop is
//     acceptable, or re-renders at NearestExactBpm. Folding that into an export would take the
//     decision away and ship a click once per cycle;
//   - after export_stems the stems exist as assets any MetaSound, SoundCue or Quartz scheduler
//     can use. A project that already has its own dynamic-music graph stops here;
//   - build_interactive is the convenience for a project that does not, and it is the only stage
//     that needs MetaSound at all.
//
// -------------------------------------------------------------------------------------------
// WHY BARE USoundWave LOOP METADATA IS NOT AN OPTION (and why the stems are what they are)
// -------------------------------------------------------------------------------------------
// FSoundWaveCuePoint and USoundWave::GetLoopRegions() serialize into the asset, but no decoder
// and no mixer source reads them - only the MetaSound Wave Player's Loop / Loop Start / Loop
// Duration pins honour loop points. A music bed that relies on wave metadata does not loop. So
// PwRenderScoreStems makes each stem seamless BY CONSTRUCTION (render past the loop point, wrap
// the overhang onto the head), and build_interactive exists because the Wave Player is the only
// runtime that can be told to loop a region.
//
// -------------------------------------------------------------------------------------------
// WHICH VERBS ARE JOBS (rpc-design.md §9)
// -------------------------------------------------------------------------------------------
// render_stems and export_stems are, because both are bounded by the caller's own numbers rather
// than by a fixed cost: a score may hold 16 tracks of 512 notes and the renderer builds one
// instrument instance per note, and an export decodes every written wave back. describe_schema
// reads static tables and build_interactive edits one document, so both answer inline - which
// also keeps their failures as real error CODES rather than a job's bare error string (§7).
//
// NEITHER JOB CAN BE CANCELLED, and neither claims it can. Nothing here registers a cancel
// callback, so FJobRegistry::Cancel answers Unsupported (JOB_CANCEL_UNSUPPORTED). Cancelling is
// exactly what an agent does in response to a hang, so a fake success there is worse than the
// missing capability. Ctx.StartJob is NOT a deferral primitive - FHandlerContext::StartJob
// invokes the bind delegate synchronously - so every argument is validated and every source
// resolved BEFORE the ticket is issued, and the work still runs on the caller's stack.
//
// -------------------------------------------------------------------------------------------
// IDEMPOTENCE (rpc-design.md §8)
// -------------------------------------------------------------------------------------------
// The transport's timeout is response-only: when it fires the handler keeps running and its work
// commits, so a retry must CONVERGE rather than accumulate.
//   - render_stems memoizes the canonical score digest -> the stem candidate ids it produced. A
//     retry that finds every one of those ids STILL RESIDENT (each verified through
//     FPwCandidateRegistry::Get, never assumed) reuses them and reports reused:true instead of
//     filling the registry with byte-identical copies. A render is deterministic in the score, so
//     the reused candidates hold exactly the samples this call would have produced.
//   - export_stems goes through AssetCreatePolicy::Resolve + the engine writer's in-place
//     NewObject, so a retry rewrites the same USoundWave rather than creating a `_1` sibling.
//   - build_interactive names the asset explicitly and PwBuildInteractiveMusicGraph rebuilds that
//     document, so a retry converges on one MetaSound Source.
//
// -------------------------------------------------------------------------------------------
// RESPONSE SIZE
// -------------------------------------------------------------------------------------------
// The wrapped MCP ToolResult carries the payload twice - escaped in content[0].text and verbatim
// in structuredContent - so the real ceiling for a bare result is about 4,250 characters, not the
// 10,000 the spill constant suggests (board ticket E-spill-threshold-measured-post-wrap). That is
// why render_stems returns a compact per-stem TABLE (name, id, four numbers) rather than 16 full
// stem reports, why export_stems returns one row per asset, and why describe_schema is sectioned
// with a digest-only default for the generation modes. Audio never travels inline in either
// direction: a stem leaves as a candidate id, and an exported stem as an asset path.
// TestAudioMusicHandler.cpp holds the gates so growth trips CI instead of a user's response.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "PinWrightHelpers.h"
#include "Utils/AssetCreatePolicy.h"
#include "Utils/GuardedLoad.h"
#include "Utils/PathUtils.h"

#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwAudioDecode.h"
#include "AudioGen/PwAudioExport.h"
#include "AudioGen/PwCandidateRegistry.h"
#include "AudioGen/PwMusicGraph.h"
#include "AudioGen/PwMusicRender.h"
#include "AudioGen/PwMusicScore.h"
#include "AudioGen/PwSynthRecipe.h"
#include "State/PluginState.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/CriticalSection.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sound/SoundWave.h"
#include "UObject/UObjectGlobals.h"

// Named (not anonymous) namespace: the main module builds with Unity on, and two anonymous
// namespaces merged into one TU collide by name. See CLAUDE.md > Building.
namespace PwMusicHandlerInternal
{
    // ---------------------------------------------------------------------------------------
    // Small JSON helpers
    // ---------------------------------------------------------------------------------------

    /**
     * Emits a rounded number, or NOTHING when the value is not finite.
     *
     * Rounding is not cosmetic: UE's JSON writer prints doubles with "%.17g", so one unrounded
     * peak costs twenty characters of a response budget measured in thousands. Omission on a
     * non-finite value follows the analysis serializer's rule - an absent field is explicit, a
     * fabricated 0.0 reads as a measurement.
     */
    void SetRounded(const TSharedPtr<FJsonObject>& Out, const TCHAR* Key, double Value, int32 Decimals)
    {
        if (!FMath::IsFinite(Value))
        {
            return;
        }
        const double Scale = FMath::Pow(10.0, static_cast<double>(Decimals));
        Out->SetNumberField(Key, FMath::RoundToDouble(Value * Scale) / Scale);
    }

    TArray<TSharedPtr<FJsonValue>> StringArray(const TArray<FString>& Items)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        Out.Reserve(Items.Num());
        for (const FString& Item : Items)
        {
            Out.Add(MakeShared<FJsonValueString>(Item));
        }
        return Out;
    }

    /** Condensed encoding, used only for digesting a document - never for a response. */
    FString ToCondensedJson(const TSharedPtr<FJsonObject>& In)
    {
        FString Text;
        const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
        if (In.IsValid())
        {
            FJsonSerializer::Serialize(In.ToSharedRef(), Writer);
        }
        return Text;
    }

    /**
     * Identity of a score for the render memo.
     *
     * Taken from SerializeMusicScore's CANONICAL form rather than from the caller's document, so
     * two spellings of one score (defaults omitted vs. made explicit, keys in another order)
     * digest alike - which is what makes a retry converge even when the client re-serialized its
     * payload in between.
     *
     * Hashed over the raw TCHAR bytes rather than through FMD5::HashAnsiString, whose ANSI
     * conversion is lossy: two scores differing only in a non-ASCII section name would digest
     * alike, and a memo hit on the wrong score would hand the caller someone else's stems while
     * reporting reused:true. Remote, but it is the one failure this key must not have.
     */
    FString ScoreDigest(const FPwMusicScore& Score)
    {
        const FString Canonical = ToCondensedJson(SerializeMusicScore(Score));
        const FSHAHash Hash = FSHA1::HashBuffer(
            *Canonical, static_cast<uint64>(Canonical.Len()) * sizeof(TCHAR));
        return Hash.ToString();
    }

    // ---------------------------------------------------------------------------------------
    // The loop report, published verbatim from FPwScoreLoop
    // ---------------------------------------------------------------------------------------

    /**
     * All three loop facts, always: exact, the SIGNED residual, and the tempo that would be
     * exact. An inexact loop is the caller's decision to make - accept the sub-frame drift, move
     * the tempo, or change the sample rate - and hiding any of the three would make it look like
     * a decision nobody had to take (§1).
     */
    TSharedPtr<FJsonObject> LoopToJson(const FPwScoreLoop& Loop)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetBoolField(TEXT("exact"), Loop.bExact);
        Out->SetNumberField(TEXT("frames"), static_cast<double>(Loop.Frames));
        SetRounded(Out, TEXT("durationMs"), Loop.DurationMs, 4);
        // Signed, and emitted even when zero: a caller distinguishing "the loop is long by a
        // third of a frame" from "short by a third" needs the sign, and a one-sided measure would
        // score the two alike (§6).
        SetRounded(Out, TEXT("residualFrames"), Loop.ResidualFrames, 6);
        // The ONE number here that is deliberately not rounded. It is recovery information whose
        // entire value is that re-rendering at it produces an exact loop, and six decimal places is
        // not enough to preserve that: the tempo is a ratio (frames into bars x beats x 60000 x
        // rate), so a rounded copy lands a fraction of a frame off and the caller's "fix" comes back
        // inexact. Costs ~18 characters once per response.
        if (FMath::IsFinite(Loop.NearestExactBpm))
        {
            Out->SetNumberField(TEXT("nearestExactBpm"), Loop.NearestExactBpm);
        }
        return Out;
    }
}

// =================================================================================================
// audio.music.describe_schema
//
// With no preset library shipping, this verb plus the wiki page is the entire cold-start path for a
// caller writing its first score, so it publishes the whole grammar as structured JSON. Every
// vocabulary below is read out of PwMusicScore.cpp's own tables, so the documentation cannot drift
// from the validation - adding a scale, a role or a generation parameter updates both at once.
//
// SIZE. The sections exist because the whole grammar does not fit one response, and the binding
// ceiling is ~4,250 characters, not the 10,000 spill constant (the wrapped ToolResult carries the
// payload twice). `overview` is the shape, the beat convention, the caps and the required list -
// enough to write a score that fails on taste rather than on structure. `rules`, `example`,
// `scales`, `roles` and `modes` are the drill-downs, `modes` returning DIGESTS unless `mode` names
// one, so a generation mode gaining parameters cannot grow it. Only `all` deliberately exceeds the
// inline threshold and comes back as a spill file.
// =================================================================================================

namespace PwMusicHandlerInternal
{
    const TCHAR* const SectionOverview = TEXT("overview");
    const TCHAR* const SectionRules    = TEXT("rules");
    const TCHAR* const SectionExample  = TEXT("example");
    const TCHAR* const SectionScales   = TEXT("scales");
    const TCHAR* const SectionRoles    = TEXT("roles");
    const TCHAR* const SectionModes    = TEXT("modes");
    const TCHAR* const SectionAll      = TEXT("all");

    /**
     * A minimal but complete score: two tracks, one generative and one explicit, two sections that
     * tile the score, and an instrument that is a plain synth layer. Kept in its authored (terse)
     * form so it also shows which fields may be omitted. Parsed at request time, never asserted -
     * `exampleValidated` is the verdict of the real parser, so an example that stopped validating
     * cannot ship as documentation.
     */
    const TCHAR* const ExampleScoreJson = TEXT(R"JSON(
{
  "bpm": 120,
  "bars": 8,
  "seed": 7,
  "timeSignature": { "numerator": 4, "denominator": 4 },
  "key": { "root": "a", "scale": "natural_minor_aeolian" },
  "sections": [
    { "name": "calm", "startBar": 0, "bars": 4, "intensityFloor": 0.1, "intensityCeil": 0.4 },
    { "name": "rise", "startBar": 4, "bars": 4, "intensityFloor": 0.4, "intensityCeil": 0.9 }
  ],
  "tracks": [
    {
      "name": "pad", "role": "pad", "octave": 3, "gainDb": -6, "pan": -0.2,
      "instrument": {
        "generator": { "kind": "osc", "params": { "waveform": "saw", "frequencyHz": 220 } },
        "ampEnvelope": [
          { "timeMs": 0, "value": 0, "curve": "exp" },
          { "timeMs": 900, "value": 0.8 },
          { "timeMs": 2400, "value": 0 }
        ],
        "fx": [ { "kind": "filter", "params": { "type": "lowpass", "cutoffHz": 2400 } } ]
      },
      "generation": {
        "mode": "evolving_pad",
        "params": { "degrees": [0, 2, 4, 6], "swellBeatsMin": 4, "swellBeatsMax": 8 }
      }
    },
    {
      "name": "bass", "role": "bass", "octave": 1,
      "instrument": {
        "generator": { "kind": "osc", "params": { "waveform": "square", "frequencyHz": 55 } },
        "ampEnvelope": [
          { "timeMs": 0, "value": 1, "curve": "exp" },
          { "timeMs": 400, "value": 0 }
        ]
      },
      "generation": { "mode": "explicit" },
      "notes": [
        { "startBeat": 0, "durationBeats": 1, "degree": 0, "velocity": 0.9 },
        { "startBeat": 2, "durationBeats": 1, "degree": 0 },
        { "startBeat": 4, "durationBeats": 1, "degree": -3 }
      ]
    }
  ]
}
)JSON");

    /** One parameter row, exactly as the parser enforces it. */
    TSharedPtr<FJsonObject> ParamToJson(const FPwSynthParamSpec& Spec)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Spec.DisplayName);
        Obj->SetStringField(TEXT("type"), PwSynthParamTypeToString(Spec.Type));
        if (Spec.Unit)
        {
            Obj->SetStringField(TEXT("unit"), Spec.Unit);
        }
        Obj->SetBoolField(TEXT("required"), Spec.bRequired);

        if (Spec.Type == EPwSynthParamType::Enum && Spec.EnumValues)
        {
            TArray<FString> Accepted;
            FString(Spec.EnumValues).ParseIntoArray(Accepted, TEXT("|"), true);
            Obj->SetArrayField(TEXT("accepted"), StringArray(Accepted));
        }
        else if (Spec.Type != EPwSynthParamType::Boolean && Spec.Type != EPwSynthParamType::String)
        {
            if (Spec.bHasMin)
            {
                Obj->SetNumberField(TEXT("min"), Spec.Min);
            }
            if (Spec.bHasMax)
            {
                Obj->SetNumberField(TEXT("max"), Spec.Max);
            }
        }

        if (Spec.Type == EPwSynthParamType::NumberArray)
        {
            Obj->SetNumberField(TEXT("minItems"), Spec.MinArrayNum);
            Obj->SetNumberField(TEXT("maxItems"), Spec.MaxArrayNum);
        }

        if (!Spec.bRequired)
        {
            switch (Spec.Type)
            {
            case EPwSynthParamType::Boolean:
                Obj->SetBoolField(TEXT("default"), Spec.DefaultNumber != 0.0);
                break;
            case EPwSynthParamType::String:
            case EPwSynthParamType::Enum:
                Obj->SetStringField(TEXT("default"), Spec.DefaultString ? Spec.DefaultString : TEXT(""));
                break;
            default:
                Obj->SetNumberField(TEXT("default"), Spec.DefaultNumber);
                break;
            }
        }

        Obj->SetStringField(TEXT("meaning"), Spec.Meaning ? Spec.Meaning : TEXT(""));
        return Obj;
    }

    /**
     * Compact per-mode entry: enough to pick a mode, with no parameter data. Keeping the digest
     * parameter-free means a later generation parameter cannot grow the default `modes` response
     * toward the ceiling at all - the drill-down owns that.
     */
    TSharedPtr<FJsonObject> GenerationDigestToJson(EPwMusicGenerationMode Mode)
    {
        const FPwSynthKindSpec& Spec = PwMusicGenerationSpec(Mode);
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("mode"), Spec.Name);
        Obj->SetStringField(TEXT("summary"), Spec.Summary);

        TArray<FString> Required;
        TArray<FString> Optional;
        if (Spec.Params)
        {
            for (const FPwSynthParamSpec& Param : *Spec.Params)
            {
                (Param.bRequired ? Required : Optional).Add(Param.DisplayName);
            }
        }
        Obj->SetArrayField(TEXT("required"), StringArray(Required));
        Obj->SetArrayField(TEXT("optional"), StringArray(Optional));
        return Obj;
    }

    TSharedPtr<FJsonObject> GenerationModeToJson(EPwMusicGenerationMode Mode)
    {
        const FPwSynthKindSpec& Spec = PwMusicGenerationSpec(Mode);
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("mode"), Spec.Name);
        Obj->SetStringField(TEXT("summary"), Spec.Summary);
        if (Spec.Constraint)
        {
            Obj->SetStringField(TEXT("constraint"), Spec.Constraint);
        }
        // The one cross-field rule the parameter table cannot express, and the most common first
        // failure: a generative track carrying notes is an error, not a merge.
        Obj->SetStringField(TEXT("notes"), Mode == EPwMusicGenerationMode::Explicit
            ? TEXT("notes[] is required and authoritative.")
            : TEXT("notes[] must be absent."));

        TArray<TSharedPtr<FJsonValue>> Params;
        if (Spec.Params)
        {
            Params.Reserve(Spec.Params->Num());
            for (const FPwSynthParamSpec& Param : *Spec.Params)
            {
                Params.Add(MakeShared<FJsonValueObject>(ParamToJson(Param)));
            }
        }
        Obj->SetArrayField(TEXT("params"), Params);
        return Obj;
    }

    void AddTopology(const TSharedPtr<FJsonObject>& Result)
    {
        TSharedPtr<FJsonObject> Topology = MakeShared<FJsonObject>();
        Topology->SetStringField(TEXT("score"),
            TEXT("{version, seed, sampleRate, bpm, timeSignature{numerator,denominator}, key{root,scale}, bars, sections[], tracks[]}"));
        Topology->SetStringField(TEXT("section"),
            TEXT("{name, startBar, bars, intensityFloor, intensityCeil}. Sections must TILE the score: first at bar 0, each starting where the previous ended, the last ending at bars. A gap is rejected."));
        Topology->SetStringField(TEXT("track"),
            TEXT("{name, role, octave, gainDb, pan, instrument, generation{mode,params}, notes[]}. One track renders to one stem and its name becomes that stem's asset name."));
        Topology->SetStringField(TEXT("note"),
            TEXT("{startBeat, durationBeats, degree|midi, velocity}. Exactly one of degree/midi: degree indexes the key's scale with 0 = the root at the track's octave, midi is absolute and is never snapped to the key."));
        Topology->SetStringField(TEXT("instrument"),
            TEXT("A synth layer verbatim - {generator, ampEnvelope, pitchEnvelope, modulation, fx}; call audio.synth.describe_schema for that grammar. startMs, gainDb and pan are REJECTED inside it because the score already owns them."));
        Topology->SetStringField(TEXT("intensity"),
            TEXT("A section's band remaps GENERATED velocity into [floor, ceil]. It never touches authored notes on an explicit track."));
        Result->SetObjectField(TEXT("topology"), Topology);
    }

    void AddBeatConvention(const TSharedPtr<FJsonObject>& Result)
    {
        TSharedPtr<FJsonObject> Beat = MakeShared<FJsonObject>();
        Beat->SetStringField(TEXT("rule"),
            TEXT("One beat is one timeSignature.denominator note and bpm counts THOSE per minute, so a bar is exactly numerator beats with no 4/denominator correction: in 6/8 at 90 bpm an eighth lasts 666.67 ms and a bar lasts 4 s."));
        Beat->SetStringField(TEXT("why"),
            TEXT("Quartz's own convention (FQuartzTimeSignature is {NumBeats, BeatType} where BeatType IS the denominator), because a score authored here is meant to be handed to Quartz. There is no tempo map in v1: one bpm governs the whole score."));
        TArray<TSharedPtr<FJsonValue>> Denominators;
        for (const int32 Denominator : PwMusicTimeSignatureDenominators())
        {
            Denominators.Add(MakeShared<FJsonValueNumber>(Denominator));
        }
        Beat->SetArrayField(TEXT("denominators"), Denominators);
        Beat->SetStringField(TEXT("denominatorsWhy"),
            TEXT("The five EQuartzTimeSignatureQuantization can express. A /1 score could not be scheduled, so it is rejected rather than accepted and stranded."));
        Result->SetObjectField(TEXT("beat"), Beat);
    }

    void AddCaps(const TSharedPtr<FJsonObject>& Result)
    {
        TSharedPtr<FJsonObject> Caps = MakeShared<FJsonObject>();
        Caps->SetNumberField(TEXT("maxTracks"), PwMusicLimits::MaxTracks);
        Caps->SetNumberField(TEXT("maxSections"), PwMusicLimits::MaxSections);
        Caps->SetNumberField(TEXT("maxNotesPerTrack"), PwMusicLimits::MaxNotesPerTrack);
        Caps->SetNumberField(TEXT("minBars"), PwMusicLimits::MinBars);
        Caps->SetNumberField(TEXT("maxBars"), PwMusicLimits::MaxBars);
        Caps->SetNumberField(TEXT("minBpm"), PwMusicLimits::MinBpm);
        Caps->SetNumberField(TEXT("maxBpm"), PwMusicLimits::MaxBpm);
        Caps->SetNumberField(TEXT("maxTimeSignatureNumerator"), PwMusicLimits::MaxTimeSignatureNumerator);
        Caps->SetNumberField(TEXT("minOctave"), PwMusicLimits::MinOctave);
        Caps->SetNumberField(TEXT("maxOctave"), PwMusicLimits::MaxOctave);
        // Published here rather than left for build_interactive to refuse, because it constrains the
        // SCORE a caller is about to write and finding it two verbs later is a wasted render.
        Caps->SetNumberField(TEXT("maxInteractiveGraphStems"), PwMusicGraphMaxStems);
        Caps->SetStringField(TEXT("enforcement"),
            TEXT("A cap is a rejection naming the field path, never a clamp. maxTracks is also the stem-asset cap; maxInteractiveGraphStems is lower because the engine's Audio Mixer node tops out there, so a wider score renders and exports but needs more than one interactive graph."));
        Result->SetObjectField(TEXT("caps"), Caps);
    }

    void AddRequired(const TSharedPtr<FJsonObject>& Result)
    {
        // Only values that are never wrong in the field carry a default; everything else is
        // required so a guess can never reach the render (rpc-design.md §3).
        TArray<FString> Required = {
            TEXT("bpm, bars - no default tempo and no default length"),
            TEXT("timeSignature.numerator and .denominator"),
            TEXT("key.root and key.scale"),
            TEXT("tracks[] - at least one, each with name, role, octave, instrument, generation.mode"),
            TEXT("tracks[].octave - register separates a bass bed from a lead and no value is right for both"),
            TEXT("tracks[].notes - required for mode 'explicit', forbidden for every generative mode"),
            TEXT("every parameter marked required in section 'modes'")
        };
        Result->SetArrayField(TEXT("required"), StringArray(Required));
    }

    void AddRules(const TSharedPtr<FJsonObject>& Result)
    {
        TSharedPtr<FJsonObject> Defaults = MakeShared<FJsonObject>();
        Defaults->SetNumberField(TEXT("version"), PwMusicLimits::ScoreVersion);
        Defaults->SetNumberField(TEXT("seed"), PwMusicLimits::DefaultSeed);
        Defaults->SetNumberField(TEXT("sampleRate"), PwMusicLimits::DefaultSampleRate);
        Defaults->SetNumberField(TEXT("tracks[].gainDb"), PwMusicLimits::DefaultTrackGainDb);
        Defaults->SetNumberField(TEXT("tracks[].pan"), PwMusicLimits::DefaultTrackPan);
        Defaults->SetNumberField(TEXT("tracks[].notes[].velocity"), PwMusicLimits::DefaultVelocity);
        Defaults->SetNumberField(TEXT("sections[].intensityFloor"), PwMusicLimits::DefaultIntensityFloor);
        Defaults->SetNumberField(TEXT("sections[].intensityCeil"), PwMusicLimits::DefaultIntensityCeil);
        Result->SetObjectField(TEXT("defaults"), Defaults);

        TArray<FString> Rules = {
            TEXT("An unrecognised scale, key root, track role or generation mode is REJECTED naming the valid set. It never degrades into major / lead / explicit."),
            TEXT("An unknown object key is rejected at every level and the error names its exact path (tracks[2].notes[17].degree), because that is what you patch."),
            TEXT("Nothing is clamped: a cap, a range, a section tiling gap, a duplicate track name or an ordering violation is an error naming the field."),
            TEXT("Key roots are sharps only. 'db' is not an alias for 'c#' - accepting a second spelling would break the byte-stable round trip."),
            TEXT("An authored `midi` note is never snapped into the key. Silently moving a pitch the caller wrote down is the failure this schema exists to prevent."),
            TEXT("Track names become asset names, so the charset is [A-Za-z0-9_-] and uniqueness is checked CASE-INSENSITIVELY - 'Pad' and 'pad' would be one file on Windows."),
            TEXT("The same score and seed render byte-identical stems, in this session and after an editor restart; each track draws from a substream hashed from the seed and the track index, so editing track 2 cannot move a sample of track 1.")
        };
        Result->SetArrayField(TEXT("strictness"), StringArray(Rules));

        TSharedPtr<FJsonObject> Loop = MakeShared<FJsonObject>();
        Loop->SetStringField(TEXT("metadataIsInert"),
            TEXT("Bare USoundWave loop metadata does nothing at runtime: FSoundWaveCuePoint and GetLoopRegions() serialize into the asset, but no decoder and no mixer source reads them. Only the MetaSound Wave Player's Loop / Loop Start / Loop Duration pins honour loop points."));
        Loop->SetStringField(TEXT("construction"),
            TEXT("So a stem is seamless BY CONSTRUCTION: render_stems renders past the loop point and wraps the overhang onto the head, continuing a note tail into the next repetition instead of cutting it. Nothing is faded - a fade would duck the bed once per cycle."));
        Loop->SetStringField(TEXT("exactness"),
            TEXT("A loop must be a whole number of frames. render_stems reports loop.exact, the SIGNED loop.residualFrames and loop.nearestExactBpm rather than rounding, because a rounded loop is a click once per cycle."));
        Loop->SetStringField(TEXT("compressionCaveat"),
            TEXT("An exact frame count is necessary but not sufficient: block-based compression (Vorbis/Opus/ADPCM/Bink) pads to a whole block and ramps in on the first block. A stem that must loop seamlessly stays PCM, or is played through a Wave Player given real loop points."));
        Result->SetObjectField(TEXT("loops"), Loop);
    }

    void AddScales(const TSharedPtr<FJsonObject>& Result)
    {
        TArray<TSharedPtr<FJsonValue>> Scales;
        for (uint8 Index = 1; Index < static_cast<uint8>(EPwMusicScale::Count); ++Index)
        {
            const EPwMusicScale Scale = static_cast<EPwMusicScale>(Index);
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("name"), PwMusicScaleToString(Scale));
            int32 DegreeCount = 0;
            // Read from the engine's own table, and OMITTED when the table has no entry: a
            // fabricated 0 would read as "this scale has no degrees" (§4).
            if (PwMusicScaleDegreeCount(Scale, DegreeCount))
            {
                Entry->SetNumberField(TEXT("degreesPerOctave"), DegreeCount);
            }
            Scales.Add(MakeShared<FJsonValueObject>(Entry));
        }
        Result->SetArrayField(TEXT("scales"), Scales);

        TArray<FString> Roots;
        for (uint8 Index = 1; Index < static_cast<uint8>(EPwMusicPitchClass::Count); ++Index)
        {
            Roots.Add(PwMusicPitchClassToString(static_cast<EPwMusicPitchClass>(Index)));
        }
        Result->SetArrayField(TEXT("keyRoots"), StringArray(Roots));
        Result->SetStringField(TEXT("scaleSource"),
            TEXT("EPwMusicScale mirrors Audio::EMusicalScale::Scale 1:1 and degree resolution reads Audio::FMidiNoteQuantizer::ScaleDegreeSetMap - the same table MetaSound's MIDI-note-quantizer node uses, so a score authored here and a graph built there agree about what 'phrygian' means."));
    }

    void AddRoles(const TSharedPtr<FJsonObject>& Result)
    {
        TArray<TSharedPtr<FJsonValue>> Roles;
        for (uint8 Index = 1; Index < static_cast<uint8>(EPwMusicTrackRole::Count); ++Index)
        {
            const EPwMusicTrackRole Role = static_cast<EPwMusicTrackRole>(Index);
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("role"), PwMusicTrackRoleToString(Role));
            Entry->SetStringField(TEXT("meaning"), PwMusicTrackRoleMeaning(Role));
            Roles.Add(MakeShared<FJsonValueObject>(Entry));
        }
        Result->SetArrayField(TEXT("roles"), Roles);
        Result->SetStringField(TEXT("roleSemantics"),
            TEXT("A role is metadata for the stem consumer and carries no DSP meaning: two tracks with the same instrument and different roles render identically. It is still a closed set, because a role nothing downstream recognises is a stem nothing mixes in."));
    }

    void AddExample(const TSharedPtr<FJsonObject>& Result)
    {
        TSharedPtr<FJsonObject> Example;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FString(ExampleScoreJson));
        const bool bDeserialized = FJsonSerializer::Deserialize(Reader, Example) && Example.IsValid();

        // `exampleValidated` is the verdict of the production parser, not a literal: a built-in
        // example that stopped validating must not ship as documentation (§1).
        bool bValidated = false;
        FPwMusicScoreError ParseError;
        if (bDeserialized)
        {
            FPwMusicScore Parsed;
            bValidated = ParseMusicScore(Example, Parsed, ParseError);
            Result->SetObjectField(TEXT("example"), Example);
        }
        Result->SetBoolField(TEXT("exampleValidated"), bValidated);
        if (!bValidated)
        {
            Result->SetStringField(TEXT("exampleError"), bDeserialized
                ? ParseError.ToString()
                : TEXT("the built-in example is not valid JSON"));
        }
        Result->SetStringField(TEXT("exampleNote"),
            TEXT("Authored form: omitted optional fields fall back to the documented defaults. A score echoed back by this subsystem has every default made explicit."));
    }
}

REGISTER_RPC_HANDLER("audio.music.describe_schema", "audio.music",
    "Machine-readable grammar of the audio.music score: the topology, the beat convention (one beat "
    "is one timeSignature.denominator note, matching Quartz), the 30 scales and 12 key roots read "
    "from the engine's own quantizer table, the track roles, the four generation modes with every "
    "parameter's type, unit, range and required-or-default state, the caps, the seamless-loop "
    "contract, and a worked example validated by the real parser at request time. Read this before "
    "writing a first score; an instrument is a synth layer, so audio.synth.describe_schema is the "
    "other half of the grammar.",
    RPC_PARAMS(
        RPC_PARAM_DEF("section", "string",
            "Which slice to return. 'overview' (default) is the topology, beat convention, caps and "
            "required list plus a digest of every generation mode and role. 'rules' is the defaults, "
            "the strictness rules and the seamless-loop contract; 'example' is a worked score; "
            "'scales' / 'roles' / 'modes' are the vocabulary drill-downs. 'all' concatenates "
            "everything and deliberately exceeds the inline threshold, so it comes back as a spill "
            "file.",
            "overview"),
        RPC_PARAM_OPT("mode", "string",
            "Restrict the 'modes' section to one generation mode (e.g. 'evolving_pad') and return "
            "its full parameter table instead of a digest. An unrecognised mode is rejected rather "
            "than ignored. Has no effect on the other sections.")
    ))
{
    using namespace PwMusicHandlerInternal;

    const FString Section = Ctx.GetString(TEXT("section"), SectionOverview).TrimStartAndEnd().ToLower();
    const FString ModeFilterName = Ctx.GetString(TEXT("mode")).TrimStartAndEnd();

    const TArray<FString> ValidSections = {
        SectionOverview, SectionRules, SectionExample, SectionScales, SectionRoles, SectionModes,
        SectionAll
    };
    if (!ValidSections.Contains(Section))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("section: '%s' is not a section. Accepted: %s."),
                *Section, *FString::Join(ValidSections, TEXT(", "))));
        return true;
    }

    // A mode filter that matches nothing is a caller error, not an empty result: a zero-entry
    // section reads identically to "this mode has no parameters" (§3).
    EPwMusicGenerationMode ModeFilter = EPwMusicGenerationMode::Unspecified;
    if (!ModeFilterName.IsEmpty())
    {
        if (!PwMusicGenerationModeFromString(ModeFilterName, ModeFilter) ||
            ModeFilter == EPwMusicGenerationMode::Unspecified)
        {
            TArray<FString> Accepted;
            for (uint8 Index = 1; Index < static_cast<uint8>(EPwMusicGenerationMode::Count); ++Index)
            {
                Accepted.Add(PwMusicGenerationModeToString(static_cast<EPwMusicGenerationMode>(Index)));
            }
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("mode: '%s' is not a generation mode. Accepted: %s."),
                    *ModeFilterName, *FString::Join(Accepted, TEXT(", "))));
            return true;
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("section"), Section);
    Result->SetNumberField(TEXT("scoreVersion"), PwMusicLimits::ScoreVersion);

    const bool bAll = (Section == SectionAll);

    if (bAll || Section == SectionOverview)
    {
        Result->SetArrayField(TEXT("sections"), StringArray(ValidSections));
        Result->SetStringField(TEXT("pipeline"),
            TEXT("render_stems (score -> one candidate per track) -> export_stems (candidates -> USoundWave assets) -> build_interactive (assets -> one MetaSound Source). Nothing writes a file: audio leaves as an asset."));
        AddTopology(Result);
        AddBeatConvention(Result);
        AddCaps(Result);
        AddRequired(Result);

        TArray<FString> ModeNames;
        for (uint8 Index = 1; Index < static_cast<uint8>(EPwMusicGenerationMode::Count); ++Index)
        {
            ModeNames.Add(PwMusicGenerationModeToString(static_cast<EPwMusicGenerationMode>(Index)));
        }
        Result->SetArrayField(TEXT("generationModes"), StringArray(ModeNames));

        TArray<FString> RoleNames;
        for (uint8 Index = 1; Index < static_cast<uint8>(EPwMusicTrackRole::Count); ++Index)
        {
            RoleNames.Add(PwMusicTrackRoleToString(static_cast<EPwMusicTrackRole>(Index)));
        }
        Result->SetArrayField(TEXT("trackRoles"), StringArray(RoleNames));
        Result->SetNumberField(TEXT("scaleCount"), static_cast<int32>(EPwMusicScale::Count) - 1);
    }

    if (bAll || Section == SectionRules)
    {
        AddRules(Result);
    }

    if (bAll || Section == SectionExample)
    {
        AddExample(Result);
    }

    if (bAll || Section == SectionScales)
    {
        AddScales(Result);
    }

    if (bAll || Section == SectionRoles)
    {
        AddRoles(Result);
    }

    if (bAll || Section == SectionModes)
    {
        TArray<TSharedPtr<FJsonValue>> Modes;
        if (ModeFilter != EPwMusicGenerationMode::Unspecified)
        {
            Modes.Add(MakeShared<FJsonValueObject>(GenerationModeToJson(ModeFilter)));
        }
        else if (bAll)
        {
            for (uint8 Index = 1; Index < static_cast<uint8>(EPwMusicGenerationMode::Count); ++Index)
            {
                Modes.Add(MakeShared<FJsonValueObject>(
                    GenerationModeToJson(static_cast<EPwMusicGenerationMode>(Index))));
            }
        }
        else
        {
            // Digest-only by default. The full four-mode table runs past the wrapped-response
            // ceiling, and a section that silently spilled would cost the caller a file read on the
            // cold-start path; `mode` narrows it to the one they are writing.
            for (uint8 Index = 1; Index < static_cast<uint8>(EPwMusicGenerationMode::Count); ++Index)
            {
                Modes.Add(MakeShared<FJsonValueObject>(
                    GenerationDigestToJson(static_cast<EPwMusicGenerationMode>(Index))));
            }
            Result->SetStringField(TEXT("modesDetail"),
                TEXT("digests only - pass mode:'<name>' for one mode's full parameter table, or section:'all' for every table (which spills to a file)."));
        }
        Result->SetArrayField(TEXT("modes"), Modes);
    }

    Ctx.SendSuccess(FString::Printf(TEXT("audio.music score schema, section '%s'."), *Section), Result);
    return true;
}

// =================================================================================================
// audio.music.render_stems
//
// A JOB, because the work is bounded by the caller's numbers rather than by a fixed cost: a score
// may carry 16 tracks of 512 notes and PwRenderScoreStems builds one instrument instance PER NOTE.
// Every argument is validated and the score fully parsed BEFORE the ticket is issued, so a caller
// error is a real error code carrying the parser's field path and not a failed job with a bare
// string (rpc-design.md §7, §9).
// =================================================================================================

namespace PwMusicHandlerInternal
{
    // ---------------------------------------------------------------------------------------
    // The render memo (rpc-design.md §8)
    //
    // The transport's timeout is response-only: when it fires the handler keeps running and its
    // stems land in the registry anyway, so a client retry must CONVERGE rather than add a second
    // byte-identical set. A render is deterministic in the score, so the canonical score digest is
    // a sufficient identity, and the memo maps that digest to the candidate ids it produced.
    //
    // Residency is MEASURED on every reuse, never assumed: each remembered id is resolved through
    // FPwCandidateRegistry::Get, and one eviction anywhere in the set makes the whole set a miss
    // and the score re-renders. That matters because the registry evicts under a byte budget and a
    // partially-evicted set would otherwise be reported as a complete one (§1).
    //
    // File-local rather than in FPluginState: nothing outside this verb has any use for it, and the
    // entry is ~200 bytes, so the FIFO cap below exists only so a long session cannot grow it
    // without bound.
    // ---------------------------------------------------------------------------------------
    constexpr int32 MaxMemoizedScores = 64;

    struct FStemRenderMemo
    {
        FCriticalSection Mutex;
        TMap<FString, TArray<FString>> DigestToCandidateIds;
        TArray<FString> InsertionOrder;
    };

    FStemRenderMemo& StemRenderMemo()
    {
        static FStemRenderMemo Memo;
        return Memo;
    }

    /** Remembered ids for a digest, or an empty array. */
    TArray<FString> RecallStemCandidateIds(const FString& Digest)
    {
        FStemRenderMemo& Memo = StemRenderMemo();
        FScopeLock Lock(&Memo.Mutex);
        if (const TArray<FString>* Found = Memo.DigestToCandidateIds.Find(Digest))
        {
            return *Found;
        }
        return TArray<FString>();
    }

    void RememberStemCandidateIds(const FString& Digest, const TArray<FString>& CandidateIds)
    {
        FStemRenderMemo& Memo = StemRenderMemo();
        FScopeLock Lock(&Memo.Mutex);
        if (!Memo.DigestToCandidateIds.Contains(Digest))
        {
            Memo.InsertionOrder.Add(Digest);
        }
        Memo.DigestToCandidateIds.Add(Digest, CandidateIds);
        while (Memo.InsertionOrder.Num() > MaxMemoizedScores)
        {
            Memo.DigestToCandidateIds.Remove(Memo.InsertionOrder[0]);
            Memo.InsertionOrder.RemoveAt(0);
        }
    }

    /**
     * The three registry numbers a render verb owes its caller, under FPwCandidateRegistry's own
     * field spellings.
     *
     * A projection of UsageToJson rather than the whole block: rendering 16 stems is what causes
     * eviction, so the caller needs to see pressure coming, but the full nine-field block costs
     * ~300 characters of a response already carrying a per-stem table. The caps and the session
     * totals are one audio.synth.list_candidates away and do not change between calls.
     */
    void AddRegistryPressure(const TSharedPtr<FJsonObject>& Result, const FPwCandidateUsage& Usage)
    {
        TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
        Block->SetNumberField(TEXT("count"), Usage.NumCandidates);
        Block->SetNumberField(TEXT("approxBytes"), static_cast<double>(Usage.ApproxBytes));
        Block->SetNumberField(TEXT("bytesRemaining"), static_cast<double>(Usage.BytesRemaining));
        if (Usage.bOverBudget)
        {
            Block->SetBoolField(TEXT("overBudget"), true);
        }
        Result->SetObjectField(TEXT("registry"), Block);
    }

    /** One compact table row per stem. The table is why this verb is one call and not sixteen. */
    TSharedPtr<FJsonObject> StemRowToJson(const FPwStemResult& Stem, const FString& CandidateId)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), Stem.TrackName);
        Row->SetStringField(TEXT("id"), CandidateId);
        // Both counts, always. A dropped note looks identical to a quiet one in the audio and
        // identical to a rendered one in a success-only report, and `dropped` absent-means-zero
        // would put the burden of knowing that on the reader (§1).
        Row->SetNumberField(TEXT("notes"), Stem.NotesRendered);
        Row->SetNumberField(TEXT("dropped"), Stem.NotesDropped);
        SetRounded(Row, TEXT("peakDb"), Stem.PeakDb, 2);
        if (!Stem.bMeasured)
        {
            // Only when false: an unmeasured stem is the exception the reader must act on, and the
            // measuredStems counter beside the table carries the positive half.
            Row->SetBoolField(TEXT("measured"), false);
        }
        return Row;
    }
}

REGISTER_RPC_HANDLER("audio.music.render_stems", "audio.music",
    "Render a music score to one seamless PCM stem per track, each landing in the session candidate "
    "registry with its own id (so audio.synth.audition and audio.music.export_stems take it "
    "directly). Returns the loop report - exact, the signed residualFrames and the nearestExactBpm - "
    "beside a compact per-stem table of notes rendered, notes dropped and measured peak. An inexact "
    "loop is REPORTED, never rounded away: it is your decision whether to accept sub-frame drift or "
    "move the tempo. Returns a job ticket immediately and renders afterwards, so poll "
    "system.job_status with the ticket_id; the job registers no cancel callback, so system.job_cancel "
    "reports JOB_CANCEL_UNSUPPORTED rather than claiming a stop it cannot deliver. Deterministic and "
    "idempotent: the same score re-rendered reuses the candidates it already produced (reused:true) "
    "instead of filling the registry with copies. Call audio.music.describe_schema for the grammar.",
    RPC_PARAMS(
        RPC_PARAM_REQ("score", "object",
            "The score document. audio.music.describe_schema publishes the grammar and a worked "
            "example; a malformed score is rejected with the parser's own field path "
            "(tracks[2].notes[17].degree), never partially rendered."),
        RPC_PARAM_DEF("mixdown", "boolean",
            "Also register one extra candidate holding the unity-gain sum of every stem, for "
            "auditioning the whole bed in one call. Off by default because it costs another full "
            "buffer of registry budget.", "false")
    ))
{
    using namespace PwMusicHandlerInternal;

    TSharedPtr<FJsonObject> ScoreJson;
    if (!Ctx.RequireObject(TEXT("score"), ScoreJson)) return true;

    // Parsed HERE, before the ticket: the parser's code and field path are the whole value of the
    // error, and a job can only carry a bare string (§9).
    FPwMusicScore Score;
    FPwMusicScoreError ParseError;
    if (!ParseMusicScore(ScoreJson, Score, ParseError))
    {
        TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
        if (!ParseError.Field.IsEmpty())
        {
            Failure->SetStringField(TEXT("field"), ParseError.Field);
        }
        Failure->SetStringField(TEXT("remedy"),
            TEXT("Patch the named field and resubmit. audio.music.describe_schema publishes the "
                 "vocabulary each closed set accepts."));
        Ctx.SendError(ParseError.Code.IsEmpty() ? FString(ErrorCodes::ERR_INVALID_RECIPE) : ParseError.Code,
            ParseError.ToString(), Failure);
        return true;
    }

    // Defensive rather than redundant: the parser rejects a trackless score, but a zero-stem
    // success is exactly the shape a typo would take, so the verb refuses to render nothing even if
    // a future schema revision allowed it (§3).
    if (Score.Tracks.Num() == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_RECIPE,
            TEXT("score.tracks is empty, so there is nothing to render. A score with no tracks is "
                 "an error rather than a zero-stem success: one track renders one stem."));
        return true;
    }

    const bool bMixdown = Ctx.GetBool(TEXT("mixdown"), false);
    // The mixdown flag is part of the identity, not just of the response: a memo entry recorded
    // without it holds one candidate fewer, and keying both shapes on one digest would make the two
    // call shapes evict each other's memo on every alternation.
    const FString Digest = ScoreDigest(Score) + (bMixdown ? TEXT("|mix") : TEXT(""));

    TSharedPtr<FJsonObject> Started = MakeShared<FJsonObject>();
    Started->SetNumberField(TEXT("tracks"), Score.Tracks.Num());
    Started->SetNumberField(TEXT("bars"), Score.Bars);
    // Structurally true, not an assertion: nothing below calls FJobRegistry::SetCancelCallback,
    // which is the only thing Cancel() reads to tell a cancellable verb from an uncancellable one.
    Started->SetBoolField(TEXT("cancellable"), false);
    Started->SetStringField(TEXT("message"),
        TEXT("Rendering stems. This job registers no cancel callback - a render cannot be stopped "
             "mid-flight - so system.job_cancel will report JOB_CANCEL_UNSUPPORTED rather than a "
             "cancellation that did not happen. Poll system.job_status with this ticket_id for the "
             "loop report and the per-stem table."));

    FJobBindArgs Args;
    Args.Method = TEXT("audio.music.render_stems");
    Args.StartedPayload = Started;
    Args.BindNativeDelegate = [Score, Digest, bMixdown](FJobOnComplete OnComplete)
    {
        FPwCandidateRegistry& Registry = FPluginState::Get().GetCandidateRegistry();

        // ---------------------------------------------------------------------------------
        // Idempotence: reuse the previous set only when EVERY id in it is still resident.
        // ---------------------------------------------------------------------------------
        const int32 ExpectedCandidates = Score.Tracks.Num() + (bMixdown ? 1 : 0);
        const TArray<FString> Remembered = RecallStemCandidateIds(Digest);
        if (Remembered.Num() == ExpectedCandidates && Remembered.Num() > 0)
        {
            bool bAllResident = true;
            for (const FString& CandidateId : Remembered)
            {
                const FPwCandidateLookupResult Lookup = Registry.Get(CandidateId);
                if (!Lookup.IsHit())
                {
                    bAllResident = false;
                    break;
                }
            }

            if (bAllResident)
            {
                // The loop report is recomputed from the score rather than remembered: it is a pure
                // function of bpm, bars, time signature and sample rate, so recomputing it cannot
                // disagree with the stems, and remembering it would be a second copy to drift.
                const FPwScoreLoop Loop = PwScoreLoopLength(Score);

                TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
                Result->SetBoolField(TEXT("reused"), true);
                Result->SetStringField(TEXT("reuseNote"),
                    TEXT("An identical score was already rendered this session and every candidate "
                         "it produced is still resident (each id re-resolved through the registry, "
                         "not assumed). A render is deterministic in the score, so these hold "
                         "exactly the samples this call would have produced."));
                Result->SetObjectField(TEXT("loop"), LoopToJson(Loop));
                Result->SetNumberField(TEXT("sampleRate"), Score.SampleRate);
                Result->SetNumberField(TEXT("bars"), Score.Bars);
                SetRounded(Result, TEXT("bpm"), Score.Bpm, 6);

                TArray<TSharedPtr<FJsonValue>> Rows;
                for (int32 Index = 0; Index < Score.Tracks.Num(); ++Index)
                {
                    TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
                    Row->SetStringField(TEXT("name"), Score.Tracks[Index].Name);
                    Row->SetStringField(TEXT("id"), Remembered[Index]);
                    Rows.Add(MakeShared<FJsonValueObject>(Row));
                }
                Result->SetArrayField(TEXT("stems"), Rows);
                // The per-stem counts belong to the render that produced them, and this call ran no
                // render. Omitted rather than echoed from a memo, because a number nobody measured
                // on this call is not a measurement (§4); the original response carried them.
                Result->SetStringField(TEXT("countsNote"),
                    TEXT("Per-stem note counts and peaks are omitted on a reuse: this call measured "
                         "nothing. They were reported by the render that created these candidates."));
                if (bMixdown)
                {
                    Result->SetStringField(TEXT("mixdownCandidateId"), Remembered.Last());
                }
                AddRegistryPressure(Result, Registry.Usage());
                OnComplete(true, Result, FString());
                return;
            }
        }

        // ---------------------------------------------------------------------------------
        // Render.
        // ---------------------------------------------------------------------------------
        FPwMusicRenderReport Report;
        FString ErrorCode;
        FString Error;
        if (!PwRenderScoreStems(Score, Report, ErrorCode, Error))
        {
            // A failed job carries only an error STRING on the wire, so the code travels in the
            // result payload, which system.job_status publishes beside it.
            TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
            Failure->SetStringField(TEXT("errorCode"),
                ErrorCode.IsEmpty() ? FString(ErrorCodes::ERR_INVALID_RECIPE) : ErrorCode);
            // Nothing partial is published: PwRenderScoreStems leaves its report unmeasured and
            // empty of stems on every failure path, and this branch registers no candidates.
            Failure->SetBoolField(TEXT("measured"), Report.bMeasured);
            OnComplete(false, Failure, Error);
            return;
        }

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("reused"), false);
        // Straight from PwScoreLoopLength via the render report - never re-derived here, because two
        // implementations of one number drift and only one of them would be the one that was
        // rendered (§2).
        Result->SetObjectField(TEXT("loop"), LoopToJson(Report.Loop));
        Result->SetNumberField(TEXT("sampleRate"), Score.SampleRate);
        Result->SetNumberField(TEXT("bars"), Score.Bars);
        SetRounded(Result, TEXT("bpm"), Score.Bpm, 6);
        Result->SetNumberField(TEXT("totalNotes"), Report.TotalNotes);

        TArray<FString> CandidateIds;
        TArray<TSharedPtr<FJsonValue>> Rows;
        int32 MeasuredStems = 0;
        int32 TotalDropped = 0;
        for (FPwStemResult& Stem : Report.Stems)
        {
            FPwCandidate Candidate;
            Candidate.Buffer = MoveTemp(Stem.Buffer);
            const FString CandidateId = Registry.Add(MoveTemp(Candidate));
            CandidateIds.Add(CandidateId);

            Rows.Add(MakeShared<FJsonValueObject>(StemRowToJson(Stem, CandidateId)));
            MeasuredStems += Stem.bMeasured ? 1 : 0;
            TotalDropped += Stem.NotesDropped;
        }
        Result->SetArrayField(TEXT("stems"), Rows);
        // Paired with the per-row `measured:false`, so "we looked at every stem" and "a stem was
        // never looked at" cannot score alike (§5b).
        Result->SetNumberField(TEXT("measuredStems"), MeasuredStems);
        Result->SetNumberField(TEXT("stemCount"), Report.Stems.Num());
        Result->SetNumberField(TEXT("notesDropped"), TotalDropped);

        if (bMixdown)
        {
            // Note the ordering: PwMixStems is fed the report's stems, whose buffers were MOVED
            // into the candidates above, so the mixdown is summed from the registry's copies rather
            // than from emptied buffers.
            TArray<FPwStemResult> ForMix;
            ForMix.Reserve(CandidateIds.Num());
            for (int32 Index = 0; Index < CandidateIds.Num(); ++Index)
            {
                const FPwCandidateLookupResult Lookup = Registry.Get(CandidateIds[Index]);
                if (!Lookup.IsHit())
                {
                    continue;
                }
                FPwStemResult Copy;
                Copy.TrackName = Report.Stems[Index].TrackName;
                Copy.Buffer = Lookup.Candidate->Buffer;
                ForMix.Add(MoveTemp(Copy));
            }

            FPwAudioBuffer Mixed;
            FString MixCode;
            FString MixError;
            if (ForMix.Num() == CandidateIds.Num() && PwMixStems(ForMix, Mixed, MixCode, MixError))
            {
                FPwCandidate MixCandidate;
                MixCandidate.Buffer = MoveTemp(Mixed);
                const FString MixId = Registry.Add(MoveTemp(MixCandidate));
                CandidateIds.Add(MixId);
                Result->SetStringField(TEXT("mixdownCandidateId"), MixId);
            }
            else
            {
                // Reported, not swallowed: the stems are still good and the caller asked for one
                // more thing that did not happen. A silent omission would read as "no mixdown was
                // requested".
                TArray<TSharedPtr<FJsonValue>> Warnings;
                Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
                    TEXT("mixdown skipped (%s): %s. The stems themselves are unaffected."),
                    MixCode.IsEmpty() ? TEXT("CANDIDATE_EVICTED") : *MixCode,
                    MixError.IsEmpty() ? TEXT("a stem was evicted before the mixdown ran") : *MixError)));
                Result->SetArrayField(TEXT("warnings"), Warnings);
            }
        }

        // Recorded only after every candidate landed, so a retry can never recall a half-registered
        // set. A memo entry whose ids no longer resolve is treated as a miss, which is why storing
        // it is safe even if the registry evicts one a moment later.
        RememberStemCandidateIds(Digest, CandidateIds);
        AddRegistryPressure(Result, Registry.Usage());

        OnComplete(true, Result, FString());
    };

    Ctx.StartJob(Args);
    return true;
}

// =================================================================================================
// audio.music.export_stems
//
// Stem candidates -> one USoundWave asset per track. There is no file export anywhere in this
// subsystem: audio leaves as an asset path plus frames, rate and channels.
//
// A JOB, for the same reason render_stems is: up to 16 waves are written and every one of them is
// DECODED BACK for verification, which is a blocking bulk-data read each time. Every candidate is
// resolved and every name validated before the ticket, so a bad id keeps the registry's own
// distinct code (CANDIDATE_EVICTED / NO_CANDIDATES / CANDIDATE_NOT_FOUND) instead of collapsing
// into a job's error string (§7, §9).
// =================================================================================================

namespace PwMusicHandlerInternal
{
    /**
     * Absolute tolerance for the decode-back RMS/peak comparison, in normalized float.
     *
     * The same figure audio.synth.export uses for the same round trip, and derived rather than
     * guessed: the encode is Audio::ArrayFloatToPcm16 (multiply by 32767 and truncate) and the
     * decode divides by 32768, so a correct round trip stays inside ~6.2e-5 per sample. Both RMS and
     * peak are 1-Lipschitz in the per-sample error, so one bound covers both signatures. 1e-3 is
     * ~16x that - loose enough to survive a change of scale convention on either side, and still
     * orders of magnitude away from a silent asset, a half-length asset or a dropped channel.
     */
    constexpr double ExportToleranceAbs = 1.0e-3;

    struct FChannelSignature
    {
        double Rms = 0.0;
        double Peak = 0.0;
    };

    FChannelSignature MeasureChannel(const TArray<float>& Samples)
    {
        FChannelSignature Out;
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

    /** Largest of the four RMS/peak deltas across both channels - the number a failing row reports. */
    double WorstSignatureDelta(const FPwAudioBuffer& Source, const FPwAudioBuffer& Decoded)
    {
        const FChannelSignature SourceLeft   = MeasureChannel(Source.Left);
        const FChannelSignature SourceRight  = MeasureChannel(Source.Right);
        const FChannelSignature DecodedLeft  = MeasureChannel(Decoded.Left);
        const FChannelSignature DecodedRight = MeasureChannel(Decoded.Right);

        double Worst = FMath::Abs(SourceLeft.Rms - DecodedLeft.Rms);
        Worst = FMath::Max(Worst, FMath::Abs(SourceLeft.Peak - DecodedLeft.Peak));
        Worst = FMath::Max(Worst, FMath::Abs(SourceRight.Rms - DecodedRight.Rms));
        Worst = FMath::Max(Worst, FMath::Abs(SourceRight.Peak - DecodedRight.Peak));
        return Worst;
    }

    /** One stem the caller asked to export, fully resolved before the ticket is issued. */
    struct FResolvedStemExport
    {
        FString TrackName;
        FString CandidateId;
        FString AssetName;
        // Shared handle, not a copy: it keeps the candidate alive for the whole job even if another
        // thread evicts it, and the buffer is megabytes.
        TSharedPtr<const FPwCandidate> Candidate;
    };
}

REGISTER_RPC_HANDLER("audio.music.export_stems", "audio.music",
    "Write each stem candidate into the project as its own USoundWave asset, named from its track "
    "name, then verify every one by decoding the created asset back through "
    "USoundWave::GetImportedSoundWaveData and comparing frames, sample rate, channel count and an "
    "RMS/peak signature against the candidate's buffer - a different subsystem from the one that "
    "wrote it, so nothing in the verification is a readback of a field the writer just set. There is "
    "no file export anywhere in this subsystem. Idempotent: re-exporting the same stems to the same "
    "folder rewrites those waves' audio in place rather than creating _1 siblings, keeping every "
    "property that is not the payload (SoundClass, attenuation, concurrency, submix and bus sends, "
    "modulation, loading behaviour, compression type, looping, volume) and refreshing only what the "
    "old audio determined - duration, format, cue points, channel layout, timecode. A rewrite that "
    "moves anything else fails that stem's row by name. Returns a job ticket immediately; the job registers no cancel "
    "callback, so system.job_cancel reports JOB_CANCEL_UNSUPPORTED rather than claiming a stop it "
    "cannot deliver. Keep the waves PCM if they must loop seamlessly - block-based compression does "
    "not preserve a sample-exact boundary.",
    RPC_PARAMS(
        RPC_PARAM_REQ("stems", "array",
            "One entry per stem: [{\"candidateId\":\"c3_a1b2\",\"name\":\"pad\"}]. Both fields are "
            "required - the id says which audio and the name says which asset, and neither has a "
            "safe default. audio.music.render_stems returns exactly this pairing."),
        RPC_PARAM_REQ("path", "path",
            "Content-browser folder for the assets; must be under /Game, e.g. /Game/Audio/Music."),
        RPC_PARAM_OPT("prefix", "string",
            "Prepended to every track name to form the asset name, e.g. 'SW_Forest_' turns track "
            "'pad' into 'SW_Forest_pad'. The combined name is validated, not sanitized: a name the "
            "sanitizer would rewrite is rejected rather than silently landing somewhere else."),
        RPC_PARAM_DEF("save", "boolean",
            "Write the .uasset files to disk. false marks the packages dirty only, and the response "
            "reports saved:false / pendingFlush:true.", "true"),
        RPC_PARAM_DEF("overwrite", "boolean",
            "Delete and recreate an existing SoundWave instead of rewriting it in place. Rejected "
            "with ASSET_IN_USE when other packages reference it.", "false")
    ))
{
    using namespace PwMusicHandlerInternal;

    const TArray<TSharedPtr<FJsonValue>>* StemsArray = nullptr;
    if (!Ctx.RequireArray(TEXT("stems"), StemsArray) || StemsArray == nullptr) return true;

    // Zero is not a small number: an empty export reported as a success looks identical to an
    // export whose every stem failed, and to a caller that built the array from a bad filter (§7).
    if (StemsArray->Num() == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("'stems' is empty, so nothing would be exported. An empty list is an error rather "
                 "than a zero-asset success. audio.music.render_stems returns the candidateId/name "
                 "pairs this argument takes."));
        return true;
    }
    if (StemsArray->Num() > PwMusicLimits::MaxTracks)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("'stems' carries %d entries; a score renders at most %d tracks, so "
                                 "an oversized list is a caller error rather than a larger export."),
                StemsArray->Num(), PwMusicLimits::MaxTracks));
        return true;
    }

    FString FolderPath;
    if (!Ctx.RequireString(TEXT("path"), FolderPath)) return true;
    const FString Prefix = Ctx.GetString(TEXT("prefix"));
    const bool bSave = Ctx.GetBool(TEXT("save"), true);
    const bool bOverwrite = Ctx.GetBool(TEXT("overwrite"), false);

    FPwCandidateRegistry& Registry = FPluginState::Get().GetCandidateRegistry();

    // ---------------------------------------------------------------------------------------
    // Resolve everything BEFORE the ticket (§9). A registry miss keeps the registry's own code.
    // ---------------------------------------------------------------------------------------
    TArray<FResolvedStemExport> Resolved;
    TSet<FString> SeenNames;
    FString ValidatedFolder;

    for (int32 Index = 0; Index < StemsArray->Num(); ++Index)
    {
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        if (!(*StemsArray)[Index].IsValid() || !(*StemsArray)[Index]->TryGetObject(Entry) || !Entry)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("stems[%d] is not an object. Each entry is "
                                     "{\"candidateId\":\"...\",\"name\":\"...\"}."), Index));
            return true;
        }

        FResolvedStemExport Stem;
        if (!(*Entry)->TryGetStringField(TEXT("candidateId"), Stem.CandidateId))
        {
            (*Entry)->TryGetStringField(TEXT("candidate_id"), Stem.CandidateId);
        }
        (*Entry)->TryGetStringField(TEXT("name"), Stem.TrackName);

        if (Stem.CandidateId.IsEmpty() || Stem.TrackName.IsEmpty())
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("stems[%d] needs both 'candidateId' and 'name'. Neither has a "
                                     "safe default: the id says which audio and the name says which "
                                     "asset."), Index));
            return true;
        }

        Stem.AssetName = Prefix + Stem.TrackName;

        // Rejected rather than rewritten, so an asset can never land under a different name than
        // the caller asked for (§1).
        const FString Sanitized = SanitizeAssetName(Stem.AssetName);
        if (Sanitized.IsEmpty() || Sanitized != Stem.AssetName)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PATH,
                FString::Printf(TEXT("stems[%d]: '%s' contains characters that cannot be used in an "
                                     "asset name. A valid form would be '%s'."),
                    Index, *Stem.AssetName, *Sanitized));
            return true;
        }

        // Case-insensitive, because two stems whose names differ only in case are one file on
        // Windows and the second would silently overwrite the first - the same rule the score
        // parser applies to track names.
        const FString NameKey = Stem.AssetName.ToLower();
        if (SeenNames.Contains(NameKey))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("stems[%d]: asset name '%s' is already used by an earlier "
                                     "entry (compared case-insensitively). Two stems sharing one "
                                     "name would be one asset, and the second would overwrite the "
                                     "first."), Index, *Stem.AssetName));
            return true;
        }
        SeenNames.Add(NameKey);

        FString ValidatedPath;
        FString PathError;
        if (!ValidateAssetCreationPath(FolderPath, Stem.AssetName, ValidatedPath, PathError))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PATH, PathError);
            return true;
        }
        const FString Folder = FPackageName::GetLongPackagePath(ValidatedPath);
        if (!Folder.Equals(TEXT("/Game")) && !Folder.StartsWith(TEXT("/Game/")))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PATH,
                FString::Printf(TEXT("SoundWave assets can only be created under /Game; '%s' "
                                     "resolves to '%s'. The engine writer hardcodes a /Game/ prefix, "
                                     "so any other mount root would land the asset elsewhere."),
                    *FolderPath, *Folder));
            return true;
        }
        ValidatedFolder = Folder;

        const FPwCandidateLookupResult Lookup = Registry.Get(Stem.CandidateId);
        if (!Lookup.IsHit())
        {
            // The registry's own code and its own recovery payload: CANDIDATE_EVICTED (re-render),
            // NO_CANDIDATES (nothing rendered yet) and CANDIDATE_NOT_FOUND (wrong id) are three
            // different next moves and flattening them is what makes an agent "fix" a good id.
            Ctx.SendError(FPwCandidateRegistry::MissErrorCode(Lookup.Status),
                FString::Printf(TEXT("stems[%d]: %s"), Index,
                    *FPwCandidateRegistry::MakeMissMessage(Stem.CandidateId, Lookup)),
                Registry.BuildMissPayload(Stem.CandidateId, Lookup));
            return true;
        }

        const FPwAudioBuffer& Buffer = Lookup.Candidate->Buffer;
        if (Buffer.NumFrames() <= 0 || !Buffer.IsValid())
        {
            Ctx.SendError(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
                FString::Printf(TEXT("stems[%d]: candidate %s holds no audio (frames=%d, rate=%d Hz), "
                                     "so there is nothing to write."),
                    Index, *Stem.CandidateId, Buffer.NumFrames(), Buffer.SampleRate));
            return true;
        }

        Stem.Candidate = Lookup.Candidate;
        Resolved.Add(MoveTemp(Stem));
    }

    TSharedPtr<FJsonObject> Started = MakeShared<FJsonObject>();
    Started->SetNumberField(TEXT("stems"), Resolved.Num());
    Started->SetStringField(TEXT("path"), ValidatedFolder);
    // Structurally true: nothing below calls FJobRegistry::SetCancelCallback.
    Started->SetBoolField(TEXT("cancellable"), false);
    Started->SetStringField(TEXT("message"),
        TEXT("Writing stem assets. This job registers no cancel callback - a package write cannot be "
             "stopped mid-flight - so system.job_cancel will report JOB_CANCEL_UNSUPPORTED rather "
             "than a cancellation that did not happen. Poll system.job_status with this ticket_id "
             "for the per-asset table."));

    FJobBindArgs Args;
    Args.Method = TEXT("audio.music.export_stems");
    Args.StartedPayload = Started;
    Args.BindNativeDelegate =
        [Resolved, ValidatedFolder, bSave, bOverwrite](FJobOnComplete OnComplete)
    {
        FPwCandidateRegistry& Registry = FPluginState::Get().GetCandidateRegistry();

        TArray<TSharedPtr<FJsonValue>> Rows;
        int32 NumExported = 0;
        int32 NumFailed = 0;
        int32 NumSavedToDisk = 0;
        int32 NumUpdatedInPlace = 0;

        for (const FResolvedStemExport& Stem : Resolved)
        {
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("name"), Stem.TrackName);
            Row->SetStringField(TEXT("asset"), Stem.AssetName);

            const auto FailRow = [&Row, &Rows, &NumFailed](const TCHAR* Code, const FString& Message)
            {
                Row->SetBoolField(TEXT("verified"), false);
                Row->SetStringField(TEXT("errorCode"), Code);
                Row->SetStringField(TEXT("error"), Message.Left(160));
                Rows.Add(MakeShared<FJsonValueObject>(Row));
                ++NumFailed;
            };

            // Mandatory before any create path: IAssetTools/CreatePackage on an occupied path can
            // reach a modal overwrite prompt that wedges the game thread for every client.
            // bRequireExactClass because a USoundWaveProcedural / USoundSourceBus at the path passes
            // IsA(USoundWave) but is a different object layout - reconstructing one as a plain
            // USoundWave is corruption.
            const FString FullPath = ValidatedFolder / Stem.AssetName;
            const AssetCreatePolicy::FResolution Resolution = AssetCreatePolicy::Resolve(
                FullPath, Stem.AssetName, USoundWave::StaticClass(), bOverwrite,
                /*bRequireExactClass=*/true);
            if (Resolution.IsRejected())
            {
                FailRow(*Resolution.ErrorCode, Resolution.ErrorMessage);
                continue;
            }
            if (Resolution.Action == AssetCreatePolicy::EAction::UpdateInPlace)
            {
                ++NumUpdatedInPlace;
            }

            const FPwAudioBuffer& Source = Stem.Candidate->Buffer;

            FString ExportError;
            FPwSoundWaveWriteReport WriteReport;
            USoundWave* Wave = PwCreateSoundWaveAsset(
                Source, ValidatedFolder, Stem.AssetName, bSave, WriteReport, ExportError);
            if (!Wave)
            {
                FailRow(ErrorCodes::ERR_CREATION_FAILED, ExportError);
                continue;
            }
            NumSavedToDisk += WriteReport.bSavedToDisk ? 1 : 0;

            const FString AssetPath = Wave->GetPathName();
            Row->SetStringField(TEXT("assetPath"), AssetPath);

            // A rewrite that moved a property the payload does not own is a failed row, not a
            // successful one with a note: an unrouted stem escapes every SoundMix and plays at
            // full level at any distance (board B-synth-export-wipes-soundclass-attenuation).
            // Reported by name only when it happens - this verb writes one row per stem and the
            // response budget cannot carry a per-row preservation block that is always true.
            if (WriteReport.ChangedProperties.Num() > 0)
            {
                FailRow(ErrorCodes::ERR_VERIFICATION_FAILED, FString::Printf(
                    TEXT("'%s' was rewritten in place but the rewrite changed %d propert%s the "
                         "payload does not own: %s."),
                    *AssetPath, WriteReport.ChangedProperties.Num(),
                    WriteReport.ChangedProperties.Num() == 1 ? TEXT("y") : TEXT("ies"),
                    *FString::JoinBy(WriteReport.ChangedProperties, TEXT(", "),
                        [](const FName& PropertyName) { return PropertyName.ToString(); })));
                continue;
            }

            // -------------------------------------------------------------------------------
            // Verification (§4). PwCreateSoundWaveAsset checks the writer's state machine and the
            // package it landed in, never the samples, so the round trip is done here through
            // USoundWave::GetImportedSoundWaveData - which parses the RIFF payload independently of
            // the FSoundWavePCMWriter path that wrote it. Nothing below reads a field the writer
            // assigned.
            // -------------------------------------------------------------------------------
            FPwAudioBuffer Decoded;
            FString DecodeCode;
            FString DecodeError;
            if (!PwDecodeSoundWaveWithCode(Wave, Decoded, DecodeCode, DecodeError))
            {
                FailRow(DecodeCode.IsEmpty() ? ErrorCodes::ERR_VERIFICATION_FAILED : *DecodeCode,
                    FString::Printf(TEXT("written but not decodable, so nothing about its contents "
                                         "is verified: %s"), *DecodeError));
                continue;
            }

            // The channel count is the one thing a deinterleaved buffer cannot carry, so it is read
            // straight off the payload header - still the parse side, never the write side.
            TArray<uint8> PayloadPcm;
            uint32 PayloadSampleRate = 0;
            uint16 PayloadChannels = 0;
            const bool bReadPayloadHeader =
                Wave->GetImportedSoundWaveData(PayloadPcm, PayloadSampleRate, PayloadChannels);

            const bool bFramesMatch = Decoded.NumFrames() == Source.NumFrames();
            const bool bRateMatch = Decoded.SampleRate == Source.SampleRate;
            const bool bChannelsMatch = bReadPayloadHeader &&
                static_cast<int32>(PayloadChannels) == PwExportChannels;
            const double WorstDelta = WorstSignatureDelta(Source, Decoded);
            const bool bVerified = bFramesMatch && bRateMatch && bChannelsMatch &&
                WorstDelta <= ExportToleranceAbs;

            Row->SetNumberField(TEXT("frames"), Decoded.NumFrames());
            Row->SetBoolField(TEXT("verified"), bVerified);
            if (!bVerified)
            {
                // The row stays compact on the success path and says by how much on the failure
                // path, which is the only place the numbers are worth the characters.
                Row->SetStringField(TEXT("errorCode"), ErrorCodes::ERR_VERIFICATION_FAILED);
                Row->SetStringField(TEXT("error"), FString::Printf(
                    TEXT("frames %d vs %d, %d Hz vs %d Hz, %d channels, worst RMS/peak delta %.6f"),
                    Source.NumFrames(), Decoded.NumFrames(), Source.SampleRate, Decoded.SampleRate,
                    bReadPayloadHeader ? static_cast<int32>(PayloadChannels) : 0, WorstDelta));
                Rows.Add(MakeShared<FJsonValueObject>(Row));
                ++NumFailed;
                continue;
            }

            // Recorded on the candidate only after the round trip passed, so list_candidates cannot
            // show an export that did not verify.
            Registry.SetExportedAssetPath(Stem.CandidateId, AssetPath);
            Rows.Add(MakeShared<FJsonValueObject>(Row));
            ++NumExported;
        }

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("path"), ValidatedFolder);
        Result->SetNumberField(TEXT("requested"), Resolved.Num());
        // Three counters rather than one flag: "16 exported" and "16 attempted, 16 rejected" are
        // different outcomes and only the counters tell them apart.
        Result->SetNumberField(TEXT("exported"), NumExported);
        Result->SetNumberField(TEXT("failed"), NumFailed);
        Result->SetNumberField(TEXT("updatedInPlace"), NumUpdatedInPlace);
        Result->SetArrayField(TEXT("stems"), Rows);

        // The same {saveRequested, saved, pendingFlush} triple both save families emit, aggregated:
        // `saved` is the MEASURED count of .uasset writes, not an echo of the save flag (§5).
        TSharedPtr<FJsonObject> Save = MakeShared<FJsonObject>();
        Save->SetBoolField(TEXT("saveRequested"), bSave);
        Save->SetNumberField(TEXT("saved"), NumSavedToDisk);
        Save->SetNumberField(TEXT("pendingFlush"), NumExported - NumSavedToDisk);
        Result->SetObjectField(TEXT("save"), Save);

        TSharedPtr<FJsonObject> Verification = MakeShared<FJsonObject>();
        Verification->SetStringField(TEXT("method"),
            TEXT("each asset decoded back through USoundWave::GetImportedSoundWaveData "
                 "(PwDecodeSoundWave), which parses the RIFF payload independently of the "
                 "FSoundWavePCMWriter path that wrote it"));
        Verification->SetNumberField(TEXT("toleranceAbs"), ExportToleranceAbs);
        Verification->SetNumberField(TEXT("verified"), NumExported);
        Result->SetObjectField(TEXT("verification"), Verification);

        Result->SetStringField(TEXT("loopNote"),
            TEXT("These waves are seamless because their SAMPLES are, not because of loop metadata: "
                 "nothing at runtime reads a SoundWave's cue points or loop regions. Keep them PCM, "
                 "or play them through a MetaSound Wave Player with its Loop pins set - "
                 "audio.music.build_interactive does exactly that."));

        if (NumFailed > 0)
        {
            Result->SetStringField(TEXT("errorCode"), ErrorCodes::ERR_VERIFICATION_FAILED);
            OnComplete(false, Result, FString::Printf(
                TEXT("%d of %d stem(s) failed to write or verify; see stems[].errorCode for the "
                     "per-stem reason. The %d that succeeded exist and are verified."),
                NumFailed, Resolved.Num(), NumExported));
            return;
        }
        OnComplete(true, Result, FString());
    };

    Ctx.StartJob(Args);
    return true;
}

// =================================================================================================
// audio.music.build_interactive
//
// Stem ASSETS -> one interactive MetaSound Source. This is the stage that makes the stems playable
// as a bed: the MetaSound Wave Player is the only runtime in the engine that honours loop points,
// so a layered, crossfading music system has to be a graph.
//
// NOT a job: one document is built. It IS tick-unsafe (asset creation + package save), so it is in
// the Dispatch/SafePoint.cpp table and runs later on a safe stack with its original context.
//
// AUTHORING STOPS AT THE ASSET. The graph exposes Intensity / Section as ordinary MetaSound inputs;
// scheduling them at a musical boundary is the game's job, through Quartz (core engine, no plugin):
// UQuartzSubsystem::CreateNewClock, SubscribeToQuantizationEvent and UAudioComponent::PlayQuantized.
// =================================================================================================

namespace PwMusicHandlerInternal
{
    /**
     * Finds the object a build just created, accepting either the package form ("/Game/A/B") or the
     * object form ("/Game/A/B.B"). Returns nullptr rather than loading: the builder created it a
     * moment ago, so anything not in memory is a fact worth reporting, not one to paper over.
     */
    UObject* FindCreatedAsset(const FString& AssetPath)
    {
        if (AssetPath.IsEmpty())
        {
            return nullptr;
        }
        if (UObject* Direct = StaticFindObject(UObject::StaticClass(), nullptr, *AssetPath))
        {
            return Direct;
        }
        if (!AssetPath.Contains(TEXT(".")))
        {
            const FString ObjectForm = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
            return StaticFindObject(UObject::StaticClass(), nullptr, *ObjectForm);
        }
        return nullptr;
    }
}

REGISTER_RPC_HANDLER("audio.music.build_interactive", "audio.music",
    "Build one interactive MetaSound Source that plays the given stem assets as loopable, "
    "crossfadeable layers - the runtime a music bed actually needs, because bare SoundWave loop "
    "metadata is inert and only the MetaSound Wave Player's Loop / Loop Start / Loop Duration pins "
    "honour loop points. Reports the MEASURED nodesAdded, connectionsMade and the graph inputs it "
    "created, plus any warnings the build produced. Every stem path is resolved before anything is "
    "created, so an unresolvable path leaves no asset behind. Authoring stops here: the graph "
    "exposes Intensity / Section as ordinary MetaSound parameters and the game schedules them at bar "
    "boundaries through Quartz (UQuartzSubsystem::CreateNewClock + UAudioComponent::PlayQuantized), "
    "which is core engine and needs no plugin.",
    RPC_PARAMS(
        RPC_PARAM_REQ("path", "path",
            "Content-browser folder for the MetaSound Source; must be under /Game."),
        RPC_PARAM_REQ("name", "string", "Asset name without extension, e.g. 'MS_ForestBed'."),
        RPC_PARAM_REQ("stems", "array",
            "One entry per layer, at most 8: [{\"assetPath\":\"/Game/Audio/Music/SW_pad\","
            "\"inputName\":\"Pad\",\"intensityThreshold\":0.3}]. Only assetPath is required. "
            "inputName defaults to 'Stem<index>'; loopStartSeconds to 0 and loopDurationSeconds to "
            "0, which the Wave Player reads as 'loop the whole asset' - the right answer for a stem "
            "from audio.music.render_stems, which is exactly one loop long by construction. "
            "intensityThreshold defaults to 0 (audible at any intensity)."),
        RPC_PARAM_DEF("save", "boolean",
            "Write the .uasset to disk. false marks the package dirty only.", "true"),
        RPC_PARAM_DEF("overwrite", "boolean",
            "Delete and recreate an existing asset at the path instead of rebuilding it in place. "
            "Rejected with ASSET_IN_USE when other packages reference it.", "false")
    ))
{
    using namespace PwMusicHandlerInternal;

    FString FolderPath;
    if (!Ctx.RequireString(TEXT("path"), FolderPath)) return true;
    FString Name;
    if (!Ctx.RequireString(TEXT("name"), Name)) return true;

    const TArray<TSharedPtr<FJsonValue>>* StemsArray = nullptr;
    if (!Ctx.RequireArray(TEXT("stems"), StemsArray) || StemsArray == nullptr) return true;
    if (StemsArray->Num() == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("'stems' is empty, so the graph would play nothing. An empty layer list is an "
                 "error rather than an empty MetaSound."));
        return true;
    }
    // The graph cap is the engine's, not the score's: MetasoundMixerNode registers 2..8 inputs per
    // channel layout and nothing wider, so a 9..16-track score renders fine and is refused HERE,
    // loudly, rather than being silently pre-mixed down to something the caller cannot control.
    if (StemsArray->Num() > PwMusicGraphMaxStems)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("'stems' carries %d entries; the engine's Audio Mixer node family "
                                 "tops out at %d inputs, so a wider graph cannot be assembled. "
                                 "Build more than one MetaSound Source, or mix stems together "
                                 "before exporting them."),
                StemsArray->Num(), PwMusicGraphMaxStems));
        return true;
    }

    const FString SanitizedName = SanitizeAssetName(Name);
    if (SanitizedName.IsEmpty() || SanitizedName != Name)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PATH,
            FString::Printf(TEXT("Invalid asset name '%s': contains characters that cannot be used "
                                 "in asset names. A valid form would be '%s'."), *Name, *SanitizedName));
        return true;
    }

    FString ValidatedPath;
    FString PathError;
    if (!ValidateAssetCreationPath(FolderPath, Name, ValidatedPath, PathError))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PATH, PathError);
        return true;
    }
    const FString ValidatedFolder = FPackageName::GetLongPackagePath(ValidatedPath);
    if (!ValidatedFolder.Equals(TEXT("/Game")) && !ValidatedFolder.StartsWith(TEXT("/Game/")))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PATH,
            FString::Printf(TEXT("MetaSound assets can only be created under /Game; '%s' resolves to "
                                 "'%s'."), *FolderPath, *ValidatedFolder));
        return true;
    }

    // ---------------------------------------------------------------------------------------
    // Resolve every stem BEFORE anything is created, so an unresolvable path leaves no asset (§3).
    // ---------------------------------------------------------------------------------------
    TArray<FPwMusicGraphStem> Stems;
    TSet<FString> SeenInputs;
    for (int32 Index = 0; Index < StemsArray->Num(); ++Index)
    {
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        if (!(*StemsArray)[Index].IsValid() || !(*StemsArray)[Index]->TryGetObject(Entry) || !Entry)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("stems[%d] is not an object."), Index));
            return true;
        }

        FPwMusicGraphStem Stem;
        (*Entry)->TryGetStringField(TEXT("assetPath"), Stem.AssetPath);
        (*Entry)->TryGetStringField(TEXT("inputName"), Stem.InputName);
        if (Stem.AssetPath.IsEmpty())
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("stems[%d] needs 'assetPath' - the exported SoundWave this "
                                     "layer plays. audio.music.export_stems returns the assetPath of "
                                     "each wave it wrote."), Index));
            return true;
        }

        // Left to the builder's documented default when absent: a Loop Duration of 0 is the Wave
        // Player's own "loop the whole asset", which is exactly right for a stem from render_stems
        // (it IS one loop, seamless by construction). Requiring the number here would make the
        // caller restate something the wave already embodies, and get it wrong when they compute it
        // differently from the renderer.
        (*Entry)->TryGetNumberField(TEXT("loopDurationSeconds"), Stem.LoopDurationSeconds);

        double LoopStart = 0.0;
        (*Entry)->TryGetNumberField(TEXT("loopStartSeconds"), LoopStart);
        if (LoopStart < 0.0)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("stems[%d].loopStartSeconds is %.4f; a negative loop start has "
                                     "no meaning."), Index, LoopStart));
            return true;
        }
        Stem.LoopStartSeconds = LoopStart;

        double Threshold = 0.0;
        (*Entry)->TryGetNumberField(TEXT("intensityThreshold"), Threshold);
        if (Threshold < 0.0 || Threshold > 1.0)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("stems[%d].intensityThreshold is %.4f; intensity is normalized "
                                     "to 0..1, matching a score section's intensityFloor/Ceil."),
                    Index, Threshold));
            return true;
        }
        Stem.IntensityThreshold = Threshold;

        // Only among names the caller actually supplied: an omitted inputName becomes the builder's
        // own "Stem<index>", so two omissions are two distinct inputs rather than a collision.
        if (!Stem.InputName.IsEmpty())
        {
            const FString InputKey = Stem.InputName.ToLower();
            if (SeenInputs.Contains(InputKey))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    FString::Printf(TEXT("stems[%d]: inputName '%s' is already used by an earlier "
                                         "entry. Two layers on one input would leave one of them "
                                         "uncontrollable."), Index, *Stem.InputName));
                return true;
            }
            SeenInputs.Add(InputKey);
        }

        // Loaded, not merely string-checked. PwBuildInteractiveMusicGraph resolves stems itself, so
        // this looks redundant - it is not, and the ordering is the reason: the AssetCreatePolicy
        // Resolve below runs with the caller's `overwrite`, which DELETES an existing asset at the
        // target path. Discovering a bad stem path after that would have destroyed a good asset for
        // a build that could never succeed. LOAD_NoWarn | LOAD_Quiet because a missing stem is an
        // expected outcome this verb reports itself; the engine's own load warning is duplicate
        // noise.
        // GUARDED, because assetPath sits inside an ARRAY ELEMENT. The dispatch-boundary type gate
        // reads top-level params only, so `stems[].assetPath` never met it, and a doubled slash
        // here reaches CreatePackage's Fatal and ends the editor.
        FString StemRefusal;
        if (!PinWrightGuardedLoad::LoadObjectChecked<USoundWave>(
                Stem.AssetPath, &StemRefusal, LOAD_NoWarn | LOAD_Quiet))
        {
            if (!StemRefusal.IsEmpty())
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    FString::Printf(TEXT("stems[%d].assetPath: %s"), Index, *StemRefusal));
                return true;
            }
            Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
                FString::Printf(TEXT("stems[%d]: '%s' does not resolve to a USoundWave. Nothing was "
                                     "created. audio.music.export_stems returns the assetPath of each "
                                     "wave it wrote."), Index, *Stem.AssetPath));
            return true;
        }

        Stems.Add(MoveTemp(Stem));
    }

    const bool bSave = Ctx.GetBool(TEXT("save"), true);
    const bool bOverwrite = Ctx.GetBool(TEXT("overwrite"), false);

    // Non-modal occupancy check where the class is resolvable. MetaSoundSource is reached by
    // reflection because the engine class carries no exported API macro for this module; when the
    // MetaSound plugin is absent the lookup fails and PwBuildInteractiveMusicGraph reports that
    // itself, so a missing class is not turned into a second, weaker error here.
    AssetCreatePolicy::FResolution Resolution;
    UClass* MetaSoundSourceClass =
        FindObject<UClass>(nullptr, TEXT("/Script/MetasoundEngine.MetaSoundSource"));
    if (MetaSoundSourceClass)
    {
        Resolution = AssetCreatePolicy::Resolve(ValidatedPath, Name, MetaSoundSourceClass,
            bOverwrite, /*bRequireExactClass=*/true);
        if (Resolution.IsRejected())
        {
            return AssetCreatePolicy::SendRejection(Ctx, Resolution);
        }
    }

    FPwMusicGraphResult GraphResult;
    FString ErrorCode;
    FString Error;
    if (!PwBuildInteractiveMusicGraph(ValidatedFolder, Name, Stems, bSave, GraphResult, ErrorCode, Error))
    {
        Ctx.SendError(ErrorCode.IsEmpty() ? FString(ErrorCodes::ERR_CREATION_FAILED) : ErrorCode, Error);
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), GraphResult.AssetPath);
    Result->SetNumberField(TEXT("stemCount"), Stems.Num());
    // Straight from the builder's report and never re-derived: `measured` is false on a
    // default-constructed result, so a path that built nothing cannot publish clean counts (§1).
    Result->SetBoolField(TEXT("measured"), GraphResult.bMeasured);
    Result->SetNumberField(TEXT("nodesAdded"), GraphResult.NodesAdded);
    Result->SetNumberField(TEXT("connectionsMade"), GraphResult.ConnectionsMade);
    Result->SetArrayField(TEXT("graphInputs"), StringArray(GraphResult.GraphInputs));
    if (GraphResult.Warnings.Num() > 0)
    {
        // Surfaced beside the still-true counts: a partial build is not a plain success (§5b).
        Result->SetArrayField(TEXT("warnings"), StringArray(GraphResult.Warnings));
    }
    Result->SetStringField(TEXT("runtime"),
        TEXT("Drive the graph inputs as ordinary MetaSound parameters - read graphInputs for the "
             "names this build actually created, and warnings for any that are exposed but not yet "
             "consumed by the graph. For bar-aligned transitions schedule the writes on a Quartz "
             "clock - UQuartzSubsystem::CreateNewClock, SubscribeToQuantizationEvent, "
             "UAudioComponent::PlayQuantized - which is core engine and needs no plugin. This verb "
             "stops at the asset."));

    if (MetaSoundSourceClass)
    {
        AssetCreatePolicy::AddCreateReport(Result, Resolution);
    }

    // Persistence is MEASURED off the asset rather than inferred from the build returning true.
    // PwBuildInteractiveMusicGraph treats a save that did not land as a failure and publishes no
    // persistence field of its own, so the only honest {existsOnDisk, pendingSave} in the response
    // is one read back off the object (§5).
    UObject* Created = FindCreatedAsset(GraphResult.AssetPath);
    Result->SetBoolField(TEXT("saveRequested"), bSave);
    if (Created)
    {
        AddAssetVerification(Result, Created);
    }
    else
    {
        Result->SetStringField(TEXT("persistenceNote"),
            TEXT("The created asset could not be resolved by path after the build, so no "
                 "existsOnDisk / pendingSave measurement is published. Check the path with "
                 "asset.exists before treating the build as durable."));
    }

    Ctx.SendSuccess(FString::Printf(
        TEXT("Built %s from %d stem(s): %d node(s), %d connection(s)."),
        *GraphResult.AssetPath, Stems.Num(), GraphResult.NodesAdded, GraphResult.ConnectionsMade),
        Result);
    return true;
}
