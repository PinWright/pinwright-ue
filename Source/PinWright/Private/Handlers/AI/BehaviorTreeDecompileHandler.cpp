// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Utils/IrSidecarRegistry.h"

#include "BTIR/BTIRDecompiler.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "Dom/JsonValue.h"

namespace
{
    UClass* GetBTIRBehaviorTreeClass()
    {
        return UBehaviorTree::StaticClass();
    }

    UClass* GetBTIRBlackboardClass()
    {
        return UBlackboardData::StaticClass();
    }

    IrSidecarRegistry::FIrSidecarResult BuildBTIRBehaviorTreeSidecar(UObject* Asset)
    {
        const FBTIRResult Result = BTIRDecompiler::BuildBehaviorTreeIrText(Cast<UBehaviorTree>(Asset));
        return {Result.Text, Result.Warnings, Result.bSuccess};
    }

    IrSidecarRegistry::FIrSidecarResult BuildBTIRBlackboardSidecar(UObject* Asset)
    {
        const FBTIRResult Result = BTIRDecompiler::BuildBlackboardIrText(Cast<UBlackboardData>(Asset));
        return {Result.Text, Result.Warnings, Result.bSuccess};
    }
}

REGISTER_DECOMPILE_IR(TEXT("btir.behavior_tree"), DumpFileNames::Btir,
    &GetBTIRBehaviorTreeClass, &BuildBTIRBehaviorTreeSidecar, 100);
REGISTER_DECOMPILE_IR(TEXT("btir.blackboard"), DumpFileNames::Btir,
    &GetBTIRBlackboardClass, &BuildBTIRBlackboardSidecar, 100);

REGISTER_RPC_HANDLER("behavior_tree.decompile", "behavior_tree",
    "Decompile a Behavior Tree or Blackboard asset into BTIR text.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the Behavior Tree or Blackboard asset")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(TEXT("assetPath"), AssetPath))
    {
        return true;
    }

    FBTIRResult DecompileResult;
    if (UBehaviorTree* BehaviorTree = LoadObject<UBehaviorTree>(nullptr, *AssetPath))
    {
        DecompileResult = BTIRDecompiler::BuildBehaviorTreeIrText(BehaviorTree);
    }
    else if (UBlackboardData* Blackboard = LoadObject<UBlackboardData>(nullptr, *AssetPath))
    {
        DecompileResult = BTIRDecompiler::BuildBlackboardIrText(Blackboard);
    }
    else
    {
        Ctx.SendError(TEXT("BTIR_ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Could not load Behavior Tree or Blackboard asset: %s"), *AssetPath));
        return true;
    }

    if (!DecompileResult.bSuccess)
    {
        const FString Message = DecompileResult.Warnings.IsEmpty()
            ? TEXT("BTIR decompile failed.")
            : FString::Join(DecompileResult.Warnings, TEXT("\n"));
        Ctx.SendError(TEXT("BTIR_DECOMPILE_FAILED"), Message);
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
