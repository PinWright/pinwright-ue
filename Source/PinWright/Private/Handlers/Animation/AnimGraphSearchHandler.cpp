// Copyright (c) 2026 Alexander Penkin. MIT License.

// AnimGraphSearchHandler.cpp - Discovery RPCs for UAnimGraphNode_* classes.
//
// Catalog + keyword search over UAnimGraphNode_Base descendants. Mirrors the
// `blueprint.graph.list_node_types` live-iteration pattern (no cache). Returns
// className, runtimeNode (wrapped FAnimNode_* struct), category, description,
// and required asset class names. inputPins / outputPins deferred — would
// require AllocateDefaultPins on transient instances.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"

#include "AnimGraphNode_Base.h"
#include "Handlers/Animation/AnimGraphNodeAccessor.h"
#include "Animation/AnimationAsset.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace
{
    struct FAnimNodeCatalogEntry
    {
        FString ClassName;        // e.g. "AnimGraphNode_SequencePlayer"
        FString RuntimeNode;      // e.g. "FAnimNode_SequencePlayer"
        FString Category;
        FString Description;
        TArray<FString> RequiredAssets;
    };

    FAnimNodeCatalogEntry BuildEntry(UClass* C)
    {
        FAnimNodeCatalogEntry E;
        E.ClassName = C->GetName();
        UAnimGraphNode_Base* CDO = Cast<UAnimGraphNode_Base>(C->GetDefaultObject());
        if (CDO)
        {
            E.Category = CDO->GetMenuCategory().ToString();
            // Use MenuTitle, not ListView. On a CDO (no graph outer), some node
            // types' ListView title routes through FNodeTextCache ->
            // UEdGraphNode::GetGraph(), which ensures ("does not have a UEdGraph
            // as an Outer") and writes a crash dump (e.g. AnimGraphNode_SaveCachedPose).
            // The MenuTitle branch short-circuits to a graph-independent label for
            // every stock UAnimGraphNode_* in its default (CDO) state, so it never
            // reaches GetGraph(); fall back to the class display name if a node
            // yields an empty menu title.
            E.Description = CDO->GetNodeTitle(ENodeTitleType::MenuTitle).ToString();
            if (E.Description.IsEmpty())
            {
                E.Description = C->GetDisplayNameText().ToString();
            }
            if (FStructProperty* P = PinWright::Anim::GetFNodeProperty(CDO))
            {
                E.RuntimeNode = FString::Printf(TEXT("F%s"), *P->Struct->GetName());
                for (TFieldIterator<FObjectProperty> It(P->Struct); It; ++It)
                {
                    FObjectProperty* OP = *It;
                    if (OP && OP->PropertyClass && OP->PropertyClass->IsChildOf(UAnimationAsset::StaticClass()))
                    {
                        E.RequiredAssets.AddUnique(OP->PropertyClass->GetName());
                    }
                }
            }
        }
        return E;
    }

    TSharedPtr<FJsonObject> EntryToJson(const FAnimNodeCatalogEntry& E, double Score)
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("className"), E.ClassName);
        O->SetStringField(TEXT("runtimeNode"), E.RuntimeNode);
        O->SetStringField(TEXT("category"), E.Category);
        O->SetStringField(TEXT("description"), E.Description);
        TArray<TSharedPtr<FJsonValue>> Assets;
        for (const FString& A : E.RequiredAssets) { Assets.Add(MakeShared<FJsonValueString>(A)); }
        O->SetArrayField(TEXT("requiredAssets"), Assets);
        if (Score >= 0.0) { O->SetNumberField(TEXT("score"), Score); }
        return O;
    }

    void CollectCatalog(bool bIncludeAbstract, TArray<FAnimNodeCatalogEntry>& Out)
    {
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* C = *It;
            if (!C || !C->IsChildOf(UAnimGraphNode_Base::StaticClass())) continue;
            if (C == UAnimGraphNode_Base::StaticClass()) continue;
            if (!bIncludeAbstract && C->HasAnyClassFlags(CLASS_Abstract)) continue;
            Out.Add(BuildEntry(C));
        }
    }
}

