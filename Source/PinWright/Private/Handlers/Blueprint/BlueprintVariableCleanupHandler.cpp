// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintVariableCleanupHandler.cpp - Find and remove unused Blueprint variables

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Dom/JsonObject.h"

#include "EditorAssetLibrary.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "ScopedTransaction.h"

using namespace BlueprintHandlerUtils;

static bool IsVariableReferencedInGraphs(UBlueprint* Blueprint, const FName& VarName)
{
    // Collect all graphs to search
    TArray<UEdGraph*> AllGraphs;
    AllGraphs.Append(Blueprint->UbergraphPages);
    AllGraphs.Append(Blueprint->FunctionGraphs);
    AllGraphs.Append(Blueprint->MacroGraphs);

    // Also search subgraphs
    TArray<UEdGraph*> SubGraphs;
    for (UEdGraph* Graph : AllGraphs)
    {
        if (Graph)
        {
            Graph->GetAllChildrenGraphs(SubGraphs);
        }
    }
    AllGraphs.Append(SubGraphs);

    for (UEdGraph* Graph : AllGraphs)
    {
        if (!Graph)
        {
            continue;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }
            if (UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(Node))
            {
                if (VarNode->GetVarName() == VarName)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

// ---- blueprint.delete_unused_variables ----
REGISTER_RPC_HANDLER("blueprint.delete_unused_variables", "blueprint",
    "Find and optionally delete unused member variables in a Blueprint",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("path"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_DEF("dryRun", "boolean", "If true, only report unused variables without deleting (default: true)", "true")
    ))
{
    FString Path = ResolveBlueprintPath(Ctx);
    if (Path.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"),
            TEXT("blueprint.delete_unused_variables requires a blueprint path."));
        return true;
    }

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    const bool bDryRun = !Payload->HasField(TEXT("dryRun"))
        || GetJsonBoolField(Payload, TEXT("dryRun"), true);

    FString Normalized, LoadErr;
    UBlueprint* Blueprint = LoadBlueprintAsset(Path, Normalized, LoadErr);
    const FString RegistryKey = !Normalized.IsEmpty() ? Normalized : Path;
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"),
            LoadErr.IsEmpty() ? TEXT("Failed to load blueprint") : *LoadErr);
        return true;
    }

    TArray<FString> UnusedVarNames;
    const int32 TotalChecked = Blueprint->NewVariables.Num();

    for (const FBPVariableDescription& VarDesc : Blueprint->NewVariables)
    {
        if (!IsVariableReferencedInGraphs(Blueprint, VarDesc.VarName))
        {
            UnusedVarNames.Add(VarDesc.VarName.ToString());
        }
    }

    int32 DeletedCount = 0;
    FBlueprintCompileDiagnostics Diagnostics;
    bool bCompileAttempted = false;
    if (!bDryRun && UnusedVarNames.Num() > 0)
    {
        FScopedTransaction Transaction(FText::FromString(FString::Printf(
            TEXT("Delete %d Unused Variables"), UnusedVarNames.Num())));
        Blueprint->Modify();

        for (const FString& VarName : UnusedVarNames)
        {
            FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, FName(*VarName));
            DeletedCount++;
        }

        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        Diagnostics = CompileBlueprintWithDiagnostics(Blueprint);
        bCompileAttempted = true;
        SaveLoadedAssetThrottled(Blueprint);
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("blueprintPath"), RegistryKey);
    Resp->SetBoolField(TEXT("dryRun"), bDryRun);
    Resp->SetNumberField(TEXT("totalChecked"), TotalChecked);
    Resp->SetNumberField(TEXT("deletedCount"), DeletedCount);

    TArray<TSharedPtr<FJsonValue>> UnusedArray;
    for (const FString& Name : UnusedVarNames)
    {
        UnusedArray.Add(MakeShared<FJsonValueString>(Name));
    }
    Resp->SetArrayField(TEXT("unusedVariables"), UnusedArray);

    if (bCompileAttempted)
    {
        AddCompileDiagnosticsToJson(Diagnostics, Resp);
    }

    AddAssetVerification(Resp, Blueprint);
    Ctx.SendSuccess(Resp);
    return true;
}
