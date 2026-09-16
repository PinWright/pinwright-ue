// Copyright (c) 2026 Alexander Penkin. MIT License.

// Niagara graph node-type catalog, op-registry search, and module-script search handlers.
// Provides niagara.graph.list_node_types, niagara.graph.search_ops, and niagara.search_modules.
#include "Handlers/Niagara/NiagaraSearchHandler.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Niagara/NiagaraEditTypes.h"
#include "Handlers/Niagara/NiagaraOpCatalog.h"

#include "NiagaraEditorCommon.h"
#include "NiagaraNode.h"
#include "NiagaraCommon.h"
#include "NiagaraScript.h"
#include "UObject/UObjectIterator.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

// ---------------------------------------------------------------------------
// Helpers in NiagaraSearch namespace (also declared in NiagaraSearchHandler.h)
// ---------------------------------------------------------------------------

namespace NiagaraSearch
{

bool ResolveSearchLimit(const FHandlerContext& Ctx, int32& OutLimit)
{
    double RequestedLimit = static_cast<double>(DefaultSearchLimit);
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    if (Payload.IsValid() && Payload->HasField(TEXT("limit")))
    {
        const TSharedPtr<FJsonValue> LimitValue = Payload->TryGetField(TEXT("limit"));
        if (!LimitValue.IsValid() ||
            !LimitValue->TryGetNumber(RequestedLimit) ||
            !FMath::IsFinite(RequestedLimit))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("limit must be a finite number accepted by integer coercion"));
            return false;
        }
    }

    // Clamp before converting so a large positive JSON number (including a numeric
    // string) can never wrap through int32 before reaching the bounded array resize.
    if (RequestedLimit > static_cast<double>(MaxSearchLimit))
    {
        OutLimit = MaxSearchLimit;
        return true;
    }

    // FHandlerContext::GetInt truncates toward zero. Values in (-1, 0) therefore
    // resolve to zero, while values whose coerced integer is negative are refused.
    if (RequestedLimit < -1.0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("limit must resolve to >= 0; got %.17g"), RequestedLimit));
        return false;
    }

    const int32 CoercedLimit = static_cast<int32>(RequestedLimit);
    if (CoercedLimit < 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("limit must resolve to >= 0; got %.17g"), RequestedLimit));
        return false;
    }

    OutLimit = CoercedLimit;
    return true;
}

FString PayloadKindFor(UClass* NodeClass)
{
    if (!NodeClass)
    {
        return TEXT("none");
    }
    const FString ClassName = NodeClass->GetName();

    if (ClassName == TEXT("NiagaraNodeFunctionCall") || ClassName == TEXT("NiagaraNodeAssignment"))
    {
        return TEXT("script_asset");
    }
    if (ClassName == TEXT("NiagaraNodeOp"))
    {
        return TEXT("op_name");
    }
    if (ClassName == TEXT("NiagaraNodeReadDataSet") || ClassName == TEXT("NiagaraNodeWriteDataSet"))
    {
        return TEXT("dataset_ref");
    }
    if (ClassName == TEXT("NiagaraNodeCustomHlsl"))
    {
        return TEXT("hlsl_text");
    }
    if (ClassName == TEXT("NiagaraNodeInput") || ClassName == TEXT("NiagaraNodeOutput"))
    {
        return TEXT("parameter_ref");
    }
    return TEXT("none");
}

FString BuildOpSignature(const FNiagaraOpInfo& Op)
{
    // Collect input type names
    TArray<FString> InputTypes;
    for (const FNiagaraOpInOutInfo& In : Op.Inputs)
    {
        InputTypes.Add(In.DataType.GetName());
    }

    // Collect output type names (usually one)
    TArray<FString> OutputTypes;
    for (const FNiagaraOpInOutInfo& Out : Op.Outputs)
    {
        OutputTypes.Add(Out.DataType.GetName());
    }

    const FString InputStr = FString::Join(InputTypes, TEXT(", "));
    const FString OutputStr = FString::Join(OutputTypes, TEXT(", "));

    return FString::Printf(TEXT("%s(%s) -> %s"), *Op.Name.ToString(), *InputStr, *OutputStr);
}

