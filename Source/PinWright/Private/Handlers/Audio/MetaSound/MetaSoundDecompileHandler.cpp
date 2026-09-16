// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Asset/AssetDumpHandler.h"

#include "MSIR/MSIRDecompiler.h"
#include "Utils/IrSidecarRegistry.h"

#include "Dom/JsonValue.h"
#include "UObject/Object.h"
#include "UObject/UObjectGlobals.h"

#if __has_include("MetasoundSource.h")
#include "MetasoundDocumentInterface.h"
#include "MetasoundSource.h"
#include "Metasound.h"
#define MCP_MSIR_HANDLER_HAS_METASOUND 1
#else
#define MCP_MSIR_HANDLER_HAS_METASOUND 0
#endif

#if MCP_MSIR_HANDLER_HAS_METASOUND

namespace
{
    UClass* GetMSIRMetaSoundPatchClass()
    {
        return UMetaSoundPatch::StaticClass();
    }

    UClass* GetMSIRMetaSoundSourceClass()
    {
        return UMetaSoundSource::StaticClass();
    }

    IrSidecarRegistry::FIrSidecarResult BuildMSIRSidecar(UObject* Asset)
    {
        const FMSIRResult Result = FMSIRDecompiler::BuildMetaSoundIrText(Asset);
        return {Result.Text, Result.Warnings, Result.bSuccess};
    }
}

REGISTER_DECOMPILE_IR(TEXT("msir.metasound_patch"), DumpFileNames::Msir,
    &GetMSIRMetaSoundPatchClass, &BuildMSIRSidecar, 100);

REGISTER_DECOMPILE_IR(TEXT("msir.metasound_source"), DumpFileNames::Msir,
    &GetMSIRMetaSoundSourceClass, &BuildMSIRSidecar, 100);

#endif // MCP_MSIR_HANDLER_HAS_METASOUND

REGISTER_RPC_HANDLER("audio.authoring.decompile_metasound", "audio.authoring",
    "Decompile a MetaSound asset to MSIR text.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Object path to the MetaSound asset")
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
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), FString::Printf(TEXT("Could not load asset: %s"), *AssetPath));
        return true;
    }

#if MCP_MSIR_HANDLER_HAS_METASOUND
    if (!Cast<IMetaSoundDocumentInterface>(Asset))
    {
        Ctx.SendError(TEXT("NOT_METASOUND"), FString::Printf(TEXT("Asset is not a MetaSound: %s"), *AssetPath));
        return true;
    }
#else
    Ctx.SendError(TEXT("MSIR_DECOMPILE_FAILED"), TEXT("MetaSound modules not available in this build."));
    return true;
#endif

    const FMSIRResult DecompileResult = FMSIRDecompiler::BuildMetaSoundIrText(Asset);
    if (!DecompileResult.bSuccess)
    {
        const FString Message = DecompileResult.Warnings.IsEmpty()
            ? TEXT("MSIR decompile failed.")
            : FString::Join(DecompileResult.Warnings, TEXT("\n"));
        Ctx.SendError(TEXT("MSIR_DECOMPILE_FAILED"), Message);
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
