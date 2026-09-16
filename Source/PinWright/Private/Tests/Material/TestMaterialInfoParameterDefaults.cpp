// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-get-material-info-no-param-defaults:
// material.authoring.get_material_info's parameters[] array must echo each parameter's
// authored default value plus its group/sortPriority metadata, so the author-then-verify
// round-trip can read back what add_scalar_parameter / add_vector_parameter /
// add_static_switch_parameter wrote. Before the fix each entry carried only
// {name, type, nodeId}; defaults/group/sortPriority were dropped, and the caller had to
// fall back to N per-node get_material_node_details calls to confirm them.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace
{
    // Builds a transient material with three parameters carrying explicit, non-zero
    // defaults and groups — exactly what add_*_parameter would store — so get_material_info
    // has something it must read back beyond name/type/nodeId.
    //   Roughness    : scalar default 0.4, group "Appearance", sortPriority 10
    //   BaseTint     : vector default (0.9,0.6,0.3,1), group "Appearance", sortPriority 20
    //   UseEmissive  : static switch default true, group "FX", sortPriority 30 (no group asserted here)
    UMaterial* CreateMaterialWithDefaultedParams(FAutomationTestBase& Test, FString& OutAssetPath)
    {
        OutAssetPath = FString::Printf(
            TEXT("/Game/__PW_GatewayTests/MatParamDefaults_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));

        UPackage* Pkg = CreatePackage(*OutAssetPath);
        if (!Test.TestNotNull(TEXT("Package created"), Pkg))
            return nullptr;

        UMaterial* Material = NewObject<UMaterial>(
            Pkg,
            FName(*FPackageName::GetLongPackageAssetName(OutAssetPath)),
            RF_Public | RF_Standalone);
        if (!Test.TestNotNull(TEXT("Material created"), Material))
        {
            CleanupTestAsset(OutAssetPath);
            return nullptr;
        }

        UMaterialExpressionScalarParameter* Scalar = NewObject<UMaterialExpressionScalarParameter>(Material);
        Scalar->ParameterName = TEXT("Roughness");
        Scalar->DefaultValue = 0.4f;
        Scalar->Group = TEXT("Appearance");
        Scalar->SortPriority = 10;
        Scalar->MaterialExpressionGuid = FGuid::NewGuid();

        UMaterialExpressionVectorParameter* Vector = NewObject<UMaterialExpressionVectorParameter>(Material);
        Vector->ParameterName = TEXT("BaseTint");
        Vector->DefaultValue = FLinearColor(0.9f, 0.6f, 0.3f, 1.0f);
        Vector->Group = TEXT("Appearance");
        Vector->SortPriority = 20;
        Vector->MaterialExpressionGuid = FGuid::NewGuid();

        UMaterialExpressionStaticSwitchParameter* Switch = NewObject<UMaterialExpressionStaticSwitchParameter>(Material);
        Switch->ParameterName = TEXT("UseEmissive");
        Switch->DefaultValue = true;
        Switch->Group = TEXT("FX");
        Switch->SortPriority = 30;
        Switch->MaterialExpressionGuid = FGuid::NewGuid();

        Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Scalar);
        Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Vector);
        Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Switch);
        Material->PostEditChange();
        Material->MarkPackageDirty();

        FAssetRegistryModule::AssetCreated(Material);
        return Material;
    }
}


// Regression: get_material_info must echo each parameter's typed defaultValue + group +
// sortPriority. Reverting the AddMaterialParameterDetails call leaves only name/type/nodeId
// and fails every defaultValue/group/sortPriority assertion below.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialInfoParameterDefaultsTest,
    "PinWright.material.authoring.get_material_info.ParameterDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialInfoParameterDefaultsTest::RunTest(const FString& Parameters)
{
    FString AssetPath;
    UMaterial* Material = CreateMaterialWithDefaultedParams(*this, AssetPath);
    if (!Material)
        return true;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("material.authoring.get_material_info"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("get_material_info succeeded"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        // Scalar: defaultValue is a number, plus group/sortPriority.
        TSharedPtr<FJsonObject> ScalarEntry = JsonArrayFindObjectByStringField(
            Capture.Result, TEXT("parameters"), TEXT("name"), TEXT("Roughness"));
        if (TestNotNull(TEXT("Roughness parameter entry present"), ScalarEntry.Get()))
        {
            double ScalarDefault = 0.0;
            TestTrue(TEXT("Roughness defaultValue present"),
                ScalarEntry->TryGetNumberField(TEXT("defaultValue"), ScalarDefault));
            TestEqual(TEXT("Roughness defaultValue == 0.4"), ScalarDefault, 0.4, 0.0001);

            FString ScalarGroup;
            TestTrue(TEXT("Roughness group present"),
                ScalarEntry->TryGetStringField(TEXT("group"), ScalarGroup));
            TestEqual(TEXT("Roughness group == Appearance"), ScalarGroup, FString(TEXT("Appearance")));

            double ScalarSort = 0.0;
            TestTrue(TEXT("Roughness sortPriority present"),
                ScalarEntry->TryGetNumberField(TEXT("sortPriority"), ScalarSort));
            TestEqual(TEXT("Roughness sortPriority == 10"), (int32)ScalarSort, 10);
        }

        // Vector: defaultValue is a {r,g,b,a} object.
        TSharedPtr<FJsonObject> VectorEntry = JsonArrayFindObjectByStringField(
            Capture.Result, TEXT("parameters"), TEXT("name"), TEXT("BaseTint"));
        if (TestNotNull(TEXT("BaseTint parameter entry present"), VectorEntry.Get()))
        {
            const TSharedPtr<FJsonObject>* VectorDefault = nullptr;
            if (TestTrue(TEXT("BaseTint defaultValue is an object"),
                    VectorEntry->TryGetObjectField(TEXT("defaultValue"), VectorDefault) && VectorDefault))
            {
                double R = 0.0, G = 0.0, B = 0.0;
                (*VectorDefault)->TryGetNumberField(TEXT("r"), R);
                (*VectorDefault)->TryGetNumberField(TEXT("g"), G);
                (*VectorDefault)->TryGetNumberField(TEXT("b"), B);
                TestEqual(TEXT("BaseTint defaultValue.r == 0.9"), R, 0.9, 0.0001);
                TestEqual(TEXT("BaseTint defaultValue.g == 0.6"), G, 0.6, 0.0001);
                TestEqual(TEXT("BaseTint defaultValue.b == 0.3"), B, 0.3, 0.0001);
            }
        }

        // Static switch: defaultValue is a bool.
        TSharedPtr<FJsonObject> SwitchEntry = JsonArrayFindObjectByStringField(
            Capture.Result, TEXT("parameters"), TEXT("name"), TEXT("UseEmissive"));
        if (TestNotNull(TEXT("UseEmissive parameter entry present"), SwitchEntry.Get()))
        {
            bool SwitchDefault = false;
            TestTrue(TEXT("UseEmissive defaultValue present"),
                SwitchEntry->TryGetBoolField(TEXT("defaultValue"), SwitchDefault));
            TestTrue(TEXT("UseEmissive defaultValue == true"), SwitchDefault);
        }
    }

    CleanupTestAsset(AssetPath);
    return true;
}
