// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Dom/JsonValue.h"

#include "CRIR/CRIRCompiler.h"

namespace
{
bool TryParseCRIRCompileMode(const FString& ModeText, ECRIRCompileMode& OutMode, FString& OutError)
{
    if (ModeText.IsEmpty() || ModeText.Equals(TEXT("replace"), ESearchCase::IgnoreCase))
    {
        OutMode = ECRIRCompileMode::Replace;
        return true;
    }

    if (ModeText.Equals(TEXT("extend"), ESearchCase::IgnoreCase))
    {
        OutMode = ECRIRCompileMode::Extend;
        return true;
    }

    OutError = FString::Printf(TEXT("Unknown CRIR compile mode '%s'. Use replace or extend."), *ModeText);
    return false;
}

FString CompileModeToText(ECRIRCompileMode Mode)
{
    return Mode == ECRIRCompileMode::Extend ? TEXT("extend") : TEXT("replace");
}
}

REGISTER_RPC_HANDLER("controlrig.compile_crir", "controlrig",
    "Compile CRIR text into a target UControlRigBlueprint.",
    RPC_PARAMS(
        RPC_PARAM_REQ("text", "string", "CRIR document text"),
        RPC_PARAM_REQ("context", "string", "Target UControlRigBlueprint asset path"),
        RPC_PARAM_OPT("mode", "string", "replace clears existing nodes per model first; extend appends"),
        RPC_PARAM_OPT("runLayout", "boolean", "Auto-position nodes missing @(x,y) (default true)"),
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

    FCRIRCompileOptions Options;
    Options.TargetAssetPath = Context;
    Options.bRunLayout = Ctx.GetBool(TEXT("runLayout"), true);
    Options.bSave = Ctx.GetBool(TEXT("save"), false);

    FString ModeError;
    if (!TryParseCRIRCompileMode(Ctx.GetString(TEXT("mode")), Options.Mode, ModeError))
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), ModeError);
        return true;
    }

    FCRIRCompileResult CompileResult = FCRIRCompiler::Compile(Text, Options);
    if (!CompileResult.bSuccess)
    {
        Ctx.SendError(CompileResult.ErrorCode, CompileResult.ErrorMessage);
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
