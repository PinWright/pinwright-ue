// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Utils/IrSidecarRegistry.h"
#include "Dom/JsonValue.h"

#include "AGIR/AGIRDecompiler.h"
#include "Animation/AnimBlueprint.h"

namespace
{
    UClass* GetAGIRAnimBlueprintClass()
    {
        return UAnimBlueprint::StaticClass();
    }

    IrSidecarRegistry::FIrSidecarResult BuildAGIRAnimBlueprintSidecar(UObject* Asset)
    {
        FAGIRDecompiler Decompiler(Cast<UAnimBlueprint>(Asset));
        const FAGIRDecompileResult Result = Decompiler.Decompile();
        return {Result.AGIRText, Result.Warnings, Result.bSuccess};
    }
}

REGISTER_DECOMPILE_IR(TEXT("agir.anim_blueprint"), DumpFileNames::Agir,
    &GetAGIRAnimBlueprintClass, &BuildAGIRAnimBlueprintSidecar, 100);

REGISTER_RPC_HANDLER("anim.decompile_agir", "anim",
    "Decompile a UAnimBlueprint asset into AGIR text.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Anim BP asset path")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(TEXT("assetPath"), AssetPath))
    {
        return true;
    }

    UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AssetPath);
    if (!AnimBP)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Could not load anim blueprint: %s"), *AssetPath));
        return true;
    }

    FAGIRDecompiler Decompiler(AnimBP);
    FAGIRDecompileResult DecompileResult = Decompiler.Decompile();

    if (!DecompileResult.bSuccess)
    {
        const FString Message = DecompileResult.Warnings.IsEmpty()
            ? TEXT("AGIR decompile failed.")
            : FString::Join(DecompileResult.Warnings, TEXT("\n"));
        Ctx.SendError(TEXT("AGIR_DECOMPILE_FAILED"), Message);
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> WarningValues;
    for (const FString& Warning : DecompileResult.Warnings)
    {
        WarningValues.Add(MakeShared<FJsonValueString>(Warning));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetStringField(TEXT("text"), DecompileResult.AGIRText);
    Result->SetArrayField(TEXT("warnings"), WarningValues);
    Ctx.SendSuccess(Result);
    return true;
}
