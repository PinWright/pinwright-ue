// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"

#if defined(__has_include) && __has_include("PCGGraph.h")

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UObjectGlobals.h"

#include "Compat/EngineVersionCompat.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "Elements/PCGSelfPruning.h"
#include "Handlers/PCG/PCGHandlerHelpers.h"

// UE 5.4 moved the self-pruning knobs (PruningType / RadiusSimilarityFactor / bRandomizedPruning)
// off UPCGSelfPruningSettings directly into a nested FPCGSelfPruningParameters `Parameters` member.
// On 5.3 the knobs live flat on the settings object. This macro selects the right container.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
#define PW_PCG_SELFPRUNE_PARAMS(Settings) ((Settings)->Parameters)
#else
#define PW_PCG_SELFPRUNE_PARAMS(Settings) (*(Settings))
#endif

namespace
{
    bool ParsePruningType(const FString& Name, EPCGSelfPruningType& Out)
    {
        if (Name == TEXT("LargeToSmall"))     { Out = EPCGSelfPruningType::LargeToSmall;     return true; }
        if (Name == TEXT("SmallToLarge"))     { Out = EPCGSelfPruningType::SmallToLarge;     return true; }
        if (Name == TEXT("AllEqual"))         { Out = EPCGSelfPruningType::AllEqual;         return true; }
        if (Name == TEXT("None"))             { Out = EPCGSelfPruningType::None;             return true; }
        if (Name == TEXT("RemoveDuplicates")) { Out = EPCGSelfPruningType::RemoveDuplicates; return true; }
        return false;
    }
}

REGISTER_RPC_HANDLER("pcg.set_self_pruning_settings", "pcg",
    "Write typed knobs into a UPCGSelfPruningSettings node's nested Parameters struct "
    "(PruningType / RadiusSimilarityFactor / bRandomizedPruning).",
    RPC_PARAMS(
        RPC_PARAM_REQ("graphPath", "path", "PCG graph asset path."),
        RPC_PARAM_REQ("nodeId", "string", "Self-pruning node id (UPCGNode::GetName())."),
        RPC_PARAM_OPT("pruningType", "string", "LargeToSmall|SmallToLarge|AllEqual|None|RemoveDuplicates."),
        RPC_PARAM_OPT("radiusSimilarityFactor", "number", "Similarity factor for radius comparison."),
        RPC_PARAM_OPT("bRandomizedPruning", "bool", "Enable randomized pruning order.")
    ))
{
    FString GraphPath;
    if (!Ctx.RequireString(TEXT("graphPath"), GraphPath)) return true;

    FString NodeId;
    if (!Ctx.RequireString(TEXT("nodeId"), NodeId)) return true;

    UPCGGraph* Graph = PinWrightPCG::LoadGraphOrError(Ctx, GraphPath);
    if (!Graph) return true;

    UPCGNode* Node = PinWrightPCG::FindNodeByName(Graph, NodeId);
    if (!Node)
    {
        Ctx.SendError(TEXT("NODE_NOT_FOUND"),
            FString::Printf(TEXT("Could not find node: %s"), *NodeId));
        return true;
    }

    UPCGSelfPruningSettings* Settings = Cast<UPCGSelfPruningSettings>(Node->GetSettings());
    if (!Settings)
    {
        Ctx.SendError(TEXT("WRONG_NODE_TYPE"),
            FString::Printf(TEXT("Node %s is not a UPCGSelfPruningSettings node"), *NodeId));
        return true;
    }

    const FString PruningTypeStr = Ctx.GetString(TEXT("pruningType"), FString());
    if (!PruningTypeStr.IsEmpty())
    {
        EPCGSelfPruningType ParsedType;
        if (!ParsePruningType(PruningTypeStr, ParsedType))
        {
            Ctx.SendError(TEXT("INVALID_PRUNING_TYPE"),
                FString::Printf(TEXT("Unknown pruningType: %s"), *PruningTypeStr));
            return true;
        }
        PW_PCG_SELFPRUNE_PARAMS(Settings).PruningType = ParsedType;
    }

    const TSharedPtr<FJsonValue> RadiusVal = Ctx.GetJsonValueFirstOf({TEXT("radiusSimilarityFactor")});
    if (RadiusVal.IsValid() && RadiusVal->Type != EJson::Null)
    {
        PW_PCG_SELFPRUNE_PARAMS(Settings).RadiusSimilarityFactor = static_cast<float>(RadiusVal->AsNumber());
    }

    const TSharedPtr<FJsonValue> RandVal = Ctx.GetJsonValueFirstOf({TEXT("bRandomizedPruning")});
    if (RandVal.IsValid() && RandVal->Type != EJson::Null)
    {
        PW_PCG_SELFPRUNE_PARAMS(Settings).bRandomizedPruning = RandVal->AsBool();
    }

#if WITH_EDITOR
    Settings->PostEditChange();
#endif
    Graph->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("nodeId"), NodeId);
    Ctx.SendSuccess(Result);
    return true;
}

#endif // __has_include("PCGGraph.h")
