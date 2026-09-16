// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintReparentHandler.cpp — Change a blueprint's parent class
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Handlers/Blueprint/BlueprintReinstancingGuard.h"
#include "Utils/ClassUtils.h"

#include "Kismet2/BlueprintEditorUtils.h"
#include "BlueprintEditorLibrary.h"

using namespace BlueprintHandlerUtils;

// ---- blueprint.reparent ----
REGISTER_RPC_HANDLER("blueprint.reparent", "blueprint", "Change the parent UClass of a Blueprint. Inherited variables/functions/components from the old parent are dropped; matching members on the new parent become inherited. Recompiles by default; expect existing graphs to need fixup if the parent's API differs.",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("path"), TEXT("path"), TEXT("Blueprint asset path.")),
        RPC_PARAM_REQ("newParentClass", "classref", "Target parent: short class name (e.g. 'Pawn') or full /Script path; must be a valid base for Blueprints."),
        RPC_PARAM_OPT("compile", "boolean", "Run blueprint.compile after the reparent; defaults to true."),
        RPC_PARAM_OPT("save", "boolean", "Mark dirty / save the package after reparent; defaults to true."),
        BlueprintReinstancingGuard::AllowReinstancingParam()
    ))
{
    FString Path = ResolveBlueprintPath(Ctx);
    if (Path.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("blueprint.reparent requires a blueprint path."));
        return true;
    }

    FString NewParentSpec;
    if (!Ctx.RequireString(TEXT("newParentClass"), NewParentSpec)) return true;

    bool bCompile = Ctx.GetBool(TEXT("compile"), true);
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    // Load the blueprint
    FString Normalized, LoadErr;
    UBlueprint* BP = LoadBlueprintAsset(Path, Normalized, LoadErr);
    if (!BP)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Blueprint not found: %s. %s"), *Path, *LoadErr));
        return true;
    }

    // Resolve the new parent class
    UClass* NewParentClass = ResolveUClass(NewParentSpec);
    if (!NewParentClass)
    {
        NewParentClass = ResolveClassByName(NewParentSpec);
    }
    if (!NewParentClass)
    {
        Ctx.SendError(TEXT("CLASS_NOT_FOUND"),
            FString::Printf(TEXT("Could not resolve parent class: %s"), *NewParentSpec));
        return true;
    }

    BlueprintReinstancingGuard::FLiveInstanceSurvey ReinstancingSurvey;
    if (bCompile)
    {
        ReinstancingSurvey = BlueprintReinstancingGuard::SurveyLiveInstances(BP);
        if (BlueprintReinstancingGuard::RefuseIfLiveInstancesWouldBeReinstanced(
                Ctx, BP, ReinstancingSurvey, TEXT("blueprint.reparent")))
        {
            return true;
        }
    }

    // Capture old parent for the response
    FString OldParentPath;
    if (BP->ParentClass)
    {
        OldParentPath = BP->ParentClass->GetPathName();
    }

    // Check if already the same parent
    bool bChanged = (BP->ParentClass != NewParentClass);
    if (bChanged)
    {
        UBlueprintEditorLibrary::ReparentBlueprint(BP, NewParentClass);
    }

    bool bCompiled = false;
    FBlueprintCompileDiagnostics CompileDiagnostics;
    if (bCompile)
    {
        CompileDiagnostics = CompileBlueprintWithDiagnostics(BP);
        bCompiled = CompileDiagnostics.bCompiled;
    }

    bool bSaved = false;
    if (bSave)
    {
        // bForce skips the throttle, but a transient package still cannot reach disk
        // and that path used to return true. WasSavePersisted keeps the report honest.
        bSaved = WasSavePersisted(SaveLoadedAssetThrottled(BP, -1.0, true));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), Normalized.IsEmpty() ? Path : Normalized);
    Result->SetStringField(TEXT("oldParentClass"), OldParentPath);
    Result->SetStringField(TEXT("newParentClass"), NewParentClass->GetPathName());
    Result->SetBoolField(TEXT("changed"), bChanged);
    Result->SetBoolField(TEXT("compiled"), bCompiled);
    Result->SetBoolField(TEXT("saved"), bSaved);
    if (bCompile)
    {
        AddCompileDiagnosticsToJson(
            CompileDiagnostics, Result, TEXT("compileErrors"), TEXT("compileWarnings"));
        BlueprintReinstancingGuard::AddSurveyToJson(ReinstancingSurvey, Result);
    }
    Ctx.SendSuccess(Result);
    return true;
}
