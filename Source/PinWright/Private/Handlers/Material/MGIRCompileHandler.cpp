// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "MGIR/MGIRCompiler.h"
#include "Handlers/Material/MaterialShaderState.h"
#include "Utils/AssetUtils.h"
#include "Utils/DerivedStateReport.h"
#include "Utils/GuardedLoad.h"
#include "Dom/JsonValue.h"
#include "Materials/MaterialInterface.h"

namespace
{
bool TryParseCompileMode(const FString& ModeText, EMGIRCompileMode& OutMode, FString& OutError)
{
    if (ModeText.IsEmpty() || ModeText.Equals(TEXT("append"), ESearchCase::IgnoreCase))
    {
        OutMode = EMGIRCompileMode::Append;
        return true;
    }

    if (ModeText.Equals(TEXT("extend"), ESearchCase::IgnoreCase))
    {
        OutMode = EMGIRCompileMode::Extend;
        return true;
    }

    OutError = FString::Printf(TEXT("Unknown MGIR compile mode '%s'. Use Append or Extend."), *ModeText);
    return false;
}

FString CompileModeToText(EMGIRCompileMode Mode)
{
    return Mode == EMGIRCompileMode::Extend ? TEXT("Extend") : TEXT("Append");
}
}

REGISTER_RPC_HANDLER("material.compile_mgir", "material",
    "Compile MGIR text into material graph assets, applying any `property Name: Value` lines in an entry material block to the material itself (blend mode, shading model, two-sided, domain, translucency lighting mode and five more - see material.mgir), and push each compiled master into the consumers that cache instances derived from it. consumerRefresh reports the landscapes whose per-component material instances were rebuilt, measured by MIC identity - without that rebuild a compile of a landscape master writes a correct .uasset and renders as a no-op while every field reports success. `blocksCompiled` counts GRAPH writes, NOT shader compiles: read shaderCompile.status for whether the resulting shader actually builds, and pass waitForShaderCompile:true to block on the real verdict rather than the non-blocking probe. A material with malformed Custom HLSL or a sampler-type mismatch compiles here, saves, reads back correctly and renders the engine Default Material.",
    RPC_PARAMS(
        RPC_PARAM_REQ("text", "string", "MGIR document text"),
        RPC_PARAM_OPT("mode", "string", "Append clears target graphs first; Extend only ADDS to existing graphs and refuses a symbol that already exists (it cannot update one)"),
        RPC_PARAM_OPT("context", "path", "Fallback target asset path for unnamed entry blocks"),
        RPC_PARAM_OPT("runLayout", "boolean", "Auto-layout expressions missing positions (default true)"),
        RPC_PARAM_OPT("save", "boolean", "Write modified assets to disk (default true); a PIE-blocked write keeps the compile successful and reports saveState:blockedByPie"),
        PinWright::MaterialShaderState::WaitParamSpec()
    ))
{
    FString Text;
    if (!Ctx.RequireString(TEXT("text"), Text))
    {
        return true;
    }

    FMGIRCompileOptions Options;
    Options.Context = Ctx.GetString(TEXT("context"));
    Options.bRunLayout = Ctx.GetBool(TEXT("runLayout"), true);
    Options.bSave = Ctx.GetBool(TEXT("save"), true);

    FString ModeError;
    if (!TryParseCompileMode(Ctx.GetString(TEXT("mode")), Options.Mode, ModeError))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_MODE, ModeError);
        return true;
    }

    FMGIRCompileResult CompileResult = FMGIRCompiler::Compile(Text, Options);
    if (!CompileResult.bSuccess)
    {
        Ctx.SendError(CompileResult.ErrorCode, CompileResult.ErrorMessage);
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> AssetValues;
    for (const FString& AssetPath : CompileResult.AssetPaths)
    {
        AssetValues.Add(MakeShared<FJsonValueString>(AssetPath));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("mode"), CompileModeToText(Options.Mode));
    Result->SetNumberField(TEXT("blocksCompiled"), CompileResult.BlocksCompiled);
    Result->SetNumberField(TEXT("expressionsCreated"), CompileResult.ExpressionsCreated);
    Result->SetArrayField(TEXT("assetPaths"), AssetValues);
    const bool bSavedToDisk = Options.bSave && IsAssetSaveStateDurable(CompileResult.SaveState);
    AddAssetSaveReport(Result, Options.bSave, bSavedToDisk, CompileResult.SaveState);
    if (CompileResult.SaveState == EAssetSaveState::BlockedByPie)
    {
        Result->SetStringField(TEXT("saveError"), ErrorCodes::ERR_PIE_ACTIVE);
    }

    // The shader verdict for every material this document wrote, folded into the one
    // namespace-wide field. "compile_mgir" names a GRAPH compile: blocksCompiled and
    // expressionsCreated describe expressions placed and wired, and a document whose Custom node
    // holds broken HLSL produces exactly the same numbers as one that renders
    // (E-material-verbs-have-no-shader-compile-signal). The default is the free non-blocking probe;
    // waitForShaderCompile:true pays for the real answer, which in a headless editor is the only
    // way to get one - see MaterialShaderState.h.
    const bool bWaitForShaderCompile =
        Ctx.GetBool(PinWright::MaterialShaderState::WaitParamName(), false);
    PinWright::MaterialShaderState::FState ShaderState;
    for (const FString& AssetPath : CompileResult.AssetPaths)
    {
        // Guarded because the path is a substring of the MGIR `text` document, which the
        // dispatch-boundary path gate cannot type-check (see Utils/GuardedLoad.h).
        UMaterialInterface* MaterialInterface =
            PinWrightGuardedLoad::LoadObjectChecked<UMaterialInterface>(AssetPath);
        if (!MaterialInterface)
        {
            // A material FUNCTION entry has no shader map of its own; skipping it keeps the
            // aggregate a statement about materials rather than diluting it with notMeasured rows.
            continue;
        }
        ShaderState.Accumulate(AssetPath,
            bWaitForShaderCompile
                ? PinWright::MaterialShaderState::ProbeAndWait(MaterialInterface)
                : PinWright::MaterialShaderState::Probe(MaterialInterface));
    }
    PinWright::MaterialShaderState::AddReport(Result, ShaderState);

    // Measured coverage of the downstream rebuild, same vocabulary compile_material
    // publishes. A caller compiling a landscape master through MGIR reads
    // consumerRefresh.consumersRefreshed to tell "the terrain is rendering what I just
    // wrote" from "the .uasset is correct and the screen is not".
    PinWright::DerivedState::AddConsumerRefreshReport(Result, CompileResult.ConsumerRefresh);

    // A compile whose consumers were not refreshed is not a plain success. Say so rather
    // than leaving the caller to infer it from a render that did not change — the exact
    // inference this defect made impossible.
    TArray<TSharedPtr<FJsonValue>> Warnings;
    // Warnings the compiler itself raised about what it changed — a function pin the rebuild
    // renamed or dropped disconnects every material already saved against it, and this is the only
    // moment that is visible. Carried first because it names a write that already happened.
    for (const FString& CompileWarning : CompileResult.Warnings)
    {
        Warnings.Add(MakeShared<FJsonValueString>(CompileWarning));
    }
    // Raised first, and raised into warnings[] rather than only into the shaderCompile block, so it
    // survives a caller that reads only the top level. A shader that failed to compile makes every
    // other field in this response a statement about an asset that draws nothing.
    if (ShaderState.Failed())
    {
        Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT(
            "The shader FAILED to compile for at least one material in this document, so it "
            "renders as the engine Default Material. blocksCompiled and expressionsCreated "
            "describe the GRAPH write and are not evidence of a working material. The %d HLSL "
            "error(s) are in shaderCompile.errors."),
            ShaderState.Errors.Num())));
    }
    if (!CompileResult.ConsumerRefresh.bMeasured)
    {
        Warnings.Add(MakeShared<FJsonValueString>(TEXT(
            "Consumers were not enumerated (no editor world available, or no material "
            "block in the document), so this result is not evidence that the compile "
            "reached anything rendering these materials.")));
    }
    else if (!CompileResult.ConsumerRefresh.IsComplete())
    {
        Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT(
            "%d of %d landscape consumers did not rebuild their component material "
            "instances, so the compile has not reached them and a render will still show "
            "the previous shader map. Re-apply with landscape.set_material on the "
            "affected landscape."),
            CompileResult.ConsumerRefresh.ConsumersFound
                - CompileResult.ConsumerRefresh.ConsumersRefreshed
                - CompileResult.ConsumerRefresh.ConsumersWithNothingToRefresh,
            CompileResult.ConsumerRefresh.ConsumersFound)));
    }
    if (Warnings.Num() > 0)
    {
        Result->SetArrayField(TEXT("warnings"), Warnings);
    }

    Ctx.SendSuccess(Result);
    return true;
}