REGISTER_RPC_HANDLER("animation.list_graph_nodes", "animation",
    "Enumerate every UAnimGraphNode_* class registered in the editor. Returns class name, wrapped FAnimNode_* runtime struct, menu category, list-view description, and the asset classes the node binds at runtime. Pair with animation.search_graph_nodes for keyword ranking.",
    RPC_PARAMS(
        RPC_PARAM_OPT("includeAbstract", "boolean", "Include abstract UAnimGraphNode_* classes (default false)"),
        RPC_PARAM_OPT("limit", "number", "Max results (0=unlimited)")
    ))
{
    bool bAbs = Ctx.GetBool(TEXT("includeAbstract"), false);
    int32 Limit = Ctx.GetInt(TEXT("limit"), 0);

    TArray<FAnimNodeCatalogEntry> Entries;
    CollectCatalog(bAbs, Entries);
    Entries.Sort([](const FAnimNodeCatalogEntry& A, const FAnimNodeCatalogEntry& B)
    {
        return A.ClassName < B.ClassName;
    });

    const int32 TotalMatches = Entries.Num();
    if (Limit > 0 && Entries.Num() > Limit)
    {
        Entries.SetNum(Limit);
    }

    TArray<TSharedPtr<FJsonValue>> Results;
    Results.Reserve(Entries.Num());
    for (const FAnimNodeCatalogEntry& E : Entries)
    {
        Results.Add(MakeShared<FJsonValueObject>(EntryToJson(E, -1.0)));
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetArrayField(TEXT("results"), Results);
    Out->SetNumberField(TEXT("totalMatches"), TotalMatches);
    Ctx.SendSuccess(Out);
    return true;
}

REGISTER_RPC_HANDLER("animation.search_graph_nodes", "animation",
    "Rank UAnimGraphNode_* classes by keyword match across className / category / description. Optional category and requiresAsset filters apply substring matches before scoring. Empty query is rejected.",
    RPC_PARAMS(
        RPC_PARAM_REQ("query", "string", "Keyword(s) to match; whitespace-tokenized, case-insensitive substring scoring"),
        RPC_PARAM_OPT("category", "string", "Restrict to nodes whose menu category contains this substring (case-insensitive)"),
        RPC_PARAM_OPT("requiresAsset", "string", "Restrict to nodes whose requiredAssets includes a class name containing this substring (case-insensitive)"),
        RPC_PARAM_OPT("includeAbstract", "boolean", "Include abstract UAnimGraphNode_* classes (default false)"),
        RPC_PARAM_DEF("limit", "number", "Max results after scoring/filtering (default 20)", "20")
    ))
{
    FString Query = Ctx.GetString(TEXT("query"));
    if (Query.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_QUERY"), TEXT("query is required"));
        return true;
    }

    FString CategoryFilter = Ctx.GetString(TEXT("category"));
    FString RequiresAsset = Ctx.GetString(TEXT("requiresAsset"));
    bool bAbs = Ctx.GetBool(TEXT("includeAbstract"), false);
    int32 Limit = Ctx.GetInt(TEXT("limit"), 20);

    const FString CategoryLower = CategoryFilter.ToLower();
    const FString RequiresAssetLower = RequiresAsset.ToLower();

    TArray<FString> Tokens;
    Query.ToLower().ParseIntoArrayWS(Tokens);

    TArray<FAnimNodeCatalogEntry> Entries;
    CollectCatalog(bAbs, Entries);

    struct FScored { FAnimNodeCatalogEntry Entry; double Score; };
    TArray<FScored> Scored;
    Scored.Reserve(Entries.Num());

    for (const FAnimNodeCatalogEntry& E : Entries)
    {
        if (!CategoryLower.IsEmpty() && !E.Category.ToLower().Contains(CategoryLower))
        {
            continue;
        }
        if (!RequiresAssetLower.IsEmpty())
        {
            bool bAssetMatch = false;
            for (const FString& A : E.RequiredAssets)
            {
                if (A.ToLower().Contains(RequiresAssetLower)) { bAssetMatch = true; break; }
            }
            if (!bAssetMatch) continue;
        }

        const FString Haystack = E.ClassName.ToLower()
            + TEXT(" ") + E.Category.ToLower()
            + TEXT(" ") + E.Description.ToLower();

        double Score = 0.0;
        for (const FString& T : Tokens)
        {
            if (T.IsEmpty()) continue;
            int32 SearchFrom = 0;
            while (SearchFrom < Haystack.Len())
            {
                const int32 Idx = Haystack.Find(T, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
                if (Idx == INDEX_NONE) break;
                Score += 1.0;
                SearchFrom = Idx + T.Len();
            }
        }

        if (Score > 0.0)
        {
            Scored.Add({E, Score});
        }
    }

    Scored.Sort([](const FScored& A, const FScored& B)
    {
        if (A.Score != B.Score) return A.Score > B.Score;
        return A.Entry.ClassName < B.Entry.ClassName;
    });

    const int32 TotalMatches = Scored.Num();
    if (Limit > 0 && Scored.Num() > Limit)
    {
        Scored.SetNum(Limit);
    }

    TArray<TSharedPtr<FJsonValue>> Results;
    Results.Reserve(Scored.Num());
    for (const FScored& S : Scored)
    {
        Results.Add(MakeShared<FJsonValueObject>(EntryToJson(S.Entry, S.Score)));
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetArrayField(TEXT("results"), Results);
    Out->SetNumberField(TEXT("totalMatches"), TotalMatches);
    Ctx.SendSuccess(Out);
    return true;
}
