// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "State/PluginState.h"
#include "Utils/AssetUtils.h"
#include "Utils/ClassUtils.h"

#include "EdGraph/EdGraph.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopeExit.h"
#include "ScopedTransaction.h"

using namespace BlueprintHandlerUtils;

namespace
{
    bool SupportsImplementedInterfaces(const UBlueprint* Blueprint)
    {
        return Blueprint
            && Blueprint->ParentClass
            && (Blueprint->BlueprintType == BPTYPE_Normal || Blueprint->BlueprintType == BPTYPE_Const);
    }

    UClass* ResolveInterfaceClass(const FString& InterfaceSpec)
    {
        const FString CleanSpec = InterfaceSpec.TrimStartAndEnd();
        if (CleanSpec.IsEmpty())
        {
            return nullptr;
        }

        if (UClass* Resolved = ResolveUClass(CleanSpec))
        {
            return Resolved;
        }

        if (!CleanSpec.EndsWith(TEXT("_C")))
        {
            return ResolveUClass(CleanSpec + TEXT("_C"));
        }

        return nullptr;
    }

    const FBPInterfaceDescription* FindImplementedInterface(
        const UBlueprint* Blueprint,
        const UClass* InterfaceClass)
    {
        if (!Blueprint || !InterfaceClass)
        {
            return nullptr;
        }

        const FTopLevelAssetPath InterfacePath = InterfaceClass->GetClassPathName();
        for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
        {
            if (Description.Interface == InterfaceClass
                || (Description.Interface && Description.Interface->GetClassPathName() == InterfacePath))
            {
                return &Description;
            }
        }
        return nullptr;
    }

    TArray<TSharedPtr<FJsonValue>> BuildInterfaceGraphNames(const FBPInterfaceDescription* Description)
    {
        TArray<TSharedPtr<FJsonValue>> GraphNames;
        if (!Description)
        {
            return GraphNames;
        }

        for (UEdGraph* Graph : Description->Graphs)
        {
            if (Graph)
            {
                GraphNames.Add(MakeShared<FJsonValueString>(Graph->GetName()));
            }
        }
        return GraphNames;
    }

    bool HandleBlueprintInterfaceMutation(FHandlerContext& Ctx, bool bAddInterface)
    {
        FString Path = ResolveBlueprintPath(Ctx);
        if (Path.IsEmpty())
        {
            Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"),
                bAddInterface
                    ? TEXT("blueprint.add_interface requires a blueprint path.")
                    : TEXT("blueprint.remove_interface requires a blueprint path."));
            return true;
        }

        FString InterfaceSpec;
        if (!Ctx.RequireString(TEXT("interfaceClass"), InterfaceSpec))
        {
            return true;
        }

        const bool bAutoCompile = Ctx.GetBool(TEXT("autoCompile"), true);
        const bool bSave = Ctx.GetBool(TEXT("save"), false);

        if (FPluginState::Get().Blueprints().IsBusy(Path))
        {
            Ctx.SendError(TEXT("BLUEPRINT_BUSY"), TEXT("Blueprint is busy"));
            return true;
        }

        FPluginState::Get().Blueprints().MarkBusy(Path);
        ON_SCOPE_EXIT
        {
            if (FPluginState::Get().Blueprints().IsBusy(Path))
            {
                FPluginState::Get().Blueprints().ClearBusy(Path);
            }
        };

