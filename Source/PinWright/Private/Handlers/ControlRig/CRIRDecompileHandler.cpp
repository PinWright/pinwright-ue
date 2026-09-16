// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Utils/IrSidecarRegistry.h"
#include "Dom/JsonValue.h"

#include "CRIR/CRIRDecompiler.h"
#include "Utils/ControlRigBlueprintCompat.h"

namespace
{
    UClass* GetCRIRControlRigBlueprintClass()
    {
        return UControlRigBlueprint::StaticClass();
    }

    IrSidecarRegistry::FIrSidecarResult BuildCRIRControlRigBlueprintSidecar(UObject* Asset)
    {
        FCRIRDecompiler Decompiler(Cast<UControlRigBlueprint>(Asset));
        const FCRIRDecompileResult Result = Decompiler.Decompile();
        return {Result.CRIRText, Result.Warnings, Result.bSuccess};
    }
}

REGISTER_DECOMPILE_IR(TEXT("crir.control_rig"), DumpFileNames::Crir,
    &GetCRIRControlRigBlueprintClass, &BuildCRIRControlRigBlueprintSidecar, 100);

REGISTER_RPC_HANDLER("controlrig.decompile_crir", "controlrig",
    "Decompile a UControlRigBlueprint asset into CRIR text.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "string", "Control Rig BP asset path")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(TEXT("assetPath"), AssetPath))
    {
        return true;
    }

    UControlRigBlueprint* BP = LoadObject<UControlRigBlueprint>(nullptr, *AssetPath);
    if (!BP)
    {
        Ctx.SendError(TEXT("CRIR_ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Could not load control rig blueprint: %s"), *AssetPath));
        return true;
    }

    FCRIRDecompiler Decompiler(BP);
    FCRIRDecompileResult DecompileResult = Decompiler.Decompile();

    if (!DecompileResult.bSuccess)
    {
        const FString Code = DecompileResult.ErrorCode.IsEmpty()
            ? FString(TEXT("CRIR_DECOMPILE_FAILED"))
            : DecompileResult.ErrorCode;
        const FString Message = DecompileResult.ErrorMessage.IsEmpty()
            ? (DecompileResult.Warnings.IsEmpty()
                ? TEXT("CRIR decompile failed.")
                : FString::Join(DecompileResult.Warnings, TEXT("\n")))
            : DecompileResult.ErrorMessage;
        Ctx.SendError(Code, Message);
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> WarningValues;
    for (const FString& Warning : DecompileResult.Warnings)
    {
        WarningValues.Add(MakeShared<FJsonValueString>(Warning));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetStringField(TEXT("text"), DecompileResult.CRIRText);
    Result->SetArrayField(TEXT("warnings"), WarningValues);
    Ctx.SendSuccess(Result);
    return true;
}