int32 ScoreOpMatch(const FString& Query,
                   const FString& Name,
                   const FString& Alternate,
                   const FString& Category,
                   const FString& Keywords)
{
    if (Query.IsEmpty())
    {
        return 0;
    }

    const FString QueryLow    = Query.ToLower();
    const FString NameLow     = Name.ToLower();
    const FString AltLow      = Alternate.ToLower();
    const FString CategoryLow = Category.ToLower();
    const FString KeywordsLow = Keywords.ToLower();

    int32 Score = 0;

    // Exact name match
    if (NameLow == QueryLow)
    {
        Score = FMath::Max(Score, 1000);
    }
    // Prefix match on name
    else if (NameLow.StartsWith(QueryLow))
    {
        Score = FMath::Max(Score, 500);
    }
    // Contains match on name
    else if (NameLow.Contains(QueryLow))
    {
        Score = FMath::Max(Score, 100);
    }

    // Token-level match on name (split by space/underscore)
    if (Score < 50)
    {
        TArray<FString> Tokens;
        NameLow.ParseIntoArray(Tokens, TEXT(" "));
        for (const FString& Token : Tokens)
        {
            if (Token.Contains(QueryLow))
            {
                Score = FMath::Max(Score, 50);
                break;
            }
        }
    }

    // Fuzzy AlternateSearchName match
    if (!AltLow.IsEmpty())
    {
        if (AltLow == QueryLow || AltLow.Contains(QueryLow))
        {
            Score = FMath::Max(Score, 200);
        }
    }

    // Category contains
    if (CategoryLow.Contains(QueryLow))
    {
        Score = FMath::Max(Score, 50);
    }

    // Keywords contains
    if (KeywordsLow.Contains(QueryLow))
    {
        Score = FMath::Max(Score, 50);
    }

    return Score;
}

} // namespace NiagaraSearch

// ---------------------------------------------------------------------------
// RPC Handlers
// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER(
    "niagara.graph.list_node_types",
    "niagara.graph",
    "Enumerate UNiagaraNode subclasses and their payload kinds.",
    RPC_NO_PARAMS)
{
    TArray<TSharedPtr<FJsonValue>> NodeTypes;

    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        if (!Class->IsChildOf(UNiagaraNode::StaticClass()))
        {
            continue;
        }
        if (Class->HasAnyClassFlags(CLASS_Abstract))
        {
            continue;
        }

        const FString ClassName   = Class->GetName();
        const FString DisplayName = Class->GetDisplayNameText().ToString();
        const FString PayloadKind = NiagaraSearch::PayloadKindFor(Class);
        const FString Category    = Class->GetMetaData(TEXT("Category"));

        TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
        NodeObj->SetStringField(TEXT("className"),   ClassName);
        NodeObj->SetStringField(TEXT("displayName"), DisplayName);
        NodeObj->SetStringField(TEXT("payloadKind"), PayloadKind);
        NodeObj->SetStringField(TEXT("category"),    Category);

        NodeTypes.Add(MakeShared<FJsonValueObject>(NodeObj));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("nodeTypes"), NodeTypes);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER(
    "niagara.graph.search_ops",
    "niagara.graph",
    "Search the Niagara op registry by name/category/keywords.",
    RPC_PARAMS(
        RPC_PARAM_OPT("query", "string",  "Search query"),
        RPC_PARAM_OPT("limit", "integer", "Max results (default 50; must be >= 0; 0 returns no rows; values above 500 clamp to 500)")))
{
    int32 Limit = 0;
    if (!NiagaraSearch::ResolveSearchLimit(Ctx, Limit))
    {
        return true;
    }

    // FNiagaraOpInfo::GetOpInfoArray / GetOpInfo are not NIAGARAEDITOR_API exported,
    // so the live op registry can't be read directly. search_ops and create_node share
    // one hand-mirrored snapshot — NiagaraOpCatalog — so the opName/signature this
    // returns round-trips into niagara.graph.create_node (caller picks an op, passes it
    // back, which validates + canonicalizes against the same catalog). Each entry
    // carries the bare leaf name (the `opName` form) and its engine category; the
    // signature is rebuilt as "Category::Leaf", the engine registry-key form.
    const TArray<NiagaraOpCatalog::FOpEntry>& OpCatalog = NiagaraOpCatalog::GetEntries();

    const FString Query = Ctx.GetString(TEXT("query"));

    struct FScoredOp
    {
        FString OpName;
        FString Category;
        int32   Score;
    };

    TArray<FScoredOp> Matches;
    Matches.Reserve(OpCatalog.Num());

    for (const NiagaraOpCatalog::FOpEntry& Entry : OpCatalog)
    {
        const FString OpName(Entry.Leaf);
        const FString Category(Entry.Category);

        const int32 Score = NiagaraSearch::ScoreOpMatch(
            Query, OpName, /*Alternate*/ FString(), Category, /*Keywords*/ FString());

        // When query is set, skip zero-score entries
        if (!Query.IsEmpty() && Score == 0)
        {
            continue;
        }

        Matches.Add({ OpName, Category, Score });
    }

    const int32 TotalMatches = Matches.Num();

    // Sort: score desc, then name asc
    Matches.Sort([](const FScoredOp& A, const FScoredOp& B)
    {
        if (A.Score != B.Score)
        {
            return A.Score > B.Score;
        }
        return A.OpName < B.OpName;
    });

    if (Matches.Num() > Limit)
    {
        Matches.SetNum(Limit);
    }

    TArray<TSharedPtr<FJsonValue>> Results;
    Results.Reserve(Matches.Num());

    for (const FScoredOp& M : Matches)
    {
        TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("opName"),    M.OpName);
        // Signature requires per-op type info that lives in the unexported FNiagaraOpInfo;
        // expose a name-only stub so callers can still group by category.
        Item->SetStringField(TEXT("signature"), NiagaraOpCatalog::MakeOpKey(M.Category, M.OpName));
        Item->SetStringField(TEXT("category"),  M.Category);
        Item->SetNumberField(TEXT("score"),     M.Score);
        Results.Add(MakeShared<FJsonValueObject>(Item));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("results"),      Results);
    Result->SetNumberField(TEXT("totalMatches"), TotalMatches);
    Ctx.SendSuccess(Result);
    return true;
}


