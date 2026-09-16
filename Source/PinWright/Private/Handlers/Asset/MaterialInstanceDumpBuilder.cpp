// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/MaterialInstanceDumpBuilder.h"

#include "Dom/JsonValue.h"
#include "Engine/SubsurfaceProfile.h"
#include "Engine/Texture.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialLayersFunctions.h"
// FMaterialParameterInfo/Value/Metadata moved from the top-level MaterialTypes.h
// into Materials/MaterialParameters.h in UE 5.7; on 5.6 and earlier they live in
// MaterialTypes.h, which has no MaterialParameters.h (and is deprecated on 5.8).
#if __has_include("Materials/MaterialParameters.h")
#include "Materials/MaterialParameters.h"
#else
#include "MaterialTypes.h"
#endif
#include "Compat/EngineVersionCompat.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "StaticParameterSet.h"
#include "Utils/JsonBuilders.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Utils/JsonSidecarRegistry.h"

namespace
{
    void AddBasePropertyOverrides(const FMaterialInstanceBasePropertyOverrides& BPO,
                                  TSharedPtr<FJsonObject>& Out)
    {
        // bOverride_* flags + paired value fields. We always emit the bOverride flag
        // plus the paired value (independent of bOverride state) so consumers can diff
        // either dimension.
        Out->SetBoolField(TEXT("bOverride_BlendMode"), BPO.bOverride_BlendMode != 0);
        Out->SetStringField(TEXT("BlendMode"), JsonBuilders::EnumValueToString<EBlendMode>(BPO.BlendMode.GetValue()));

        Out->SetBoolField(TEXT("bOverride_ShadingModel"), BPO.bOverride_ShadingModel != 0);
        Out->SetStringField(TEXT("ShadingModel"), JsonBuilders::EnumValueToString<EMaterialShadingModel>(BPO.ShadingModel.GetValue()));

        Out->SetBoolField(TEXT("bOverride_OpacityMaskClipValue"), BPO.bOverride_OpacityMaskClipValue != 0);
        Out->SetNumberField(TEXT("OpacityMaskClipValue"), BPO.OpacityMaskClipValue);

        Out->SetBoolField(TEXT("bOverride_TwoSided"), BPO.bOverride_TwoSided != 0);
        Out->SetBoolField(TEXT("TwoSided"), BPO.TwoSided != 0);

        Out->SetBoolField(TEXT("bOverride_DitheredLODTransition"), BPO.bOverride_DitheredLODTransition != 0);
        Out->SetBoolField(TEXT("DitheredLODTransition"), BPO.DitheredLODTransition != 0);

        Out->SetBoolField(TEXT("bOverride_OutputTranslucentVelocity"), BPO.bOverride_OutputTranslucentVelocity != 0);
        Out->SetBoolField(TEXT("OutputTranslucentVelocity"), BPO.bOutputTranslucentVelocity != 0);

        Out->SetBoolField(TEXT("bOverride_CastDynamicShadowAsMasked"), BPO.bOverride_CastDynamicShadowAsMasked != 0);
        Out->SetBoolField(TEXT("CastDynamicShadowAsMasked"), BPO.bCastDynamicShadowAsMasked != 0);

        Out->SetBoolField(TEXT("bOverride_DisplacementScaling"), BPO.bOverride_DisplacementScaling != 0);
        {
            TSharedPtr<FJsonObject> Scaling = MakeShared<FJsonObject>();
            Scaling->SetNumberField(TEXT("Magnitude"), BPO.DisplacementScaling.Magnitude);
            Scaling->SetNumberField(TEXT("Center"), BPO.DisplacementScaling.Center);
            Out->SetObjectField(TEXT("DisplacementScaling"), Scaling);
        }

        // 5.6+: DisplacementFadeRange / bOverride_DisplacementFadeRange were added to
        // FMaterialInstanceBasePropertyOverrides. On 5.4/5.5 these members do not exist,
        // so we skip the section entirely there to preserve the per-version contract.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        Out->SetBoolField(TEXT("bOverride_DisplacementFadeRange"), BPO.bOverride_DisplacementFadeRange != 0);
        {
            TSharedPtr<FJsonObject> Fade = MakeShared<FJsonObject>();
            Fade->SetNumberField(TEXT("StartSizePixels"), BPO.DisplacementFadeRange.StartSizePixels);
            Fade->SetNumberField(TEXT("EndSizePixels"), BPO.DisplacementFadeRange.EndSizePixels);
            Out->SetObjectField(TEXT("DisplacementFadeRange"), Fade);
        }
#endif

        Out->SetBoolField(TEXT("bOverride_MaxWorldPositionOffsetDisplacement"), BPO.bOverride_MaxWorldPositionOffsetDisplacement != 0);
        Out->SetNumberField(TEXT("MaxWorldPositionOffsetDisplacement"), BPO.MaxWorldPositionOffsetDisplacement);
    }
}

