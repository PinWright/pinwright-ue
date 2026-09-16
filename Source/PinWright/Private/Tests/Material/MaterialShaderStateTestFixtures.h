// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "AssetRegistry/AssetRegistryModule.h"
#include "Compat/EngineVersionCompat.h"
#include "Handlers/Material/MaterialUsageFlags.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace PinWrightMaterialShaderStateTestFixtures
{
    // Declare one usage on a base material, without the recompile UMaterial::SetMaterialUsage
    // would run.
    //
    // UMaterial::SetUsageByFlag is public from UE 5.8, the release that also gave an instance its
    // own overridable usage flags; before that the setter is private and the flag it writes is the
    // public `bUsedWith*` UPROPERTY the read path already resolves, so the pre-5.8 route writes
    // that property. Both spellings set the same member and neither recompiles.
    inline bool DeclareMaterialUsage(UMaterial* Material, EMaterialUsage Usage)
    {
        if (!Material)
        {
            return false;
        }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        Material->SetUsageByFlag(Usage, true);
        return true;
#else
        const FString PropertyName = PinWright::MaterialUsage::FindUsagePropertyName(Usage);
        FBoolProperty* Flag = PropertyName.IsEmpty()
            ? nullptr
            : FindFProperty<FBoolProperty>(UMaterial::StaticClass(), FName(*PropertyName));
        if (!Flag)
        {
            return false;
        }
        Flag->SetPropertyValue_InContainer(Material, true);
        return true;
#endif
    }

    inline UMaterial* MakeSandboxMaterial(const TCHAR* NameStem, FString& OutAssetPath)
    {
        OutAssetPath = FString::Printf(TEXT("/Game/__PW_GatewayTests/%s_%s"),
            NameStem, *FGuid::NewGuid().ToString(EGuidFormats::Digits));

        UPackage* Package = CreatePackage(*OutAssetPath);
        return Package
            ? NewObject<UMaterial>(Package,
                FName(*FPackageName::GetLongPackageAssetName(OutAssetPath)),
                RF_Public | RF_Standalone)
            : nullptr;
    }

    inline UMaterial* MakeBrokenHlslMaterial(const TCHAR* NameStem, FString& OutAssetPath)
    {
        UMaterial* Material = MakeSandboxMaterial(NameStem, OutAssetPath);
        if (!Material)
        {
            return nullptr;
        }

        UMaterialExpressionCustom* Custom = NewObject<UMaterialExpressionCustom>(Material);
        if (!Custom)
        {
            return nullptr;
        }
        Custom->MaterialExpressionGuid = FGuid::NewGuid();
        Custom->OutputType = ECustomMaterialOutputType::CMOT_Float3;
        Custom->Code = TEXT("return GetPrimitiveData(Parameters).LocalToWorld[2].xyz;");
        Custom->Description = TEXT("PW_InvalidHLSL");
        Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Custom);

        UMaterialExpressionConstant3Vector* CleanBranch =
            NewObject<UMaterialExpressionConstant3Vector>(Material);
        UMaterialExpressionStaticSwitchParameter* Switch =
            NewObject<UMaterialExpressionStaticSwitchParameter>(Material);
        if (!CleanBranch || !Switch)
        {
            return nullptr;
        }
        CleanBranch->MaterialExpressionGuid = FGuid::NewGuid();
        CleanBranch->Constant = FLinearColor::Black;
        Switch->MaterialExpressionGuid = FGuid::NewGuid();
        Switch->ParameterName = TEXT("UseBrokenHlsl");
        Switch->DefaultValue = true;
        Switch->A.Expression = Custom;
        Switch->B.Expression = CleanBranch;
        Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(CleanBranch);
        Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Switch);
        Material->GetEditorOnlyData()->EmissiveColor.Expression = Switch;
        Material->GetEditorOnlyData()->EmissiveColor.OutputIndex = 0;
        Material->PostEditChange();
        FAssetRegistryModule::AssetCreated(Material);
        return Material;
    }

    inline UMaterial* MakeCleanMaterial(const TCHAR* NameStem, FString& OutAssetPath)
    {
        UMaterial* Material = MakeSandboxMaterial(NameStem, OutAssetPath);
        if (!Material)
        {
            return nullptr;
        }

        UMaterialExpressionConstant3Vector* Colour =
            NewObject<UMaterialExpressionConstant3Vector>(Material);
        if (!Colour)
        {
            return nullptr;
        }
        Colour->MaterialExpressionGuid = FGuid::NewGuid();
        Colour->Constant = FLinearColor(0.2f, 0.4f, 0.6f);
        Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Colour);
        Material->GetEditorOnlyData()->BaseColor.Expression = Colour;
        Material->GetEditorOnlyData()->BaseColor.OutputIndex = 0;
        Material->PostEditChange();
        FAssetRegistryModule::AssetCreated(Material);
        return Material;
    }

    // A parented instance with no overrides: bHasStaticPermutationResource is false, so it owns no
    // shader and GetMaterialResource forwards to the parent. Give it a differing base-property
    // override and PostEditChange to flip it, which is the state a static-switch override creates.
    inline UMaterialInstanceConstant* MakeMaterialInstance(UMaterialInterface* Parent,
        const TCHAR* NameStem, FString& OutAssetPath)
    {
        OutAssetPath = FString::Printf(TEXT("/Game/__PW_GatewayTests/%s_%s"),
            NameStem, *FGuid::NewGuid().ToString(EGuidFormats::Digits));

        UPackage* Package = CreatePackage(*OutAssetPath);
        if (!Package || !Parent)
        {
            return nullptr;
        }

        UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(Package,
            FName(*FPackageName::GetLongPackageAssetName(OutAssetPath)),
            RF_Public | RF_Standalone);
        if (Instance)
        {
            Instance->SetParentEditorOnly(Parent);
            FAssetRegistryModule::AssetCreated(Instance);
        }
        return Instance;
    }
}
