// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

#include "AssetDumpFixtureHelpers.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialInstanceConstant.h"
// FMaterialParameterInfo et al. moved from the top-level MaterialTypes.h into
// Materials/MaterialParameters.h in UE 5.7; on 5.6 and earlier they live in
// MaterialTypes.h, which has no MaterialParameters.h (and is deprecated on 5.8).
#if __has_include("Materials/MaterialParameters.h")
#include "Materials/MaterialParameters.h"
#else
#include "MaterialTypes.h"
#endif

namespace
{
    using AssetDumpFixtureHelpers::LoadJsonFile;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpMaterialInstanceSidecarTest,
    "PinWright.asset.dump.MaterialInstanceSidecar",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpMaterialInstanceSidecarTest::RunTest(const FString& Parameters)
{
    const FString Suffix      = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString ParentPath  = FString::Printf(TEXT("/Engine/Transient/M_TestParent_MIC_%s"), *Suffix);
    const FString InstancePath = FString::Printf(TEXT("/Engine/Transient/MI_TestChild_MIC_%s"), *Suffix);
    const FString ScratchRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir())
        / TEXT("AssetDumpTest_MIC") / Suffix;
    // Both fixtures are RF_Standalone and are published with FAssetRegistryModule::AssetCreated
    // below; without the matching AssetDeleted + detach they survive the periodic suite GC and
    // stay visible to a later /Engine/Transient asset-registry rescan. Instance first: it holds
    // the parent reference.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(InstancePath);
        CleanupTestAsset(ParentPath);
    };

    // ---- Build parent UMaterial with a Roughness scalar param (default 0.5) ----
    UPackage* ParentPkg = CreatePackage(*ParentPath);
    if (!TestNotNull(TEXT("Parent package created"), ParentPkg))
    {
        return true;
    }

    UMaterial* ParentMat = NewObject<UMaterial>(
        ParentPkg,
        FName(*FPackageName::GetLongPackageAssetName(ParentPath)),
        RF_Public | RF_Standalone | RF_Transient);
    if (!TestNotNull(TEXT("Parent material created"), ParentMat))
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
        return true;
    }

    UMaterialExpressionScalarParameter* ScalarParam =
        NewObject<UMaterialExpressionScalarParameter>(ParentMat);
    if (!TestNotNull(TEXT("Scalar parameter created"), ScalarParam))
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
        return true;
    }
    ScalarParam->ParameterName = TEXT("Roughness");
    ScalarParam->DefaultValue  = 0.5f;
    if (!ScalarParam->MaterialExpressionGuid.IsValid())
    {
        ScalarParam->MaterialExpressionGuid = FGuid::NewGuid();
    }
    ParentMat->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(ScalarParam);
    ParentMat->PostEditChange();
    FAssetRegistryModule::AssetCreated(ParentMat);

    // ---- Build child MIC via the factory used by editor authoring ----
    UPackage* InstPkg = CreatePackage(*InstancePath);
    if (!TestNotNull(TEXT("Instance package created"), InstPkg))
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
        return true;
    }

    UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
    Factory->InitialParent = ParentMat;
    UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(
        Factory->FactoryCreateNew(
            UMaterialInstanceConstant::StaticClass(),
            InstPkg,
            FName(*FPackageName::GetLongPackageAssetName(InstancePath)),
            RF_Public | RF_Standalone | RF_Transient,
            nullptr,
            GWarn));
    if (!TestNotNull(TEXT("Instance created"), Instance))
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
        return true;
    }
    Instance->SetParentEditorOnly(ParentMat);

    // ---- Set the overrides we'll assert on ----
    Instance->SetScalarParameterValueEditorOnly(
        FMaterialParameterInfo(FName(TEXT("Roughness"))), 0.9f);
    Instance->BasePropertyOverrides.bOverride_OpacityMaskClipValue = 1;
    Instance->BasePropertyOverrides.OpacityMaskClipValue = 0.3333f;
    Instance->BasePropertyOverrides.bOverride_BlendMode = 1;
    Instance->BasePropertyOverrides.BlendMode = BLEND_Masked;
    Instance->PostEditChange();
    FAssetRegistryModule::AssetCreated(Instance);

    // ---- Dump via the production entry point ----
    const FString ObjectPath = FString::Printf(
        TEXT("%s.%s"),
        *InstancePath,
        *FPackageName::GetLongPackageAssetName(InstancePath));

    AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot);

    // Hard failure, not a skip: the fixture is synthesized in-test, so there is no
    // environment on which a dump error is expected. Passing on an error code made the
    // whole test a no-op the moment DumpSingleAsset stopped resolving the in-memory
    // /Engine/Transient package (AssetDumpHandler.cpp's FindPackage escape hatch ahead of
    // DoesPackageExist) — every assertion below would have been silently skipped.
    if (!TestTrue(TEXT("Dump succeeds"), Result.ErrorCode.IsEmpty()))
    {
        AddError(FString::Printf(TEXT("DumpSingleAsset failed (%s: %s)."),
            *Result.ErrorCode, *Result.ErrorMessage));
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
        return false;
    }

    // ---- Assertion 1: file exists (counterfactual hinge) ----
    const FString SidecarPath = Result.DumpDir / DumpFileNames::MaterialInstance;
    if (!TestTrue(TEXT("material_instance.json exists"),
        IFileManager::Get().FileExists(*SidecarPath)))
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
        return true;
    }

    TSharedPtr<FJsonObject> MicJson = LoadJsonFile(SidecarPath);
    if (!TestTrue(TEXT("material_instance.json parsed"), MicJson.IsValid()))
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
        return true;
    }

    // ---- Assertion 2: parent path matches ----
    FString ParentValue;
    TestTrue(TEXT("parent field present"),
        MicJson->TryGetStringField(TEXT("parent"), ParentValue));
    TestEqual(TEXT("parent equals parent material path"),
        ParentValue, ParentMat->GetPathName());

    // ---- Assertion 3: overrides.scalar.Roughness ----
    const TSharedPtr<FJsonObject>* OverridesObj = nullptr;
    if (TestTrue(TEXT("overrides field present"),
        MicJson->TryGetObjectField(TEXT("overrides"), OverridesObj)) && OverridesObj)
    {
        const TSharedPtr<FJsonObject>* ScalarObj = nullptr;
        if (TestTrue(TEXT("overrides.scalar present"),
            (*OverridesObj)->TryGetObjectField(TEXT("scalar"), ScalarObj)) && ScalarObj)
        {
            double ScalarVal = 0.0;
            TestTrue(TEXT("overrides.scalar.Roughness present"),
                (*ScalarObj)->TryGetNumberField(TEXT("Roughness"), ScalarVal));
            TestEqual(TEXT("overrides.scalar.Roughness == 0.9"),
                ScalarVal, 0.9, 0.0001);
        }
    }

    // ---- Assertion 4: basePropertyOverrides fields ----
    const TSharedPtr<FJsonObject>* BPOObj = nullptr;
    if (TestTrue(TEXT("basePropertyOverrides field present"),
        MicJson->TryGetObjectField(TEXT("basePropertyOverrides"), BPOObj)) && BPOObj)
    {
        bool bOpacityOverride = false;
        TestTrue(TEXT("bOverride_OpacityMaskClipValue present"),
            (*BPOObj)->TryGetBoolField(TEXT("bOverride_OpacityMaskClipValue"), bOpacityOverride));
        TestTrue(TEXT("bOverride_OpacityMaskClipValue == true"), bOpacityOverride);

        double Clip = 0.0;
        TestTrue(TEXT("OpacityMaskClipValue present"),
            (*BPOObj)->TryGetNumberField(TEXT("OpacityMaskClipValue"), Clip));
        TestTrue(TEXT("OpacityMaskClipValue ~ 0.3333"),
            FMath::IsNearlyEqual((float)Clip, 0.3333f, 0.001f));

        FString BlendModeStr;
        TestTrue(TEXT("BlendMode present"),
            (*BPOObj)->TryGetStringField(TEXT("BlendMode"), BlendModeStr));
        TestTrue(TEXT("BlendMode contains BLEND_Masked"),
            BlendModeStr.Contains(TEXT("BLEND_Masked")));
    }

    IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
    return true;
}
