// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Utils/IrSidecarRegistry.h"

#include "NIR/NIRDecompiler.h"

#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "Dom/JsonValue.h"

namespace
{
    UClass* GetNIRSystemClass()  { return UNiagaraSystem::StaticClass(); }
    UClass* GetNIREmitterClass() { return UNiagaraEmitter::StaticClass(); }
    UClass* GetNIRScriptClass()  { return UNiagaraScript::StaticClass(); }

    IrSidecarRegistry::FIrSidecarResult BuildNIRSidecar(UObject* Asset)
    {
        const FNIRResult Result = NIRDecompiler::BuildNiagaraIrText(Asset);
        return {Result.Text, Result.Warnings, Result.bSuccess};
    }
}

REGISTER_DECOMPILE_IR(TEXT("nir.system"),  DumpFileNames::Nir, &GetNIRSystemClass,  &BuildNIRSidecar, 100);
REGISTER_DECOMPILE_IR(TEXT("nir.emitter"), DumpFileNames::Nir, &GetNIREmitterClass, &BuildNIRSidecar, 100);
REGISTER_DECOMPILE_IR(TEXT("nir.script"),  DumpFileNames::Nir, &GetNIRScriptClass,  &BuildNIRSidecar, 100);

REGISTER_RPC_HANDLER("niagara.decompile_nir", "niagara",
    "Decompile a Niagara System / Emitter / Module Script to NIR text (v1a: System+Emitter shell).",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Object path to the Niagara asset")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(TEXT("assetPath"), AssetPath))
    {
        return true;
    }

    UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
    if (!Asset)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Could not load Niagara asset: %s"), *AssetPath));
        return true;
    }

    if (!(Asset->IsA<UNiagaraSystem>() || Asset->IsA<UNiagaraEmitter>() || Asset->IsA<UNiagaraScript>()))
    {
        Ctx.SendError(TEXT("NOT_NIAGARA_ASSET"),
            FString::Printf(TEXT("Asset is not a Niagara System / Emitter / Script: %s"), *AssetPath));
        return true;
    }

    const FNIRResult DecompileResult = NIRDecompiler::BuildNiagaraIrText(Asset);
    if (!DecompileResult.bSuccess)
    {
        const FString Message = DecompileResult.Warnings.IsEmpty()
            ? TEXT("NIR decompile failed.")
            : FString::Join(DecompileResult.Warnings, TEXT("\n"));
        Ctx.SendError(TEXT("NIR_DECOMPILE_FAILED"), Message);
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> WarningValues;
    for (const FString& Warning : DecompileResult.Warnings)
    {
        WarningValues.Add(MakeShared<FJsonValueString>(Warning));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetStringField(TEXT("ir"), DecompileResult.Text);
    Result->SetStringField(TEXT("text"), DecompileResult.Text);
    Result->SetArrayField(TEXT("warnings"), WarningValues);
    Ctx.SendSuccess(Result);
    return true;
}
