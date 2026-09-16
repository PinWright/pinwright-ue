// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace PinWrightMaterialTestHelpers
{
    template <typename AssetType>
    inline AssetType* CreateFixtureAsset(FAutomationTestBase& Test, const TCHAR* Prefix,
        FString& OutPackagePath)
    {
        OutPackagePath = FString::Printf(TEXT("/Game/__PW_GatewayTests/%s_%s"),
            Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        UPackage* Package = CreatePackage(*OutPackagePath);
        if (!Test.TestNotNull(TEXT("Fixture package created"), Package))
        {
            return nullptr;
        }

        const FName AssetName(*FPackageName::GetLongPackageAssetName(OutPackagePath));
        AssetType* Asset = NewObject<AssetType>(Package, AssetName, RF_Public | RF_Standalone);
        if (!Test.TestNotNull(TEXT("Fixture asset created"), Asset))
        {
            CleanupTestAsset(OutPackagePath);
            return nullptr;
        }
        FAssetRegistryModule::AssetCreated(Asset);
        return Asset;
    }

    inline int32 ExpressionCount(const UMaterial* Material)
    {
        return Material && Material->GetEditorOnlyData()
            ? Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Num()
            : 0;
    }

    inline int32 ExpressionCount(const UMaterialFunction* Function)
    {
        return Function && Function->GetEditorOnlyData()
            ? Function->GetEditorOnlyData()->ExpressionCollection.Expressions.Num()
            : 0;
    }
}
