// Copyright (c) 2026 Alexander Penkin. MIT License.

// AudioSynthSchemaHandler.cpp - audio.synth.describe_schema
//
// With no preset library shipping, this verb plus the cookbook wiki page is the
// entire cold-start path for a caller writing its first recipe, so it publishes
// the whole grammar as structured JSON: every generator and effect kind with its
// parameters (type, unit, range, required-or-default, one-line meaning), the
// envelope and curve forms, the caps, the metric names `targets` accepts, and a
// worked example.
//
// SIZE: the response spill threshold is 10,000 characters
// (Utils/HttpResponseSpill.cpp). A full dump is roughly twice that, so the verb
// is sectioned - `overview` (the default) is a compact topology plus kind lists
// plus the worked example, and `generators` / `effects` / `envelopes` /
// `targets` are the drill-downs. Each of those stays inline; only the explicit
// `all` opt-in exceeds the threshold and spills, and the response says so.
//
// Everything published here is read out of the spec tables in
// AudioGen/PwSynthRecipe.cpp, so the documentation cannot drift from the
// validation: adding a parameter row updates both at once.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"

#include "AudioGen/PwSynthRecipe.h"
#include "Utils/HttpResponseSpill.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace PwSynthSchemaHandler
{
    const TCHAR* const SectionOverview = TEXT("overview");
    const TCHAR* const SectionGenerators = TEXT("generators");
    const TCHAR* const SectionEffects = TEXT("effects");
    const TCHAR* const SectionEnvelopes = TEXT("envelopes");
    const TCHAR* const SectionTargets = TEXT("targets");
    const TCHAR* const SectionAll = TEXT("all");

    // A minimal but complete recipe: two layers, a per-layer effect chain, an
    // amp and pitch envelope, FM modulation, a master effect, normalization and
    // a metric target. Kept in its authored (terse) form so it also demonstrates
    // which fields may be omitted. Validated at request time, not asserted.
    const TCHAR* const ExampleRecipeJson = TEXT(R"JSON(
{
  "durationMs": 900,
  "seed": 7,
  "layers": [
    {
      "gainDb": -3,
      "pan": -0.2,
      "generator": { "kind": "osc", "params": { "waveform": "saw", "frequencyHz": 880 } },
      "ampEnvelope": [
        { "timeMs": 0, "value": 0, "curve": "linear" },
        { "timeMs": 8, "value": 1, "curve": "exp" },
        { "timeMs": 600, "value": 0 }
      ],
      "pitchEnvelope": [
        { "timeMs": 0, "semitones": 12 },
        { "timeMs": 600, "semitones": -24 }
      ],
      "modulation": { "fm": { "depth": 2.5, "rateHz": 55 } },
      "fx": [
        { "kind": "filter", "params": { "type": "lowpass", "cutoffHz": 6000, "resonance": 3 } },
        { "kind": "distort", "params": { "type": "soft", "drive": 6 } }
      ]
    },
    {
      "gainDb": -9,
      "pan": 0.3,
      "generator": { "kind": "noise", "params": { "color": "white", "highCutHz": 9000 } },
      "ampEnvelope": [
        { "timeMs": 0, "value": 1, "curve": "exp" },
        { "timeMs": 120, "value": 0 }
      ]
    }
  ],
  "master": {
    "fx": [ { "kind": "reverb", "params": { "decayMs": 700, "mix": 0.18 } } ],
    "normalize": { "mode": "peak", "target": -1 },
    "fadeOutMs": 15
  },
  "targets": { "peakDb": { "min": -3, "max": -0.5 }, "durationMs": { "max": 1000 } }
}
)JSON");

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

    TArray<FString> GeneratorKindNames()
    {
        TArray<FString> Names;
        for (uint8 Index = 1; Index < static_cast<uint8>(EPwSynthGeneratorKind::Count); ++Index)
        {
            Names.Add(PwSynthGeneratorKindToString(static_cast<EPwSynthGeneratorKind>(Index)));
        }
        return Names;
    }

    TArray<FString> FxKindNames()
    {
        TArray<FString> Names;
        for (uint8 Index = 1; Index < static_cast<uint8>(EPwSynthFxKind::Count); ++Index)
        {
            Names.Add(PwSynthFxKindToString(static_cast<EPwSynthFxKind>(Index)));
        }
        return Names;
    }

    // One parameter row, exactly as the parser enforces it.
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

    TSharedPtr<FJsonObject> KindToJson(const FPwSynthKindSpec& Kind)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("kind"), Kind.Name);
        Obj->SetStringField(TEXT("summary"), Kind.Summary);
        if (Kind.bMasterOnly)
        {
            Obj->SetBoolField(TEXT("masterOnly"), true);
        }
        if (Kind.Constraint)
        {
            Obj->SetStringField(TEXT("constraint"), Kind.Constraint);
        }

        TArray<TSharedPtr<FJsonValue>> Params;
        if (Kind.Params)
        {
            Params.Reserve(Kind.Params->Num());
            for (const FPwSynthParamSpec& Spec : *Kind.Params)
            {
                Params.Add(MakeShared<FJsonValueObject>(ParamToJson(Spec)));
            }
        }
        Obj->SetArrayField(TEXT("params"), Params);
        return Obj;
    }

    // Compact per-kind entry used by the overview: just enough to pick a kind.
    // Deliberately carries no parameter data - the drill-down owns that, and
    // keeping the digest parameter-free means a wave-2 agent adding DSP
    // parameters cannot grow the overview toward the spill threshold at all.
    TSharedPtr<FJsonObject> KindDigestToJson(const FPwSynthKindSpec& Kind)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("kind"), Kind.Name);
        Obj->SetStringField(TEXT("summary"), Kind.Summary);
        if (Kind.bMasterOnly)
        {
            Obj->SetBoolField(TEXT("masterOnly"), true);
        }
        return Obj;
    }

    void AddCaps(const TSharedPtr<FJsonObject>& Result)
    {
        TSharedPtr<FJsonObject> Caps = MakeShared<FJsonObject>();
        Caps->SetNumberField(TEXT("maxLayers"), PwSynthLimits::MaxLayers);
        Caps->SetNumberField(TEXT("maxLayerFx"), PwSynthLimits::MaxLayerFx);
        Caps->SetNumberField(TEXT("maxMasterFx"), PwSynthLimits::MaxMasterFx);
        Caps->SetNumberField(TEXT("maxEnvelopePoints"), PwSynthLimits::MaxEnvelopePoints);
        Caps->SetNumberField(TEXT("minDurationMs"), PwSynthLimits::MinDurationMs);
        Caps->SetNumberField(TEXT("maxDurationMs"), PwSynthLimits::MaxDurationMs);
        Caps->SetNumberField(TEXT("minSampleRate"), PwSynthLimits::MinSampleRate);
        Caps->SetNumberField(TEXT("maxSampleRate"), PwSynthLimits::MaxSampleRate);
        Caps->SetStringField(TEXT("enforcement"),
            TEXT("Exceeding a cap is a rejection, never a clamp: a silently clamped recipe would not round-trip through patch."));
        Result->SetObjectField(TEXT("caps"), Caps);
    }

    void AddTopology(const TSharedPtr<FJsonObject>& Result)
    {
        TSharedPtr<FJsonObject> Topology = MakeShared<FJsonObject>();
        Topology->SetStringField(TEXT("shape"),
            TEXT("Fixed signal chain, not a node graph: recipe -> layers[] -> (generator -> ampEnvelope/pitchEnvelope/modulation -> fx[]) -> stereo mix bus -> master.fx[] -> normalize -> fades."));
        Topology->SetStringField(TEXT("channels"),
            TEXT("Generators are mono. The mix bus is stereo and each layer's pan (-1..1) places it. No generator takes stereo parameters."));
        Topology->SetStringField(TEXT("timing"),
            TEXT("durationMs is the whole render. A layer starts at its startMs; its envelope times are relative to that start, not absolute."));
        Topology->SetStringField(TEXT("recipe"),
            TEXT("{version, seed, sampleRate, durationMs, layers[], master{fx[],normalize{mode,target},fadeInMs,fadeOutMs}, targets{}}"));
        Topology->SetStringField(TEXT("layer"),
            TEXT("{startMs, gainDb, pan, generator{kind,params}, ampEnvelope[{timeMs,value,curve}], pitchEnvelope[{timeMs,semitones}], modulation{fm|am|ring:{depth,rateHz,source}}, fx[{kind,params}]}"));
        Result->SetObjectField(TEXT("topology"), Topology);
    }

    void AddDefaultsAndRequired(const TSharedPtr<FJsonObject>& Result)
    {
        // Only values that are never wrong in the field carry a default; every
        // other field is required so a guess can never reach the render.
        TSharedPtr<FJsonObject> Defaults = MakeShared<FJsonObject>();
        Defaults->SetNumberField(TEXT("version"), PwSynthLimits::RecipeVersion);
        Defaults->SetNumberField(TEXT("seed"), PwSynthLimits::DefaultSeed);
        Defaults->SetNumberField(TEXT("sampleRate"), PwSynthLimits::DefaultSampleRate);
        Defaults->SetNumberField(TEXT("layers[].startMs"), PwSynthLimits::DefaultLayerStartMs);
        Defaults->SetNumberField(TEXT("layers[].gainDb"), PwSynthLimits::DefaultLayerGainDb);
        Defaults->SetNumberField(TEXT("layers[].pan"), PwSynthLimits::DefaultLayerPan);
        Defaults->SetStringField(TEXT("layers[].ampEnvelope[].curve"), PwSynthCurveToString(EPwSynthCurve::Linear));
        Defaults->SetStringField(TEXT("layers[].modulation.*.source"), PwSynthModSourceToString(EPwSynthModSource::Sine));
        Defaults->SetNumberField(TEXT("master.fadeInMs"), PwSynthLimits::DefaultFadeMs);
        Defaults->SetNumberField(TEXT("master.fadeOutMs"), PwSynthLimits::DefaultFadeMs);
        Result->SetObjectField(TEXT("defaults"), Defaults);

        TArray<FString> Required = {
            TEXT("durationMs - the render length; there is no default duration"),
            TEXT("layers - at least one, at most 8"),
            TEXT("layers[].generator.kind - there is no default generator"),
            TEXT("layers[].fx[].kind and master.fx[].kind - there is no no-op effect"),
            TEXT("every parameter marked required:true in the generators / effects sections"),
            TEXT("master.normalize.mode and .target whenever a normalize block is present"),
            TEXT("at least one of min / max on every targets entry")
        };
        Result->SetArrayField(TEXT("required"), StringArray(Required));
    }

    void AddStrictness(const TSharedPtr<FJsonObject>& Result)
    {
        TArray<FString> Rules = {
            TEXT("An unrecognised generator kind, effect kind, filter type, curve, normalize mode or modulator source is rejected. It never degrades into a default oscillator or a no-op effect."),
            TEXT("An unknown object key is rejected at every level and the error names its path, so a typo surfaces instead of being silently dropped."),
            TEXT("A parameter with no correct default is required. There is no invented 440 Hz, no invented cutoff, no invented mode set."),
            TEXT("Values are type-strict: a number field rejects \"880\" as a string, so a patched recipe cannot change type behind you."),
            TEXT("Nothing is clamped. A cap, a range or an ordering violation is an error naming the field."),
            TEXT("Errors name a JSON path such as layers[2].fx[1].kind - patch that path and resubmit.")
        };
        Result->SetArrayField(TEXT("strictness"), StringArray(Rules));

        TSharedPtr<FJsonObject> Codes = MakeShared<FJsonObject>();
        Codes->SetStringField(ErrorCodes::ERR_UNKNOWN_GENERATOR,
            TEXT("generator.kind is not one of the six generator kinds."));
        Codes->SetStringField(ErrorCodes::ERR_UNKNOWN_EFFECT,
            TEXT("an fx[].kind is not one of the fifteen effect kinds."));
        Codes->SetStringField(ErrorCodes::ERR_INVALID_PARAMS,
            TEXT("a value problem: a required value is missing, the JSON type is wrong, the number is out of range, or the token is outside a closed vocabulary."));
        Codes->SetStringField(ErrorCodes::ERR_INVALID_RECIPE,
            TEXT("a shape problem: an unknown key, a cap exceeded, a missing container, an ordering violation or a cross-field contradiction."));
        Result->SetObjectField(TEXT("errorCodes"), Codes);
    }

    // Publishes the worked example AND whether it actually parses, measured by
    // running it through the same ParseSynthRecipe the render path uses. The
    // boolean is a measurement, not a literal (rpc-design §1) - if the example
    // ever drifts out of the schema the response says so instead of teaching a
    // recipe that will be rejected.
    void AddExample(const TSharedPtr<FJsonObject>& Result)
    {
        TSharedPtr<FJsonObject> Example;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FString(ExampleRecipeJson));
        const bool bDeserialized = FJsonSerializer::Deserialize(Reader, Example) && Example.IsValid();

        bool bValidated = false;
        FPwSynthRecipeError ParseError;
        if (bDeserialized)
        {
            FPwSynthRecipe Parsed;
            bValidated = ParseSynthRecipe(Example, Parsed, ParseError);
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
            TEXT("Shown in authored form: omitted optional fields fall back to the documented defaults. audio.synth serializes back with every default made explicit, which is the form to apply JSON Patch to."));
    }

    // Headroom left for the page/pageCount/nextPage fields and for the response
    // growing slightly between the packing measurement and the final emit.
    constexpr int32 PageOverheadReserve = 512;

    int32 MeasureChars(const TSharedPtr<FJsonObject>& Object)
    {
        FString Text;
        // Same writer policy the transport serializes responses with
        // (JsonRpc::Serialize / HttpResponseSpill both use TJsonWriterFactory<>),
        // so this measures the bytes the spill gate will measure, not a proxy.
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
        FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
        Writer->Close();
        return Text.Len();
    }

    // Greedily packs kinds into pages by SERIALIZING each trial page and stopping
    // before the inline budget, rather than assuming a per-kind average. That
    // makes the page size self-tuning: a wave-2 agent adding DSP parameters
    // re-packs the pages automatically instead of silently pushing the section
    // over the spill threshold. Returns the index of the first kind on each page.
    TArray<int32> PackPages(const TSharedPtr<FJsonObject>& Result, const TCHAR* FieldName,
        const TArray<const FPwSynthKindSpec*>& Kinds)
    {
        const int32 Budget = FMath::Max(1024,
            HttpResponseSpill::GetDefaultThresholdCharacters() - PageOverheadReserve);

        TArray<int32> PageStarts;
        int32 Index = 0;
        while (Index < Kinds.Num())
        {
            PageStarts.Add(Index);

            TArray<TSharedPtr<FJsonValue>> Entries;
            int32 Count = 0;
            while (Index + Count < Kinds.Num())
            {
                Entries.Add(MakeShared<FJsonValueObject>(KindToJson(*Kinds[Index + Count])));
                Result->SetArrayField(FieldName, Entries);
                if (Count > 0 && MeasureChars(Result) > Budget)
                {
                    Entries.Pop();
                    break;
                }
                ++Count;
            }

            // A single kind that alone blows the budget still ships on its own
            // page: truncating it would publish a schema that omits parameters.
            Index += FMath::Max(1, Count);
        }

        Result->SetArrayField(FieldName, TArray<TSharedPtr<FJsonValue>>());
        return PageStarts;
    }

    TArray<const FPwSynthKindSpec*> CollectGeneratorSpecs(EPwSynthGeneratorKind Only)
    {
        TArray<const FPwSynthKindSpec*> Specs;
        for (uint8 Index = 1; Index < static_cast<uint8>(EPwSynthGeneratorKind::Count); ++Index)
        {
            const EPwSynthGeneratorKind Kind = static_cast<EPwSynthGeneratorKind>(Index);
            if (Only == EPwSynthGeneratorKind::Unspecified || Only == Kind)
            {
                Specs.Add(&PwSynthGeneratorSpec(Kind));
            }
        }
        return Specs;
    }

    TArray<const FPwSynthKindSpec*> CollectFxSpecs(EPwSynthFxKind Only)
    {
        TArray<const FPwSynthKindSpec*> Specs;
        for (uint8 Index = 1; Index < static_cast<uint8>(EPwSynthFxKind::Count); ++Index)
        {
            const EPwSynthFxKind Kind = static_cast<EPwSynthFxKind>(Index);
            if (Only == EPwSynthFxKind::Unspecified || Only == Kind)
            {
                Specs.Add(&PwSynthFxSpec(Kind));
            }
        }
        return Specs;
    }

    // Emits one page of a kind listing. Returns false when RequestedPage is out of
    // range, having already sent the error: an empty page would read identically to
    // "this section has no more kinds".
    bool AddPagedKinds(FHandlerContext& Ctx, const TSharedPtr<FJsonObject>& Result,
        const TCHAR* FieldName, const TArray<const FPwSynthKindSpec*>& Kinds,
        int32 RequestedPage, bool bPaginate)
    {
        if (!bPaginate)
        {
            TArray<TSharedPtr<FJsonValue>> Entries;
            Entries.Reserve(Kinds.Num());
            for (const FPwSynthKindSpec* Kind : Kinds)
            {
                Entries.Add(MakeShared<FJsonValueObject>(KindToJson(*Kind)));
            }
            Result->SetArrayField(FieldName, Entries);
            return true;
        }

        const TArray<int32> PageStarts = PackPages(Result, FieldName, Kinds);
        const int32 PageCount = FMath::Max(1, PageStarts.Num());

        if (RequestedPage < 1 || RequestedPage > PageCount)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("page: %d is out of range for section '%s', which has %d page(s)."),
                    RequestedPage, FieldName, PageCount));
            return false;
        }

        const int32 First = PageStarts.IsValidIndex(RequestedPage - 1) ? PageStarts[RequestedPage - 1] : 0;
        const int32 Last = PageStarts.IsValidIndex(RequestedPage) ? PageStarts[RequestedPage] : Kinds.Num();

        TArray<TSharedPtr<FJsonValue>> Entries;
        Entries.Reserve(Last - First);
        for (int32 Index = First; Index < Last; ++Index)
        {
            Entries.Add(MakeShared<FJsonValueObject>(KindToJson(*Kinds[Index])));
        }
        Result->SetArrayField(FieldName, Entries);

        Result->SetNumberField(TEXT("page"), RequestedPage);
        Result->SetNumberField(TEXT("pageCount"), PageCount);
        Result->SetNumberField(TEXT("kindsOnPage"), Entries.Num());
        if (RequestedPage < PageCount)
        {
            Result->SetNumberField(TEXT("nextPage"), RequestedPage + 1);
            Result->SetStringField(TEXT("pagingNote"),
                FString::Printf(TEXT("This section is split across %d pages so each stays inline instead of spilling to a file. Re-call with page=%d for the rest, or pass kind='<name>' to fetch one kind directly."),
                    PageCount, RequestedPage + 1));
        }
        return true;
    }

    void AddGeneratorsNote(const TSharedPtr<FJsonObject>& Result)
    {
        Result->SetStringField(TEXT("generatorsNote"),
            TEXT("Generators are mono; place a layer with its pan. A layer has exactly one generator - build a composite sound from several layers, not from a nested generator."));
    }

    void AddEffectsNote(const TSharedPtr<FJsonObject>& Result)
    {
        Result->SetStringField(TEXT("effectsNote"),
            FString::Printf(TEXT("The same vocabulary serves layers[].fx (cap %d) and master.fx (cap %d); both chains run in array order. A `mix` on a parallel effect (delay, reverb, chorus, flanger, phaser, ringmod, convolve) is required because fully wet would delete the dry signal, while an in-line effect defaults to mix 1. Effects marked masterOnly need the stereo bus and are rejected inside a layer."),
                PwSynthLimits::MaxLayerFx, PwSynthLimits::MaxMasterFx));
    }

    void AddEnvelopes(const TSharedPtr<FJsonObject>& Result)
    {
        TSharedPtr<FJsonObject> Amp = MakeShared<FJsonObject>();
        Amp->SetStringField(TEXT("shape"), TEXT("[{timeMs, value, curve}] - piecewise, at least 1 point."));
        Amp->SetStringField(TEXT("timeMs"), FString::Printf(
            TEXT("Required. Relative to the layer's startMs, non-decreasing, and not past the layer's end. Range 0..%g ms."),
            PwSynthLimits::MaxDurationMs));
        Amp->SetStringField(TEXT("value"), TEXT("Required. Linear amplitude 0..1; layer level lives on gainDb, not here."));
        Amp->SetStringField(TEXT("curve"), FString::Printf(
            TEXT("Optional, default '%s'. Shapes the segment FROM this point TO the next; the last point's curve is unused."),
            PwSynthCurveToString(EPwSynthCurve::Linear)));
        Amp->SetStringField(TEXT("omitted"),
            TEXT("Omitting ampEnvelope means no amplitude shaping at all: the layer runs at unity for its whole span and can click at both edges."));
        Result->SetObjectField(TEXT("ampEnvelope"), Amp);

        TSharedPtr<FJsonObject> Curves = MakeShared<FJsonObject>();
        for (uint8 Index = 0; Index < static_cast<uint8>(EPwSynthCurve::Count); ++Index)
        {
            const EPwSynthCurve Curve = static_cast<EPwSynthCurve>(Index);
            Curves->SetStringField(PwSynthCurveToString(Curve), PwSynthCurveMeaning(Curve));
        }
        Result->SetObjectField(TEXT("curves"), Curves);

        TSharedPtr<FJsonObject> Pitch = MakeShared<FJsonObject>();
        Pitch->SetStringField(TEXT("shape"), TEXT("[{timeMs, semitones}] - piecewise, segments are linear in semitones; there is no per-segment curve."));
        Pitch->SetStringField(TEXT("timeMs"), TEXT("Required. Same rules as the amp envelope: layer-relative, non-decreasing, inside the layer."));
        Pitch->SetStringField(TEXT("semitones"), TEXT("Required. Offset from the generator's own pitch, -48..48."));
        Result->SetObjectField(TEXT("pitchEnvelope"), Pitch);

        TSharedPtr<FJsonObject> Modulation = MakeShared<FJsonObject>();
        Modulation->SetStringField(TEXT("shape"),
            TEXT("{\"fm\"|\"am\"|\"ring\": {depth, rateHz, source}} - exactly one routing. Omit the whole field for no modulation; an empty object is rejected."));
        TSharedPtr<FJsonObject> Routings = MakeShared<FJsonObject>();
        for (uint8 Index = 1; Index < static_cast<uint8>(EPwSynthModulationRouting::Count); ++Index)
        {
            const EPwSynthModulationRouting Routing = static_cast<EPwSynthModulationRouting>(Index);
            Routings->SetStringField(PwSynthModRoutingToString(Routing),
                Routing == EPwSynthModulationRouting::Fm ? TEXT("Frequency modulation; depth is the FM index.") :
                Routing == EPwSynthModulationRouting::Am ? TEXT("Amplitude modulation; depth is 0..1 and the carrier survives.") :
                TEXT("Ring modulation; depth is 0..1 and the carrier is suppressed."));
        }
        Modulation->SetObjectField(TEXT("routings"), Routings);

        TArray<TSharedPtr<FJsonValue>> ModParams;
        for (const FPwSynthParamSpec& Spec : PwSynthModulationParamSpecs())
        {
            ModParams.Add(MakeShared<FJsonValueObject>(ParamToJson(Spec)));
        }
        Modulation->SetArrayField(TEXT("params"), ModParams);
        Result->SetObjectField(TEXT("modulation"), Modulation);

        TSharedPtr<FJsonObject> Normalize = MakeShared<FJsonObject>();
        Normalize->SetStringField(TEXT("shape"), TEXT("master.normalize = {mode, target}; omit the block for no normalization."));
        Normalize->SetStringField(TEXT("mode"), TEXT("Required. 'peak' scales the true peak onto target in dBFS; 'lufs' scales integrated loudness onto target in LUFS."));
        Normalize->SetStringField(TEXT("target"), TEXT("Required. No default: -1 dBFS and -16 LUFS are not interchangeable, so the caller must state which one it means."));
        Result->SetObjectField(TEXT("normalize"), Normalize);
    }

    void AddTargets(const TSharedPtr<FJsonObject>& Result)
    {
        TSharedPtr<FJsonObject> Targets = MakeShared<FJsonObject>();
        Targets->SetStringField(TEXT("shape"),
            TEXT("targets = {\"<metric>\": {min?, max?}} - every metric optional, but each entry needs at least one bound."));
        Targets->SetStringField(TEXT("purpose"),
            TEXT("A machine-checkable acceptance range for the rendered buffer. Unknown metric names are rejected so a typo cannot read as a satisfied constraint."));

        TArray<TSharedPtr<FJsonValue>> Metrics;
        for (uint8 Index = 0; Index < static_cast<uint8>(EPwSynthMetric::Count); ++Index)
        {
            const FPwSynthMetricSpec& Spec = PwSynthMetricSpec(static_cast<EPwSynthMetric>(Index));
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("metric"), Spec.Name);
            Obj->SetStringField(TEXT("unit"), Spec.Unit);
            Obj->SetStringField(TEXT("meaning"), Spec.Meaning);
            Metrics.Add(MakeShared<FJsonValueObject>(Obj));
        }
        Targets->SetArrayField(TEXT("metrics"), Metrics);
        Result->SetObjectField(TEXT("targets"), Targets);
    }

    void AddOverview(const TSharedPtr<FJsonObject>& Result)
    {
        AddTopology(Result);
        AddCaps(Result);
        AddDefaultsAndRequired(Result);
        AddStrictness(Result);

        TArray<TSharedPtr<FJsonValue>> Generators;
        for (uint8 Index = 1; Index < static_cast<uint8>(EPwSynthGeneratorKind::Count); ++Index)
        {
            Generators.Add(MakeShared<FJsonValueObject>(
                KindDigestToJson(PwSynthGeneratorSpec(static_cast<EPwSynthGeneratorKind>(Index)))));
        }
        Result->SetArrayField(TEXT("generatorKinds"), Generators);

        TArray<TSharedPtr<FJsonValue>> Effects;
        for (uint8 Index = 1; Index < static_cast<uint8>(EPwSynthFxKind::Count); ++Index)
        {
            Effects.Add(MakeShared<FJsonValueObject>(
                KindDigestToJson(PwSynthFxSpec(static_cast<EPwSynthFxKind>(Index)))));
        }
        Result->SetArrayField(TEXT("effectKinds"), Effects);

        AddExample(Result);
    }
}

