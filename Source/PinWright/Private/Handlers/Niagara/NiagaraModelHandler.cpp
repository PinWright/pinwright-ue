// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Niagara/NiagaraModelBuilder.h"
#include "Utils/AssetUtils.h"

#include "NiagaraEmitter.h"
#include "NiagaraSystem.h"

namespace
{
    void CollectModelWarnings(const TSharedPtr<FJsonObject>& Object, TArray<TSharedPtr<FJsonValue>>& Warnings)
    {
        if (!Object.IsValid())
        {
            return;
        }

        const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
        if (Object->TryGetArrayField(TEXT("diagnostics"), Diagnostics) && Diagnostics)
        {
            for (const TSharedPtr<FJsonValue>& DiagnosticValue : *Diagnostics)
            {
                if (!DiagnosticValue.IsValid() || DiagnosticValue->Type != EJson::Object)
                {
                    continue;
                }

                const TSharedPtr<FJsonObject> Diagnostic = DiagnosticValue->AsObject();
                if (!Diagnostic.IsValid())
                {
                    continue;
                }

                FString Severity;
                Diagnostic->TryGetStringField(TEXT("severity"), Severity);
                if (!Severity.Equals(TEXT("warning"), ESearchCase::IgnoreCase))
                {
                    continue;
                }

                FString Message;
                if (Diagnostic->TryGetStringField(TEXT("message"), Message) && !Message.IsEmpty())
                {
                    Warnings.Add(MakeShared<FJsonValueString>(Message));
                }
                else
                {
                    Warnings.Add(DiagnosticValue);
                }
            }
        }

        for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : Object->Values)
        {
            if (!Pair.Value.IsValid())
            {
                continue;
            }
            if (Pair.Value->Type == EJson::Object)
            {
                CollectModelWarnings(Pair.Value->AsObject(), Warnings);
            }
            else if (Pair.Value->Type == EJson::Array)
            {
                for (const TSharedPtr<FJsonValue>& Value : Pair.Value->AsArray())
                {
                    if (Value.IsValid() && Value->Type == EJson::Object)
                    {
                        CollectModelWarnings(Value->AsObject(), Warnings);
                    }
                }
            }
        }
    }

    TArray<TSharedPtr<FJsonValue>> ExtractModelWarnings(const TSharedPtr<FJsonObject>& Model)
    {
        TArray<TSharedPtr<FJsonValue>> Warnings;
        CollectModelWarnings(Model, Warnings);
        return Warnings;
    }
}

REGISTER_RPC_HANDLER("niagara.decompile_model", "niagara",
    "Decompile a Niagara system or emitter asset into the compact semantic Niagara model JSON.",
    RPC_PARAMS(
        RPC_PARAM_OPT("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_OPT("path", "path", "Alias for assetPath")
    ))
{
    const FString AssetPathRaw = Ctx.GetStringFirstOf({TEXT("assetPath"), TEXT("path")});
    if (AssetPathRaw.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'assetPath' or 'path'."));
        return true;
    }

    FString NormalizeErr;
    const FString NormalizedPath = TryResolveAssetPath(AssetPathRaw, nullptr, &NormalizeErr);
    if (NormalizedPath.IsEmpty())
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Could not resolve asset path '%s': %s"), *AssetPathRaw, *NormalizeErr));
        return true;
    }

    UObject* Asset = LoadObject<UObject>(nullptr, *NormalizedPath);
    if (!Asset)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Could not load Niagara asset '%s'."), *NormalizedPath));
        return true;
    }

    TSharedPtr<FJsonObject> Model;
    FString AssetKind;
    if (const UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset))
    {
        AssetKind = TEXT("NiagaraSystem");
        Model = NiagaraModelBuilder::BuildSystemModelJson(System);
    }
    else if (const UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Asset))
    {
        AssetKind = TEXT("NiagaraEmitter");
        Model = NiagaraModelBuilder::BuildEmitterModelJson(Emitter);
    }
    else
    {
        Ctx.SendError(TEXT("UNSUPPORTED_ASSET"), TEXT("Asset is not a Niagara System or Niagara Emitter."));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("assetPath"), NormalizedPath);
    Result->SetStringField(TEXT("assetKind"), AssetKind);
    Result->SetObjectField(TEXT("json"), Model);
    Result->SetArrayField(TEXT("warnings"), ExtractModelWarnings(Model));
    Ctx.SendSuccess(Result);
    return true;
}
