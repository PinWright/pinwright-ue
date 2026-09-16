// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintInspectHandler.cpp - All-in-one Blueprint inspection
// Returns metadata, variables, functions, events, components, graphs, references, and performance warnings.
// For pseudocode decompilation use blueprint.decompile / blueprint.decompile_function instead.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Utils/AssetDumpSuggestion.h"
#include "Utils/AssetUtils.h"
#include "Utils/PropertyUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "HAL/FileManager.h"
#include "UObject/Package.h"

using namespace BlueprintHandlerUtils;

// ---- blueprint.inspect ----
REGISTER_RPC_HANDLER("blueprint.inspect", "blueprint",
    "Blueprint summary: metadata, variables, functions, events, components, graphs, references, and warnings. For pseudocode decompilation use blueprint.decompile.",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_DEF("includeReferences", "bool", "Include asset dependencies", "true"),
        RPC_PARAM_DEF("includeScriptRefs", "bool", "Include /Script/ references (filtered by default)", "false"),
        RPC_PARAM_DEF("includeProperties", "bool", "Include sparse CDO property diff with inheritance tags", "false")
    ))
{
    FString AssetPath = ResolveBlueprintPath(Ctx);
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"),
            TEXT("blueprint.inspect: a blueprint path is required (use 'path' or 'assetPath')."));
        return true;
    }

    const bool bIncludeReferences = Ctx.GetBool(TEXT("includeReferences"), true);
    const bool bIncludeScriptRefs = Ctx.GetBool(TEXT("includeScriptRefs"), false);
    const bool bIncludeProperties = Ctx.GetBool(TEXT("includeProperties"), false);

    FString Normalized, LoadErr;
    UBlueprint* BP = LoadBlueprintAsset(AssetPath, Normalized, LoadErr);
    if (!BP)
    {
        Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"),
            LoadErr.IsEmpty() ? TEXT("Failed to load Blueprint asset") : *LoadErr);
        return true;
    }

    const FString ResolvedPath = Normalized.IsEmpty() ? AssetPath : Normalized;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

    // ---- Basic metadata ----
    Result->SetStringField(TEXT("assetPath"), ResolvedPath);

    if (BP->ParentClass)
        Result->SetStringField(TEXT("parentClass"), BP->ParentClass->GetName());

    // Walk up to first native (non-Blueprint-compiled) class
    UClass* NativeParent = BP->ParentClass;
    while (NativeParent && NativeParent->HasAnyClassFlags(CLASS_CompiledFromBlueprint))
        NativeParent = NativeParent->GetSuperClass();
    if (NativeParent)
        Result->SetStringField(TEXT("nativeParentClass"), NativeParent->GetName());

    // Blueprint type as readable string
    FString BlueprintTypeStr;
    switch (BP->BlueprintType)
    {
    case BPTYPE_Normal:       BlueprintTypeStr = TEXT("Normal");      break;
    case BPTYPE_Const:        BlueprintTypeStr = TEXT("Const");       break;
    case BPTYPE_MacroLibrary: BlueprintTypeStr = TEXT("MacroLibrary"); break;
    case BPTYPE_Interface:    BlueprintTypeStr = TEXT("Interface");   break;
    case BPTYPE_LevelScript:  BlueprintTypeStr = TEXT("LevelScript"); break;
    case BPTYPE_FunctionLibrary: BlueprintTypeStr = TEXT("FunctionLibrary"); break;
    default:                  BlueprintTypeStr = TEXT("Unknown");     break;
    }
    Result->SetStringField(TEXT("blueprintType"), BlueprintTypeStr);

    // Implemented interfaces
    TArray<TSharedPtr<FJsonValue>> InterfaceNames;
    for (const FBPInterfaceDescription& Iface : BP->ImplementedInterfaces)
    {
        if (Iface.Interface)
            InterfaceNames.Add(MakeShared<FJsonValueString>(Iface.Interface->GetName()));
    }
    Result->SetArrayField(TEXT("interfaces"), InterfaceNames);

    // File size in KB
    FString PackageFilename;
    if (FPackageName::DoesPackageExist(BP->GetOutermost()->GetName(), &PackageFilename))
    {
        const int64 SizeBytes = IFileManager::Get().FileSize(*PackageFilename);
        if (SizeBytes >= 0)
            Result->SetNumberField(TEXT("fileSizeKB"), SizeBytes / 1024.0);
    }

    // ---- Existing data via shared utils ----
    Result->SetArrayField(TEXT("variables"), CollectBlueprintVariables(BP));
    Result->SetArrayField(TEXT("functions"), CollectBlueprintFunctions(BP));
    Result->SetArrayField(TEXT("events"), CollectBlueprintEvents(BP));

    // ---- Components from SimpleConstructionScript ----
    TArray<TSharedPtr<FJsonValue>> ComponentsArray;
    if (BP->SimpleConstructionScript)
    {
        for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
        {
            if (!Node) continue;
            TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
            CompObj->SetStringField(TEXT("componentClass"),
                Node->ComponentClass ? Node->ComponentClass->GetName() : TEXT("Unknown"));
            CompObj->SetStringField(TEXT("variableName"), Node->GetVariableName().ToString());
            ComponentsArray.Add(MakeShared<FJsonValueObject>(CompObj));
        }
    }
    Result->SetArrayField(TEXT("components"), ComponentsArray);

    // ---- Graphs (ubergraph pages + function graphs + composite subgraphs) ----
    // Collect once and reuse for both the graphs JSON array and the tick-warning scan below.
    const TArray<UEdGraph*> AllBPGraphs = BlueprintHandlerUtils::CollectAllBlueprintGraphsRecursive(BP);

    TArray<TSharedPtr<FJsonValue>> GraphsArray;
    for (UEdGraph* Graph : AllBPGraphs)
    {
        if (!Graph) continue;
        TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
        GraphObj->SetStringField(TEXT("name"), Graph->GetName());
        GraphObj->SetNumberField(TEXT("nodeCount"), Graph->Nodes.Num());
        GraphsArray.Add(MakeShared<FJsonValueObject>(GraphObj));
    }
    Result->SetArrayField(TEXT("graphs"), GraphsArray);

    // ---- Performance warnings ----
    TArray<TSharedPtr<FJsonValue>> Warnings;

    // Check for Tick usage across all graphs (including nested composite subgraphs —
    // Event Tick placed inside a collapsed graph still fires every frame).
    auto CheckGraphForTick = [&](UEdGraph* Graph)
    {
        if (!Graph) return;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node) continue;
            const FString NodeClass = Node->GetClass()->GetName();
            if (NodeClass.Contains(TEXT("K2Node_Event")))
            {
                const FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::MenuTitle).ToString();
                const bool bIsTick = NodeTitle.Contains(TEXT("ReceiveTick"))
                    || NodeTitle.Contains(TEXT("Event Tick"));
                if (bIsTick)
                {
                    bool bHasConnectedExec = false;
                    for (UEdGraphPin* Pin : Node->Pins)
                    {
                        if (Pin && Pin->Direction == EGPD_Output
                            && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
                            && Pin->LinkedTo.Num() > 0)
                        {
                            bHasConnectedExec = true;
                            break;
                        }
                    }
                    if (bHasConnectedExec)
                    {
                        Warnings.Add(MakeShared<FJsonValueString>(
                            TEXT("Blueprint uses Tick event — consider if per-frame updates are required")));
                    }
                }
            }
        }
    };

    for (UEdGraph* Graph : AllBPGraphs)
    {
        CheckGraphForTick(Graph);
    }

    // Check for unusually large variable count
    if (BP->NewVariables.Num() > 50)
    {
        Warnings.Add(MakeShared<FJsonValueString>(
            FString::Printf(TEXT("Blueprint has %d variables — unusually high, consider refactoring"),
                BP->NewVariables.Num())));
    }

    Result->SetArrayField(TEXT("performanceWarnings"), Warnings);

    if (bIncludeProperties)
    {
        UObject* CDO = nullptr;
        UObject* ParentCDO = nullptr;
        if (BP->GeneratedClass)
        {
            CDO = BP->GeneratedClass->GetDefaultObject();
            if (UClass* SuperClass = BP->GeneratedClass->GetSuperClass())
            {
                ParentCDO = SuperClass->GetDefaultObject();
            }
        }

        Result->SetObjectField(TEXT("properties"),
            CDO ? BuildClassPropertyJson(CDO, ParentCDO) : MakeShared<FJsonObject>());
    }

    // ---- References (conditional) ----
    if (bIncludeReferences)
    {
        IAssetRegistry& AssetRegistry =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

        TArray<FName> Dependencies;
        FAssetIdentifier Identifier(BP->GetOutermost()->GetFName());
        TArray<FAssetDependency> DependencyList;
        AssetRegistry.GetDependencies(Identifier, DependencyList,
            UE::AssetRegistry::EDependencyCategory::Package);
        for (const FAssetDependency& Dep : DependencyList)
            Dependencies.Add(Dep.AssetId.PackageName);

        TArray<TSharedPtr<FJsonValue>> RefsArray;
        for (const FName& DepName : Dependencies)
        {
            const FString DepStr = DepName.ToString();
            if (!bIncludeScriptRefs && DepStr.StartsWith(TEXT("/Script/")))
                continue;
            RefsArray.Add(MakeShared<FJsonValueString>(DepStr));
        }
        Result->SetArrayField(TEXT("references"), RefsArray);

        // ---- Referenced By (referencers, depth 1) ----
        TArray<FName> Referencers;
        TArray<FAssetDependency> ReferencerList;
        AssetRegistry.GetReferencers(Identifier, ReferencerList,
            UE::AssetRegistry::EDependencyCategory::Package);
        for (const FAssetDependency& Ref : ReferencerList)
            Referencers.Add(Ref.AssetId.PackageName);

        TArray<TSharedPtr<FJsonValue>> ReferencedByArray;
        for (const FName& RefName : Referencers)
        {
            const FString RefStr = RefName.ToString();
            if (!bIncludeScriptRefs && RefStr.StartsWith(TEXT("/Script/")))
                continue;
            ReferencedByArray.Add(MakeShared<FJsonValueString>(RefStr));
        }
        Result->SetArrayField(TEXT("referencedBy"), ReferencedByArray);
    }

    if (FString DumpHint = AssetDumpSuggestion::BuildDumpSuggestionHint(ResolvedPath, AssetDumpSuggestion::EDumpSubjectKind::Asset); !DumpHint.IsEmpty())
    {
        Result->SetStringField(TEXT("hint"), DumpHint);
    }

    Ctx.SendSuccess(Result);
    return true;
}
