// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintDecompilerHandler.cpp - Decompile Blueprint graphs to BPIR text
// (Now uses FBpirDecompiler for BPIR output)

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Decompiler/BpirDecompiler.h"
#include "Utils/AssetUtils.h"

#include "Engine/Blueprint.h"

using namespace BlueprintHandlerUtils;

static bool SendDecompileResult(FHandlerContext& Ctx, const FBpirDecompileResult& Result)
{
    // Warnings serialize as `{text, severity}` objects.
    TArray<TSharedPtr<FJsonValue>> WarningsArray;
    for (const FBpirWarning& Warning : Result.Warnings)
    {
        TSharedPtr<FJsonObject> WarnObj = MakeShared<FJsonObject>();
        WarnObj->SetStringField(TEXT("text"), Warning.Text);
        WarnObj->SetStringField(TEXT("severity"),
            Warning.Severity == EBpirWarningSeverity::Error ? TEXT("error") : TEXT("warn"));
        WarningsArray.Add(MakeShared<FJsonValueObject>(WarnObj));
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetBoolField(TEXT("success"), Result.bSuccess);
    Out->SetStringField(TEXT("bpir"), Result.BpirText);
    Out->SetArrayField(TEXT("warnings"), WarningsArray);

    if (Result.bSuccess)
    {
        Ctx.SendSuccess(Out);
    }
    else
    {
        TArray<FString> WarningTexts;
        WarningTexts.Reserve(Result.Warnings.Num());
        for (const FBpirWarning& W : Result.Warnings) { WarningTexts.Add(W.Text); }
        FString ErrorMessage = WarningTexts.Num() > 0
            ? FString::Join(WarningTexts, TEXT("; "))
            : TEXT("Decompilation failed");
        Ctx.SendError(TEXT("DECOMPILE_FAILED"), ErrorMessage);
    }
    return true;
}

// ---- blueprint.decompile ----
REGISTER_RPC_HANDLER("blueprint.decompile", "blueprint",
    "Decompile Blueprint graphs into BPIR (Blueprint IR) text",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_OPT("graphName", "string", "Specific graph name (omit for all graphs)")
    ))
{
    FString AssetPath = ResolveBlueprintPath(Ctx);
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"),
            TEXT("blueprint.decompile: a blueprint path is required (use 'path' or 'assetPath')."));
        return true;
    }

    FString Normalized, LoadErr;
    UBlueprint* BP = LoadBlueprintAsset(AssetPath, Normalized, LoadErr);
    if (!BP)
    {
        Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"),
            LoadErr.IsEmpty() ? TEXT("Failed to load Blueprint asset") : *LoadErr);
        return true;
    }

    const FString GraphName = Ctx.GetString(TEXT("graphName"));

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = GraphName.IsEmpty()
        ? Decompiler.Decompile()
        : Decompiler.DecompileGraph(GraphName);

    return SendDecompileResult(Ctx, Result);
}

// ---- blueprint.decompile_function ----
REGISTER_RPC_HANDLER("blueprint.decompile_function", "blueprint",
    "Decompile a specific Blueprint function to BPIR text",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ("functionName", "string", "Name of the function to decompile")
    ))
{
    FString AssetPath = ResolveBlueprintPath(Ctx);
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"),
            TEXT("blueprint.decompile_function: a blueprint path is required (use 'path' or 'assetPath')."));
        return true;
    }
    FString FunctionName;
    if (!Ctx.RequireString(TEXT("functionName"), FunctionName)) return true;

    FString Normalized, LoadErr;
    UBlueprint* BP = LoadBlueprintAsset(AssetPath, Normalized, LoadErr);
    if (!BP)
    {
        Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"),
            LoadErr.IsEmpty() ? TEXT("Failed to load Blueprint asset") : *LoadErr);
        return true;
    }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.DecompileFunction(FunctionName);

    return SendDecompileResult(Ctx, Result);
}

// ---- blueprint.decompile_macro ----
REGISTER_RPC_HANDLER("blueprint.decompile_macro", "blueprint",
    "Decompile a specific Blueprint macro to BPIR text",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ("macroName", "string", "Name of the macro to decompile")
    ))
{
    FString AssetPath = ResolveBlueprintPath(Ctx);
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"),
            TEXT("blueprint.decompile_macro: a blueprint path is required (use 'path' or 'assetPath')."));
        return true;
    }
    FString MacroName;
    if (!Ctx.RequireString(TEXT("macroName"), MacroName)) return true;

    FString Normalized, LoadErr;
    UBlueprint* BP = LoadBlueprintAsset(AssetPath, Normalized, LoadErr);
    if (!BP)
    {
        Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"),
            LoadErr.IsEmpty() ? TEXT("Failed to load Blueprint asset") : *LoadErr);
        return true;
    }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.DecompileMacro(MacroName);

    return SendDecompileResult(Ctx, Result);
}
