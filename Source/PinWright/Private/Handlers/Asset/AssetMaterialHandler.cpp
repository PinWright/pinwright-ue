// Copyright (c) 2026 Alexander Penkin. MIT License.

// Asset material instance/stat handlers: list material instances, reset
// instance parameters, get material stats.
// Migrated from PinWright_AssetWorkflowHandlers.cpp

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Compat/EngineVersionCompat.h"
#include "StaticParameterSet.h"
#include "Utils/AssetUtils.h"

// Material expression includes
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionCosine.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionSine.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Engine/Texture.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "MaterialDomain.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialShared.h"
#include "Handlers/Material/MainInputBindings.h"

// ============================================================================
// asset.list_material_instances
// ============================================================================
REGISTER_RPC_HANDLER("asset.list_material_instances", "asset", "List material instances that use a given parent material",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the parent material")
    ))
{
    FString AssetPath = Ctx.GetString(TEXT("assetPath"));
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("assetPath required"));
        return true;
    }

    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    FARFilter Filter;
    Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("MaterialInstanceConstant")));
    Filter.bRecursiveClasses = true;

    TArray<FAssetData> AssetList;
    AssetRegistry.GetAssets(Filter, AssetList);

    TArray<TSharedPtr<FJsonValue>> Instances;

    for (const FAssetData& Asset : AssetList)
    {
        FString ParentTag;
        if (Asset.GetTagValue(TEXT("Parent"), ParentTag))
        {
            if (ParentTag.Contains(AssetPath))
            {
                Instances.Add(MakeShared<FJsonValueString>(Asset.GetSoftObjectPath().ToString()));
            }
        }
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetArrayField(TEXT("instances"), Instances);
    Ctx.SendSuccess(Resp);
    return true;
}

// ============================================================================
// asset.reset_instance_parameters
// ============================================================================
REGISTER_RPC_HANDLER("asset.reset_instance_parameters", "asset", "Reset all parameter overrides on a material instance",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the material instance constant"),
        RPC_PARAM_DEF("save", "boolean", "Write the reset material instance to disk; false leaves the package dirty and reports saveRequested:false (default true).", "true")
    ))
{
    FString AssetPath = Ctx.GetString(TEXT("assetPath"));
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("assetPath required"));
        return true;
    }

    const FResolvedAsset Resolved = ResolveAsset(AssetPath, /*bLoadObject=*/true);
    if (!Resolved.bExists)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), Resolved.ErrorMessage);
        return true;
    }
    if (!Resolved.Object)
    {
        Ctx.SendError(TEXT("LOAD_FAILED"), Resolved.ErrorMessage);
        return true;
    }

    UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(Resolved.Object);

    if (!MIC)
    {
        Ctx.SendError(TEXT("INVALID_ASSET_TYPE"), TEXT("Asset is not a Material Instance Constant"));
        return true;
    }

    MIC->ClearParameterValuesEditorOnly();
    MIC->PostEditChange();
    MIC->MarkPackageDirty();

    const FStaticParameterSet StaticParameters = MIC->GetStaticParameters();
    const int32 StaticSwitchCount = StaticParameters.StaticSwitchParameters.Num();
#if WITH_EDITORONLY_DATA
    const int32 StaticComponentMaskCount =
        StaticParameters.EditorOnly.StaticComponentMaskParameters.Num();
    const int32 TerrainLayerWeightCount =
        StaticParameters.EditorOnly.TerrainLayerWeightParameters.Num();
#else
    const int32 StaticComponentMaskCount = 0;
    const int32 TerrainLayerWeightCount = 0;
#endif
    const int32 MaterialLayersCount = StaticParameters.bHasMaterialLayers ? 1 : 0;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
    const int32 ParameterCollectionCount = MIC->ParameterCollectionParameterValues.Num();
#else
    // UMaterialInstance::ParameterCollectionParameterValues arrived in 5.7; before that a
    // material instance carries no parameter-collection overrides at all.
    const int32 ParameterCollectionCount = 0;
#endif
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    const int32 TextureCollectionCount = MIC->TextureCollectionParameterValues.Num();
#else
    // UMaterialInstance::TextureCollectionParameterValues arrived in 5.5; before that a
    // material instance carries no texture-collection overrides at all.
    const int32 TextureCollectionCount = 0;
