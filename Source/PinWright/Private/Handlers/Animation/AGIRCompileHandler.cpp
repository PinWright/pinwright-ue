// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "AGIR/AGIRCompiler.h"

namespace
{
bool TryParseAGIRCompileMode(const FString& ModeText, EAGIRCompileMode& OutMode, FString& OutError)
{
    if (ModeText.IsEmpty() || ModeText.Equals(TEXT("replace"), ESearchCase::IgnoreCase))
    {
        OutMode = EAGIRCompileMode::Replace;
        return true;
    }

    if (ModeText.Equals(TEXT("extend"), ESearchCase::IgnoreCase))
    {
        OutMode = EAGIRCompileMode::Extend;
        return true;
    }

    OutError = FString::Printf(TEXT("Unknown AGIR compile mode '%s'. Use Replace or Extend."), *ModeText);
    return false;
}

FString CompileModeToText(EAGIRCompileMode Mode)
{
    return Mode == EAGIRCompileMode::Extend ? TEXT("Extend") : TEXT("Replace");
}
}

REGISTER_RPC_HANDLER("anim.compile_agir", "anim",
    "Compile AGIR text into a target UAnimBlueprint.",
    RPC_PARAMS(
        RPC_PARAM_REQ("text", "string", "AGIR document text"),
        RPC_PARAM_REQ("context", "path", "Target Anim BP asset path"),
        RPC_PARAM_OPT("mode", "string", "Replace clears target graphs first; Extend appends to existing graphs"),
        RPC_PARAM_OPT("runLayout", "boolean", "Auto-layout nodes missing positions (default true)"),
        RPC_PARAM_OPT("save", "boolean", "Mark modified asset dirty for saving (default false)")
    ))
{
    FString Text;
    if (!Ctx.RequireString(TEXT("text"), Text))
    {
        return true;
    }

    FString Context;
    if (!Ctx.RequireString(TEXT("context"), Context))
    {
        return true;
    }

    FAGIRCompileOptions Options;
    Options.Context = Context;
    Options.bRunLayout = Ctx.GetBool(TEXT("runLayout"), true);
    Options.bSave = Ctx.GetBool(TEXT("save"), false);

    FString ModeError;
    if (!TryParseAGIRCompileMode(Ctx.GetString(TEXT("mode")), Options.Mode, ModeError))
    {
        Ctx.SendError(TEXT("INVALID_MODE"), ModeError);
        return true;
    }

    FAGIRCompileResult CompileResult = FAGIRCompiler::Compile(Text, Options);
    if (!CompileResult.bSuccess)
    {
        // Relay any attribution hint on the tool surface so callers can tell a
        // tool round-trip gap from their own bad AGIR without reading plugin C++
        // (see E-agir-roundtrip-symbol-not-found-undiagnosable). The hint rides
        // in a `hint` field on the error's structured result. A null result
        // routes through SendError's plain-error path, so build it only when a
        // hint is present and dispatch once.
        TSharedPtr<FJsonObject> ErrorResult;
        if (!CompileResult.Hint.IsEmpty())
        {
            ErrorResult = MakeShared<FJsonObject>();
            ErrorResult->SetStringField(TEXT("hint"), CompileResult.Hint);
        }
        Ctx.SendError(CompileResult.ErrorCode, CompileResult.ErrorMessage, ErrorResult);
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> WarningValues;
    for (const FString& Warning : CompileResult.Warnings)
    {
        WarningValues.Add(MakeShared<FJsonValueString>(Warning));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("mode"), CompileModeToText(Options.Mode));
    Result->SetStringField(TEXT("assetPath"), CompileResult.AssetPath);
    Result->SetNumberField(TEXT("blocksCompiled"), CompileResult.BlocksCompiled);
    Result->SetNumberField(TEXT("nodesCreated"), CompileResult.NodesCreated);
    Result->SetArrayField(TEXT("warnings"), WarningValues);
    Ctx.SendSuccess(Result);
    return true;
}
