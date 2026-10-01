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
#include "Handlers/Material/MaterialShaderState.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "Utils/PackageDirtyUtils.h"
#include "Utils/PieState.h"
#include "Utils/RenderingAvailability.h"

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
REGISTER_RPC_HANDLER("asset.get_material_stats", "asset",
    "Get a material's compiled shader statistics (vertex/pixel instruction counts, samplers, texture "
    "samples, interpolators) for the editor's shader platform. Compiles the material first under the "
    "same bounded wait as material.authoring.compile_material. stats is null with "
    "statsUnavailableReason when no complete shader map can be measured (PIE active, no renderer, "
    "compile failed or timed out); it is never a row of zeros.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the material or material instance")
    ))
{
    namespace MSS = PinWright::MaterialShaderState;

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

    // Route through the single shading-model->bare-name mapper so this readback can't drift from
    // material.authoring's get_material_node_details('Main') payload.
    FString ShadingModelStr = TEXT("Unknown");
    int32 TextureSampleNodeCount = 0;
    if (UMaterial* BaseMat = Material->GetMaterial())
    {
        ShadingModelStr = PinWright::Material::GetShadingModelString(BaseMat);
        for (UMaterialExpression* Expr : BaseMat->GetEditorOnlyData()->ExpressionCollection.Expressions)
        {
            if (Expr && Expr->IsA<UMaterialExpressionTextureSample>())
            {
                TextureSampleNodeCount++;
            }
        }
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    // Graph facts, available without a shader map, so they stay readable when stats is null.
    Resp->SetStringField(TEXT("shadingModel"), ShadingModelStr);
    Resp->SetNumberField(TEXT("textureSampleNodeCount"), TextureSampleNodeCount);
    Resp->SetStringField(TEXT("measuredSubject"), MSS::ToWire(MSS::ResolveMeasuredSubject(Material)));

    // UMaterialEditingLibrary::GetStatistics reads the representative shaders off the
    // GMaxRHIShaderPlatform resource. It calls FMaterial::FinishCompilation() (no ceiling) on an
    // incomplete map, and it returns an all-zero struct when there is nothing to read (PIE, no
    // renderer, a skipped compile) - zeros indistinguishable from a measurement. So it is called
    // only once the bounded ProbeAndWait has reported a complete map, and every other path
    // publishes stats:null with the reason.
    FString UnavailableReason;
    FString UnavailableDetail;
    FMaterialStatistics Statistics;
    if (!PinWrightRendering::IsAvailable())
    {
        UnavailableReason = TEXT("nullRhi");
        UnavailableDetail = TEXT("This editor has no GPU renderer (-NullRHI or a commandlet), so no ")
            TEXT("shader is compiled for a real platform. Relaunch in mode 'offscreen' or 'visible'.");
    }
    else
    {
        Resp->SetStringField(TEXT("statsPlatform"),
            FDataDrivenShaderPlatformInfo::GetName(GMaxRHIShaderPlatform).ToString());

        if (PinWrightPieState::IsPlayInEditorActive())
        {
            // Non-blocking: report what is on the material, measure nothing while PIE runs.
            MSS::AddReport(Resp, MSS::Probe(Material));
            UnavailableReason = TEXT("pieActive");
            UnavailableDetail = TEXT("Play-In-Editor is running; the engine's shader statistics read ")
                TEXT("as zeros under PIE. Stop PIE and call again.");
        }
        else
        {
            // Read verb: the compile below must not leave a package dirty that was clean.
            PinWright::PackageDirty::FScopedPackageDirtyRestore DirtyRestore;
            DirtyRestore.Capture(Material);
            DirtyRestore.Capture(Material->GetMaterial());

            // Start ProbeAndWait from the state it is proven on: the incomplete on-demand map that
            // PostLoad / PostEditChange install with EMaterialShaderPrecompileMode::None, which
            // keeps its compiling id, so CacheShaders(Synchronous) re-finds it, submits every job
            // and drains it under the 90 s bound (material.shader_state.ValidMaterialReportsCompleted).
            // A never-cached material is seeded into that state first; a complete map is read
            // as-is. UMaterialInterface::EnsureIsComplete() is deliberately not called: its
            // FinishCompilation() has no ceiling, and the suite run that called it first read
            // 'notCompiled' with nothing waited on, on the very fixture that passes without it.
            MSS::FState State = MSS::Probe(Material);
            if (State.Status != MSS::EStatus::Completed)
            {
                if (State.Status == MSS::EStatus::NotCompiled)
                {
                    if (UMaterialInterface* Subject =
                            MaterialCompileErrorCollector::ResolveCompileSubject(Material))
                    {
                        Subject->CacheShaders(EMaterialShaderPrecompileMode::None);
                    }
                }
                State = MSS::ProbeAndWait(Material);
            }
            MSS::AddReport(Resp, State);
            if (!State.Succeeded())
            {
                UnavailableReason = MSS::ToWire(State.Status);
                UnavailableDetail = TEXT("No complete shader map to measure; see shaderCompile.status ")
                    TEXT("and shaderCompile.errors.");
            }
            else
            {
                Statistics = UMaterialEditingLibrary::GetStatistics(Material);
                if (Statistics.NumVertexShaderInstructions == 0 && Statistics.NumPixelShaderInstructions == 0)
                {
                    UnavailableReason = TEXT("noRepresentativeShaders");
                    UnavailableDetail = TEXT("The shader map compiled but holds none of the representative ")
                        TEXT("shaders the engine's material stats read, so no instruction count exists.");
                }
            }
        }
    }

    if (UnavailableReason.IsEmpty())
    {
        TSharedPtr<FJsonObject> Stats = MakeShared<FJsonObject>();
        Stats->SetNumberField(TEXT("vertexInstructions"), Statistics.NumVertexShaderInstructions);
        Stats->SetNumberField(TEXT("pixelInstructions"), Statistics.NumPixelShaderInstructions);
        Stats->SetNumberField(TEXT("samplers"), Statistics.NumSamplers);
        Stats->SetNumberField(TEXT("vsTextureSamples"), Statistics.NumVertexTextureSamples);
        Stats->SetNumberField(TEXT("psTextureSamples"), Statistics.NumPixelTextureSamples);
        Stats->SetNumberField(TEXT("virtualTextureSamples"), Statistics.NumVirtualTextureSamples);
        Stats->SetNumberField(TEXT("uvScalars"), Statistics.NumUVScalars);
        Stats->SetNumberField(TEXT("interpolatorScalars"), Statistics.NumInterpolatorScalars);
        // Legacy keys, unchanged in meaning: samplerCount was always the graph TextureSample node
        // count (== textureSampleNodeCount), not the compiler's sampler slots (== samplers).
        Stats->SetStringField(TEXT("shadingModel"), ShadingModelStr);
        Stats->SetNumberField(TEXT("samplerCount"), TextureSampleNodeCount);
        Resp->SetObjectField(TEXT("stats"), Stats);
    }
    else
    {
        Resp->SetField(TEXT("stats"), MakeShared<FJsonValueNull>());
        Resp->SetStringField(TEXT("statsUnavailableReason"), UnavailableReason);
        Resp->SetStringField(TEXT("statsUnavailableDetail"), UnavailableDetail);
    }

    Ctx.SendSuccess(Resp);
    return true;
}