#endif
    const int32 TotalOverrideCount =
        MIC->ScalarParameterValues.Num() +
        MIC->VectorParameterValues.Num() +
        MIC->DoubleVectorParameterValues.Num() +
        ParameterCollectionCount +
        MIC->TextureParameterValues.Num() +
        TextureCollectionCount +
        MIC->RuntimeVirtualTextureParameterValues.Num() +
        MIC->SparseVolumeTextureParameterValues.Num() +
        MIC->FontParameterValues.Num() +
        StaticSwitchCount + StaticComponentMaskCount + TerrainLayerWeightCount +
        MaterialLayersCount;

    TSharedPtr<FJsonObject> RemainingOverrides = MakeShared<FJsonObject>();
    RemainingOverrides->SetNumberField(TEXT("scalar"), MIC->ScalarParameterValues.Num());
    RemainingOverrides->SetNumberField(TEXT("vector"), MIC->VectorParameterValues.Num());
    RemainingOverrides->SetNumberField(TEXT("doubleVector"), MIC->DoubleVectorParameterValues.Num());
    RemainingOverrides->SetNumberField(TEXT("parameterCollection"), ParameterCollectionCount);
    RemainingOverrides->SetNumberField(TEXT("texture"), MIC->TextureParameterValues.Num());
    RemainingOverrides->SetNumberField(TEXT("textureCollection"), TextureCollectionCount);
    RemainingOverrides->SetNumberField(TEXT("runtimeVirtualTexture"), MIC->RuntimeVirtualTextureParameterValues.Num());
    RemainingOverrides->SetNumberField(TEXT("sparseVolumeTexture"), MIC->SparseVolumeTextureParameterValues.Num());
    RemainingOverrides->SetNumberField(TEXT("font"), MIC->FontParameterValues.Num());
    RemainingOverrides->SetNumberField(TEXT("staticSwitch"), StaticSwitchCount);
    RemainingOverrides->SetNumberField(TEXT("staticComponentMask"), StaticComponentMaskCount);
    RemainingOverrides->SetNumberField(TEXT("terrainLayerWeight"), TerrainLayerWeightCount);
    RemainingOverrides->SetNumberField(TEXT("materialLayers"), MaterialLayersCount);
    RemainingOverrides->SetNumberField(TEXT("total"), TotalOverrideCount);

    const bool bSave = Ctx.GetBool(TEXT("save"), true);
    FString PackageName = MIC->GetOutermost()->GetName();
    int64 SizeBytes = 0;
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    bool bSavedToDisk = false;
    if (bSave)
    {
        bSavedToDisk = SaveAssetToDiskReportingPresence(
            MIC, /*bForce=*/true, &PackageName, &SizeBytes, &SaveState);
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("assetPath"), MIC->GetPathName());
    Resp->SetStringField(TEXT("package"), PackageName);
    Resp->SetObjectField(TEXT("remainingOverrideCounts"), RemainingOverrides);
    if (bSave)
    {
        AddAssetSaveSizeReport(Resp, SizeBytes, bSavedToDisk);
    }
    AddAssetSaveReport(Resp, bSave, bSavedToDisk, SaveState);
    Ctx.SendSuccess(Resp);
    return true;
}

// ============================================================================
// asset.get_material_stats
// ============================================================================
REGISTER_RPC_HANDLER("asset.get_material_stats", "asset", "Get material statistics (shading model, samplers, etc.)",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the material")
    ))
{
    FString AssetPath = Ctx.GetString(TEXT("assetPath"));
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("assetPath required"));
        return true;
    }

    const FResolvedAsset Resolved = ResolveAsset(AssetPath, /*bLoadObject=*/true);
    if (!Resolved.bExists)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), Resolved.ErrorMessage);
        return true;
    }
    if (!Resolved.Object)
    {
        Ctx.SendError(TEXT("LOAD_FAILED"), Resolved.ErrorMessage);
        return true;
    }

    UMaterialInterface* Material = Cast<UMaterialInterface>(Resolved.Object);

    if (!Material)
    {
        Ctx.SendError(TEXT("INVALID_ASSET_TYPE"), TEXT("Asset is not a Material"));
        return true;
    }

    Material->EnsureIsComplete();

    TSharedPtr<FJsonObject> Stats = MakeShared<FJsonObject>();

    // Route through the single shading-model->bare-name mapper so this readback can't drift from
    // material.authoring's get_material_node_details('Main') payload.
    FString ShadingModelStr = TEXT("Unknown");
    if (UMaterial* BaseMat = Material->GetMaterial())
    {
        ShadingModelStr = PinWright::Material::GetShadingModelString(BaseMat);
    }
    Stats->SetStringField(TEXT("shadingModel"), ShadingModelStr);

    // instructionCount: emit an honest JSON null rather than a fabricated number.
    // The real representative instruction count lives on the compiled shader map and is
    // produced by FMaterialStatsUtils::GetRepresentativeInstructionCounts, but that utility
    // is not DLL-exported from the MaterialEditor module (it carries no MATERIALEDITOR_API),
    // and the one exported entry point that wraps it (ExtractMatertialStatsInfo) takes the
    // module-private FShaderStatsInfo type — so neither is callable from this module. The
    // count is also only populated once the offline platform shader compiler has run, which
    // does not happen on the headless automation path. A fixed -1 here read as a measured
    // statistic next to the real shadingModel/samplerCount fields
    // (B-material-stats-instruction-count-hardcoded); a null signals "not computed" honestly.
    Stats->SetField(TEXT("instructionCount"), MakeShared<FJsonValueNull>());

    int32 SamplerCount = 0;
    if (UMaterial* BaseMat = Material->GetMaterial())
    {
        for (UMaterialExpression* Expr : BaseMat->GetEditorOnlyData()->ExpressionCollection.Expressions)
        {
            if (Expr && Expr->IsA<UMaterialExpressionTextureSample>())
            {
                SamplerCount++;
            }
        }
    }
    Stats->SetNumberField(TEXT("samplerCount"), SamplerCount);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetObjectField(TEXT("stats"), Stats);
    Ctx.SendSuccess(Resp);
    return true;
}