// ---------------------------------------------------------------------------
// Module-search helpers (NiagaraSearch namespace; public helpers declared in NiagaraSearchHandler.h)
// ---------------------------------------------------------------------------

namespace NiagaraSearch
{

bool FilterByUsage(const FAssetData& AssetData, const FString& UsageName)
{
    FString TagValue;
    if (!AssetData.GetTagValue(GET_MEMBER_NAME_CHECKED(UNiagaraScript, Usage), TagValue))
    {
        return false;
    }
    return TagValue.Equals(UsageName, ESearchCase::IgnoreCase);
}

bool FilterByStageBitmask(const FAssetData& AssetData, ENiagaraScriptUsage Stage)
{
    FString BitmaskStr;
    if (!AssetData.GetTagValue(FName(TEXT("ModuleUsageBitmask")), BitmaskStr))
    {
        return false;
    }
    const int32 Bitmask = FCString::Atoi(*BitmaskStr);
    return UNiagaraScript::IsSupportedUsageContextForBitmask(Bitmask, Stage);
}

FString ClassifySource(const FString& PackagePath)
{
    if (PackagePath.StartsWith(TEXT("/Niagara/")))
    {
        return TEXT("engine");
    }
    if (PackagePath.StartsWith(TEXT("/Game/")))
    {
        return TEXT("project");
    }
    if (PackagePath.StartsWith(TEXT("/")) &&
        !PackagePath.StartsWith(TEXT("/Engine/")))
    {
        return TEXT("plugin");
    }
    return TEXT("unknown");
}

int32 ScoreModuleMatch(const FString& Query,
                       const FString& Name,
                       const FString& Description,
                       const FString& Keywords)
{
    if (Query.IsEmpty())
    {
        return 0;
    }

    const FString QueryLow  = Query.ToLower();
    const FString NameLow   = Name.ToLower();
    const FString DescLow   = Description.ToLower();
    const FString KwLow     = Keywords.ToLower();

    int32 Score = 0;

    // Name exact / prefix / contains
    if (NameLow == QueryLow)
    {
        Score = FMath::Max(Score, 1000);
    }
    else if (NameLow.StartsWith(QueryLow))
    {
        Score = FMath::Max(Score, 500);
    }
    else if (NameLow.Contains(QueryLow))
    {
        Score = FMath::Max(Score, 100);
    }

    // Token-level match on name
    if (Score < 50)
    {
        TArray<FString> Tokens;
        NameLow.ParseIntoArray(Tokens, TEXT(" "));
        for (const FString& Token : Tokens)
        {
            if (Token.Contains(QueryLow))
            {
                Score = FMath::Max(Score, 50);
                break;
            }
        }
    }

    // Description contains
    if (DescLow.Contains(QueryLow))
    {
        Score = FMath::Max(Score, 50);
    }

    // Keywords contains
    if (KwLow.Contains(QueryLow))
    {
        Score = FMath::Max(Score, 50);
    }

    return Score;
}

} // namespace NiagaraSearch (module-search additions)