REGISTER_RPC_HANDLER("audio.synth.describe_schema", "audio.synth",
    "Machine-readable grammar of the audio.synth recipe: generator and effect kinds with every parameter's type, unit, range and required-or-default state, the envelope / curve / modulation / normalize forms, the caps, the metric names targets accepts, and a validated worked example.",
    RPC_PARAMS(
        RPC_PARAM_DEF("section", "string",
            "Which slice to return. 'overview' (default) is the compact topology, caps, defaults, strictness rules, kind digests and a worked example. 'generators' / 'effects' / 'envelopes' / 'targets' are the full drill-downs. 'all' concatenates everything and deliberately exceeds the 10000-character inline threshold, so it comes back as a spill file.",
            "overview"),
        RPC_PARAM_OPT("kind", "string",
            "Restrict a 'generators' or 'effects' section to a single kind (e.g. 'granular', 'compressor'). An unrecognised kind is rejected rather than ignored. Has no effect on the other sections."),
        RPC_PARAM_DEF("page", "integer",
            "Which page of a 'generators' or 'effects' listing to return. Those sections are split so each page stays under the inline response threshold; the response reports pageCount and nextPage. Ignored when 'kind' narrows the section to one entry, and by the other sections.",
            "1")
    ))
{
    using namespace PwSynthSchemaHandler;

    const FString Section = Ctx.GetString(TEXT("section"), SectionOverview).TrimStartAndEnd().ToLower();
    const FString KindFilter = Ctx.GetString(TEXT("kind")).TrimStartAndEnd();
    const int32 RequestedPage = Ctx.GetInt(TEXT("page"), 1);

    const TArray<FString> ValidSections = {
        SectionOverview, SectionGenerators, SectionEffects, SectionEnvelopes, SectionTargets, SectionAll
    };
    if (!ValidSections.Contains(Section))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("section: '%s' is not a section. Accepted: %s."),
                *Section, *FString::Join(ValidSections, TEXT(", "))));
        return true;
    }

    const bool bWantsGenerators = (Section == SectionGenerators) || (Section == SectionAll);
    const bool bWantsEffects = (Section == SectionEffects) || (Section == SectionAll);

    // A kind filter that matches nothing is a caller error, not an empty result:
    // a zero-entry section reads identically to "this kind has no parameters".
    EPwSynthGeneratorKind GeneratorFilter = EPwSynthGeneratorKind::Unspecified;
    EPwSynthFxKind FxFilter = EPwSynthFxKind::Unspecified;
    if (!KindFilter.IsEmpty())
    {
        if (Section == SectionGenerators)
        {
            if (!PwSynthGeneratorKindFromString(KindFilter, GeneratorFilter))
            {
                Ctx.SendError(ErrorCodes::ERR_UNKNOWN_GENERATOR,
                    FString::Printf(TEXT("kind: '%s' is not a generator kind. Accepted: %s."),
                        *KindFilter, *FString::Join(GeneratorKindNames(), TEXT(", "))));
                return true;
            }
        }
        else if (Section == SectionEffects)
        {
            if (!PwSynthFxKindFromString(KindFilter, FxFilter))
            {
                Ctx.SendError(ErrorCodes::ERR_UNKNOWN_EFFECT,
                    FString::Printf(TEXT("kind: '%s' is not an effect kind. Accepted: %s."),
                        *KindFilter, *FString::Join(FxKindNames(), TEXT(", "))));
                return true;
            }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("section"), Section);
    Result->SetNumberField(TEXT("recipeVersion"), PwSynthLimits::RecipeVersion);
    Result->SetArrayField(TEXT("sections"), StringArray(ValidSections));

    if (Section == SectionOverview || Section == SectionAll)
    {
        AddOverview(Result);
    }
    if (bWantsGenerators)
    {
        AddGeneratorsNote(Result);
        // 'all' is the explicit whole-schema opt-in and is documented to spill, so
        // it is the one caller that gets every kind in one unpaged listing.
        const bool bPaginate = (Section != SectionAll) &&
            (GeneratorFilter == EPwSynthGeneratorKind::Unspecified);
        if (!AddPagedKinds(Ctx, Result, TEXT("generators"),
                CollectGeneratorSpecs(GeneratorFilter), RequestedPage, bPaginate))
        {
            return true;
        }
    }
    if (bWantsEffects)
    {
        AddEffectsNote(Result);
        const bool bPaginate = (Section != SectionAll) &&
            (FxFilter == EPwSynthFxKind::Unspecified);
        if (!AddPagedKinds(Ctx, Result, TEXT("effects"),
                CollectFxSpecs(FxFilter), RequestedPage, bPaginate))
        {
            return true;
        }
    }
    if (Section == SectionEnvelopes || Section == SectionAll)
    {
        AddEnvelopes(Result);
    }
    if (Section == SectionTargets || Section == SectionAll)
    {
        AddTargets(Result);
    }

    // Sections that are not self-contained say where the rest is, so the caller
    // drills in instead of reaching for 'all' and paying a spill file.
    if (Section == SectionOverview)
    {
        Result->SetStringField(TEXT("next"),
            TEXT("Parameter ranges are in the drill-downs: call audio.synth.describe_schema with section='generators', 'effects', 'envelopes' or 'targets'. Add kind='<name>' to fetch one kind, or follow nextPage when a listing is split across pages. 'all' returns every section at once but exceeds the inline response threshold and comes back as a file."));
    }
    else if (Section != SectionAll)
    {
        Result->SetStringField(TEXT("next"),
            TEXT("Call audio.synth.describe_schema with no arguments for the topology, caps, defaults and a worked example."));
    }

    Ctx.SendSuccess(Result);
    return true;
}