        FString Normalized;
        FString LoadErr;
        UBlueprint* Blueprint = LoadBlueprintAsset(Path, Normalized, LoadErr);
        if (!Blueprint)
        {
            Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"),
                LoadErr.IsEmpty() ? TEXT("Failed to load blueprint") : *LoadErr);
            return true;
        }

        const FString RegistryKey = Normalized.IsEmpty() ? Path : Normalized;
        if (!SupportsImplementedInterfaces(Blueprint))
        {
            Ctx.SendError(TEXT("INVALID_BLUEPRINT_TYPE"),
                TEXT("Target Blueprint type does not support implemented interfaces."));
            return true;
        }

        UClass* InterfaceClass = ResolveInterfaceClass(InterfaceSpec);
        if (!InterfaceClass)
        {
            Ctx.SendError(TEXT("CLASS_NOT_FOUND"),
                FString::Printf(TEXT("Could not resolve interfaceClass: %s"), *InterfaceSpec));
            return true;
        }
        if (!FKismetEditorUtilities::IsClassABlueprintImplementableInterface(InterfaceClass))
        {
            Ctx.SendError(TEXT("INVALID_INTERFACE_CLASS"),
                FString::Printf(TEXT("Class is not a Blueprint-implementable interface: %s"),
                    *InterfaceClass->GetPathName()));
            return true;
        }
        if (bAddInterface && !FKismetEditorUtilities::CanBlueprintImplementInterface(Blueprint, InterfaceClass))
        {
            Ctx.SendError(TEXT("INVALID_INTERFACE_CLASS"),
                FString::Printf(TEXT("Blueprint cannot implement interface: %s"),
                    *InterfaceClass->GetPathName()));
            return true;
        }

        const FTopLevelAssetPath InterfacePath = InterfaceClass->GetClassPathName();
        const bool bAlreadyImplemented = FindImplementedInterface(Blueprint, InterfaceClass) != nullptr;
        const bool bChanged = bAddInterface ? !bAlreadyImplemented : bAlreadyImplemented;

        bool bMutationSucceeded = true;
        if (bChanged)
        {
            if (bAddInterface)
            {
                FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.add_interface")));
                Blueprint->Modify();
                bMutationSucceeded = FBlueprintEditorUtils::ImplementNewInterface(Blueprint, InterfacePath);
            }
            else
            {
                FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.remove_interface")));
                Blueprint->Modify();
                FBlueprintEditorUtils::RemoveInterface(Blueprint, InterfacePath, false);
            }
        }

        if (!bMutationSucceeded)
        {
            Ctx.SendError(TEXT("INTERFACE_MUTATION_FAILED"),
                FString::Printf(TEXT("Failed to %s interface %s on Blueprint %s."),
                    bAddInterface ? TEXT("add") : TEXT("remove"),
                    *InterfaceClass->GetPathName(),
                    *RegistryKey));
            return true;
        }

        bool bCompiledSuccessfully = true;
        FBlueprintCompileDiagnostics Diagnostics;
        if (bChanged && bAutoCompile)
        {
            Diagnostics = CompileBlueprintWithDiagnostics(Blueprint);
            bCompiledSuccessfully = Diagnostics.bCompiled;
        }

        bool bSaved = false;
        if (bChanged && bSave && (!bAutoCompile || bCompiledSuccessfully))
        {
            // bForce skips the throttle, but a transient package still cannot reach
            // disk and that path used to return true.
            bSaved = WasSavePersisted(SaveLoadedAssetThrottled(Blueprint, -1.0, true));
        }

        const FBPInterfaceDescription* CurrentDescription =
            FindImplementedInterface(Blueprint, InterfaceClass);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("success"), bCompiledSuccessfully);
        Result->SetStringField(TEXT("blueprintPath"), RegistryKey);
        Result->SetStringField(TEXT("interfaceClass"), InterfaceClass->GetPathName());
        Result->SetBoolField(TEXT("changed"), bChanged);
        Result->SetBoolField(TEXT("saved"), bSaved);
        Result->SetArrayField(TEXT("interfaceGraphs"), BuildInterfaceGraphNames(CurrentDescription));
        if (bChanged && bAutoCompile)
        {
            AddCompileDiagnosticsToJson(Diagnostics, Result);
        }
        else
        {
            Result->SetBoolField(TEXT("compiled"), false);
        }
        AddAssetVerification(Result, Blueprint);
        Ctx.SendSuccess(Result);
        return true;
    }
}

REGISTER_RPC_HANDLER("blueprint.add_interface", "blueprint",
    "Make a Blueprint implement a Blueprint Interface. Idempotent: an already implemented interface returns changed:false.",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("blueprintPath"), TEXT("path"), TEXT("Target Blueprint asset path. Also accepts path/name aliases.")),
        RPC_PARAM_REQ("interfaceClass", "classref", "Blueprint-implementable interface class, by short name or full class path."),
        RPC_PARAM_DEF("autoCompile", "boolean", "Compile the Blueprint after a change; defaults to true.", "true"),
        RPC_PARAM_DEF("save", "boolean", "Save the Blueprint after a change; defaults to false.", "false")
    ))
{
    return HandleBlueprintInterfaceMutation(Ctx, true);
}

REGISTER_RPC_HANDLER("blueprint.remove_interface", "blueprint",
    "Remove a Blueprint Interface implementation from a Blueprint. Idempotent: a missing interface returns changed:false.",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("blueprintPath"), TEXT("path"), TEXT("Target Blueprint asset path. Also accepts path/name aliases.")),
        RPC_PARAM_REQ("interfaceClass", "classref", "Blueprint-implementable interface class, by short name or full class path."),
        RPC_PARAM_DEF("autoCompile", "boolean", "Compile the Blueprint after a change; defaults to true.", "true"),
        RPC_PARAM_DEF("save", "boolean", "Save the Blueprint after a change; defaults to false.", "false")
    ))
{
    return HandleBlueprintInterfaceMutation(Ctx, false);
}