TSharedPtr<FJsonObject> MaterialInstanceDumpBuilder::BuildMaterialInstanceJson(const UMaterialInstanceConstant* Instance)
{
    if (!Instance)
    {
        return nullptr;
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    // ---- parent + parentChain ----
    UMaterialInterface* Parent = Instance->Parent;
    if (Parent)
    {
        Root->SetStringField(TEXT("parent"), Parent->GetPathName());
    }
    else
    {
        Root->SetField(TEXT("parent"), MakeShared<FJsonValueNull>());
    }
    {
        TArray<TSharedPtr<FJsonValue>> ChainArr;
        UMaterialInterface* Walk = Parent;
        // Walk up the parent chain; FMaterialInstance::Parent is also a UMaterialInterface*,
        // so we keep walking as long as we see a UMaterialInstance link in the chain.
        while (Walk)
        {
            ChainArr.Add(MakeShared<FJsonValueString>(Walk->GetPathName()));
            UMaterialInstance* WalkMI = Cast<UMaterialInstance>(Walk);
            Walk = WalkMI ? WalkMI->Parent : nullptr;
        }
        Root->SetArrayField(TEXT("parentChain"), ChainArr);
    }

    // ---- overrides ----
    {
        TSharedPtr<FJsonObject> Overrides = MakeShared<FJsonObject>();

        TSharedPtr<FJsonObject> ScalarObj = MakeShared<FJsonObject>();
        for (const FScalarParameterValue& V : Instance->ScalarParameterValues)
        {
            ScalarObj->SetNumberField(V.ParameterInfo.Name.ToString(), V.ParameterValue);
        }
        Overrides->SetObjectField(TEXT("scalar"), ScalarObj);

        TSharedPtr<FJsonObject> VectorObj = MakeShared<FJsonObject>();
        for (const FVectorParameterValue& V : Instance->VectorParameterValues)
        {
            VectorObj->SetObjectField(V.ParameterInfo.Name.ToString(), JsonBuilders::BuildLinearColorJson(V.ParameterValue));
        }
        Overrides->SetObjectField(TEXT("vector"), VectorObj);

        TSharedPtr<FJsonObject> TextureObj = MakeShared<FJsonObject>();
        for (const FTextureParameterValue& V : Instance->TextureParameterValues)
        {
            TextureObj->SetStringField(
                V.ParameterInfo.Name.ToString(),
                JsonBuilders::GetObjectPathSafe(V.ParameterValue));
        }
        Overrides->SetObjectField(TEXT("texture"), TextureObj);

#if WITH_EDITORONLY_DATA
        // Editor-only API; the StaticSwitchParameters list is on the runtime struct,
        // StaticComponentMaskParameters lives under EditorOnly.
        FStaticParameterSet StaticParams = Instance->GetStaticParameters();

        TSharedPtr<FJsonObject> StaticSwitchObj = MakeShared<FJsonObject>();
        for (const FStaticSwitchParameter& P : StaticParams.StaticSwitchParameters)
        {
            if (P.bOverride)
            {
                StaticSwitchObj->SetBoolField(P.ParameterInfo.Name.ToString(), P.Value);
            }
        }
        Overrides->SetObjectField(TEXT("staticSwitch"), StaticSwitchObj);

        TSharedPtr<FJsonObject> StaticCompMaskObj = MakeShared<FJsonObject>();
        for (const FStaticComponentMaskParameter& P : StaticParams.EditorOnly.StaticComponentMaskParameters)
        {
            if (P.bOverride)
            {
                TSharedPtr<FJsonObject> Mask = MakeShared<FJsonObject>();
                Mask->SetBoolField(TEXT("r"), P.R);
                Mask->SetBoolField(TEXT("g"), P.G);
                Mask->SetBoolField(TEXT("b"), P.B);
                Mask->SetBoolField(TEXT("a"), P.A);
                StaticCompMaskObj->SetObjectField(P.ParameterInfo.Name.ToString(), Mask);
            }
        }
        Overrides->SetObjectField(TEXT("staticComponentMask"), StaticCompMaskObj);
#else
        Overrides->SetObjectField(TEXT("staticSwitch"), MakeShared<FJsonObject>());
        Overrides->SetObjectField(TEXT("staticComponentMask"), MakeShared<FJsonObject>());
#endif

        Root->SetObjectField(TEXT("overrides"), Overrides);
    }

    // ---- basePropertyOverrides ----
    {
        TSharedPtr<FJsonObject> BPOJson = MakeShared<FJsonObject>();
        AddBasePropertyOverrides(Instance->BasePropertyOverrides, BPOJson);
        Root->SetObjectField(TEXT("basePropertyOverrides"), BPOJson);
    }

    // ---- materialLayers ----
#if WITH_EDITORONLY_DATA
    {
        FMaterialLayersFunctions Layers;
        if (Instance->GetMaterialLayers(Layers))
        {
            const bool bHasContent =
                Layers.Layers.Num() > 0 ||
                Layers.Blends.Num() > 0 ||
                Layers.EditorOnly.LayerStates.Num() > 0 ||
                Layers.EditorOnly.LayerNames.Num() > 0;
            if (bHasContent)
            {
                TSharedPtr<FJsonObject> LayersJson = MakeShared<FJsonObject>();

                TArray<TSharedPtr<FJsonValue>> LayerPaths;
                for (UMaterialFunctionInterface* L : Layers.Layers)
                {
                    LayerPaths.Add(MakeShared<FJsonValueString>(JsonBuilders::GetObjectPathSafe(L)));
                }
                LayersJson->SetArrayField(TEXT("Layers"), LayerPaths);

                TArray<TSharedPtr<FJsonValue>> BlendPaths;
                for (UMaterialFunctionInterface* B : Layers.Blends)
                {
                    BlendPaths.Add(MakeShared<FJsonValueString>(JsonBuilders::GetObjectPathSafe(B)));
                }
                LayersJson->SetArrayField(TEXT("Blends"), BlendPaths);

                TArray<TSharedPtr<FJsonValue>> StatesArr;
                for (bool S : Layers.EditorOnly.LayerStates)
                {
                    StatesArr.Add(MakeShared<FJsonValueBoolean>(S));
                }
                LayersJson->SetArrayField(TEXT("LayerStates"), StatesArr);

                TArray<TSharedPtr<FJsonValue>> NamesArr;
                for (const FText& N : Layers.EditorOnly.LayerNames)
                {
                    NamesArr.Add(MakeShared<FJsonValueString>(N.ToString()));
                }
                LayersJson->SetArrayField(TEXT("LayerNames"), NamesArr);

                Root->SetObjectField(TEXT("materialLayers"), LayersJson);
            }
        }
    }
#endif

    // ---- parentLightmassSettings ----
    // The FLightmassMaterialInterfaceSettings struct is protected on UMaterialInterface;
    // only per-field getters are public. Emit the section only if at least one override
    // flag is set so we don't pollute every MIC with default-zeroed lightmass JSON.
    {
        const bool bAnyOverride =
            Instance->GetOverrideCastShadowAsMasked() ||
            Instance->GetOverrideEmissiveBoost() ||
            Instance->GetOverrideDiffuseBoost() ||
            Instance->GetOverrideExportResolutionScale();
        if (bAnyOverride)
        {
            TSharedPtr<FJsonObject> LM = MakeShared<FJsonObject>();
            LM->SetBoolField(TEXT("bOverrideCastShadowAsMasked"), Instance->GetOverrideCastShadowAsMasked());
            LM->SetBoolField(TEXT("CastShadowAsMasked"), Instance->GetCastShadowAsMasked());
            LM->SetBoolField(TEXT("bOverrideEmissiveBoost"), Instance->GetOverrideEmissiveBoost());
            LM->SetNumberField(TEXT("EmissiveBoost"), Instance->GetEmissiveBoost());
            LM->SetBoolField(TEXT("bOverrideDiffuseBoost"), Instance->GetOverrideDiffuseBoost());
            LM->SetNumberField(TEXT("DiffuseBoost"), Instance->GetDiffuseBoost());
            LM->SetBoolField(TEXT("bOverrideExportResolutionScale"), Instance->GetOverrideExportResolutionScale());
            LM->SetNumberField(TEXT("ExportResolutionScale"), Instance->GetExportResolutionScale());
            Root->SetObjectField(TEXT("parentLightmassSettings"), LM);
        }
    }

    // ---- nanitePassthrough ----
    {
        TSharedPtr<FJsonObject> Nan = MakeShared<FJsonObject>();
        Nan->SetBoolField(TEXT("bIsNaniteOverride"), Instance->NaniteOverrideMaterial.bEnableOverride);
        UMaterialInterface* NaniteMat = Instance->NaniteOverrideMaterial.GetOverrideMaterial();
        if (NaniteMat)
        {
            Nan->SetStringField(TEXT("NaniteOverrideMaterial"), NaniteMat->GetPathName());
        }
        else
        {
            Nan->SetField(TEXT("NaniteOverrideMaterial"), MakeShared<FJsonValueNull>());
        }
        Root->SetObjectField(TEXT("nanitePassthrough"), Nan);
    }

    // ---- physMaterial / physMaterialMask ----
    if (Instance->PhysMaterial)
    {
        Root->SetStringField(TEXT("physMaterial"), Instance->PhysMaterial->GetPathName());
    }
    else
    {
        Root->SetField(TEXT("physMaterial"), MakeShared<FJsonValueNull>());
    }

    // PhysicalMaterialMap is a fixed-size array indexed by mask color channel; flatten to an
    // object keyed by index for consumers that only care about non-null slots.
    {
        TSharedPtr<FJsonObject> PMM = MakeShared<FJsonObject>();
        bool bAny = false;
        for (int32 Idx = 0; Idx < EPhysicalMaterialMaskColor::MAX; ++Idx)
        {
            if (UPhysicalMaterial* PM = Instance->PhysicalMaterialMap[Idx])
            {
                PMM->SetStringField(FString::FromInt(Idx), PM->GetPathName());
                bAny = true;
            }
        }
        if (bAny)
        {
            Root->SetObjectField(TEXT("physMaterialMask"), PMM);
        }
        else
        {
            Root->SetField(TEXT("physMaterialMask"), MakeShared<FJsonValueNull>());
        }
    }

    // ---- subsurfaceProfile ----
    {
        // GetSubsurfaceProfile_Internal walks the parent chain; for the dump we want the
        // override-as-authored. SubsurfaceProfile is a UPROPERTY on UMaterialInterface.
        UObject* SSP = Instance->SubsurfaceProfile;
        if (SSP)
        {
            Root->SetStringField(TEXT("subsurfaceProfile"), SSP->GetPathName());
        }
        else
        {
            Root->SetField(TEXT("subsurfaceProfile"), MakeShared<FJsonValueNull>());
        }
    }

    // ---- flags ----
    {
        TSharedPtr<FJsonObject> Flags = MakeShared<FJsonObject>();
        Flags->SetBoolField(TEXT("bHasStaticPermutationResource"), Instance->bHasStaticPermutationResource != 0);
        Root->SetObjectField(TEXT("flags"), Flags);
    }

    return Root;
}

namespace
{
    UClass* GetMaterialInstanceSidecarClass()
    {
        return UMaterialInstanceConstant::StaticClass();
    }

    TSharedPtr<FJsonObject> BuildMaterialInstanceSidecar(UObject* Asset)
    {
        return MaterialInstanceDumpBuilder::BuildMaterialInstanceJson(Cast<UMaterialInstanceConstant>(Asset));
    }
}

REGISTER_DUMP_JSON_SIDECAR(TEXT("material_instance"), DumpFileNames::MaterialInstance,
    &GetMaterialInstanceSidecarClass, &BuildMaterialInstanceSidecar,
    nullptr, nullptr, TEXT("MaterialInstanceDumpBuilder returned null for MIC."), 100);