// ---------------------------------------------------------------------------
// niagara.search_modules RPC handler
// ---------------------------------------------------------------------------

REGISTER_RPC_HANDLER(
    "niagara.search_modules",
    "niagara",
    "Search Niagara module scripts by usage, stage, and keywords.",
    RPC_PARAMS(
        RPC_PARAM_OPT("query",        "string", "Search query"),
        RPC_PARAM_REQ("usage",        "string", "Usage filter: Module|DynamicInput|Function|..."),
        RPC_PARAM_OPT("stage",        "string", "Stage filter: ParticleSpawn|ParticleUpdate|EmitterSpawn|EmitterUpdate|SystemSpawn|SystemUpdate"),
        RPC_PARAM_OPT("inputType",    "string", "Input type filter for DynamicInput usage"),
        RPC_PARAM_OPT("sourceFilter", "string", "engine|plugin|project|any (default any)"),
        RPC_PARAM_OPT("limit",        "integer", "Max results (default 50; must be >= 0; 0 returns no rows; values above 500 clamp to 500)")))
{
    // ---- Required: usage ----
    FString Usage;
    if (!Ctx.RequireString(TEXT("usage"), Usage))
    {
        return true;
    }

    int32 Limit = 0;
    if (!NiagaraSearch::ResolveSearchLimit(Ctx, Limit))
    {
        return true;
    }

    // ---- Optional params ----
    const FString Query        = Ctx.GetString(TEXT("query"));
    const FString StageStr     = Ctx.GetString(TEXT("stage"));
    FString       SourceFilter = Ctx.GetString(TEXT("sourceFilter"));
    if (SourceFilter.IsEmpty())
    {
        SourceFilter = TEXT("any");
    }

    // ---- Decode stage to enum (if provided) ----
    bool               bFilterStage  = !StageStr.IsEmpty();
    ENiagaraScriptUsage DecodedStage  = ENiagaraScriptUsage::Module; // default, overwritten below

    if (bFilterStage)
    {
        if (!NiagaraEdit::TryParseStackScriptUsageAlias(StageStr, DecodedStage))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_STAGE,
                FString::Printf(TEXT("Unknown stage '%s'. Valid values: ParticleSpawn, ParticleUpdate, EmitterSpawn, EmitterUpdate, SystemSpawn, SystemUpdate."), *StageStr));
            return true;
        }
    }

    // ---- Walk asset registry ----
    IAssetRegistry& AR = FAssetRegistryModule::GetRegistry();
    TArray<FAssetData> Assets;
    AR.GetAssetsByClass(UNiagaraScript::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses=*/false);

    struct FScoredAsset
    {
        int32      Score;
        FAssetData Asset;
        FString    Description;
        FString    Keywords;
        FString    Source;
    };

    TArray<FScoredAsset> Filtered;
    Filtered.Reserve(Assets.Num());

    for (const FAssetData& AssetData : Assets)
    {
        // Usage filter (required)
        if (!NiagaraSearch::FilterByUsage(AssetData, Usage))
        {
            continue;
        }

        // Stage bitmask filter (optional)
        if (bFilterStage && !NiagaraSearch::FilterByStageBitmask(AssetData, DecodedStage))
        {
            continue;
        }

        // Compute source once — needed for both the source filter and the output
        const FString Source = NiagaraSearch::ClassifySource(AssetData.PackagePath.ToString());

        // Source filter (optional, default "any")
        if (!SourceFilter.Equals(TEXT("any"), ESearchCase::IgnoreCase))
        {
            if (!Source.Equals(SourceFilter, ESearchCase::IgnoreCase))
            {
                continue;
            }
        }

        // Score against query — read tags once and cache them in FScoredAsset
        FString Description;
        AssetData.GetTagValue(FName(TEXT("Description")), Description);

        FString Keywords;
        AssetData.GetTagValue(FName(TEXT("Keywords")), Keywords);

        const int32 Score = NiagaraSearch::ScoreModuleMatch(
            Query,
            AssetData.AssetName.ToString(),
            Description,
            Keywords);

        // When query is set, skip zero-score results
        if (!Query.IsEmpty() && Score == 0)
        {
            continue;
        }

        Filtered.Add({ Score, AssetData, Description, Keywords, Source });
    }

    // ---- totalMatches (before limit) ----
    const int32 TotalMatches = Filtered.Num();

    // ---- Sort: score desc, then name asc ----
    Filtered.Sort([](const FScoredAsset& A, const FScoredAsset& B)
    {
        if (A.Score != B.Score)
        {
            return A.Score > B.Score;
        }
        return A.Asset.AssetName.ToString() < B.Asset.AssetName.ToString();
    });

    // ---- Trim to limit ----
    if (Filtered.Num() > Limit)
    {
        Filtered.SetNum(Limit);
    }

    // ---- Build result array ----
    TArray<TSharedPtr<FJsonValue>> Results;
    Results.Reserve(Filtered.Num());

    for (const FScoredAsset& Entry : Filtered)
    {
        const FAssetData& AssetData = Entry.Asset;

        // Description and Source are cached in FScoredAsset from the filter pass
        const FString& Description = Entry.Description;

        // Usage tag (string)
        FString UsageTag;
        AssetData.GetTagValue(GET_MEMBER_NAME_CHECKED(UNiagaraScript, Usage), UsageTag);

        // ModuleUsageBitmask → validStages array
        TArray<TSharedPtr<FJsonValue>> ValidStagesArr;
        FString BitmaskStr;
        if (AssetData.GetTagValue(FName(TEXT("ModuleUsageBitmask")), BitmaskStr))
        {
            const int32 Bitmask = FCString::Atoi(*BitmaskStr);
            TArray<ENiagaraScriptUsage> SupportedUsages =
                UNiagaraScript::GetSupportedUsageContextsForBitmask(Bitmask);
            for (ENiagaraScriptUsage SupportedUsage : SupportedUsages)
            {
                ValidStagesArr.Add(MakeShared<FJsonValueString>(
                    NiagaraEdit::StackScriptUsageToString(SupportedUsage)));
            }
        }

        // Source classification (cached from filter pass)
        const FString& Source = Entry.Source;

        TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("assetPath"),   AssetData.GetObjectPathString());
        Item->SetStringField(TEXT("name"),        AssetData.AssetName.ToString());
        Item->SetStringField(TEXT("description"), Description);
        Item->SetStringField(TEXT("usage"),       UsageTag);
        Item->SetArrayField(TEXT("validStages"),  ValidStagesArr);
        // inputs/outputs deferred (requires per-asset load) — left as empty arrays for v1
        Item->SetArrayField(TEXT("inputs"),  TArray<TSharedPtr<FJsonValue>>());
        Item->SetArrayField(TEXT("outputs"), TArray<TSharedPtr<FJsonValue>>());
        Item->SetStringField(TEXT("source"),      Source);
        Item->SetNumberField(TEXT("score"),        Entry.Score);

        Results.Add(MakeShared<FJsonValueObject>(Item));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("results"),       Results);
    Result->SetNumberField(TEXT("totalMatches"),  TotalMatches);
    Ctx.SendSuccess(Result);
    return true;
}
