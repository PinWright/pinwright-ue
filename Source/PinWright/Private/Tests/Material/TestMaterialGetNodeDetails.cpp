// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace
{
    // Creates a transient-package material under __PW_GatewayTests carrying a single
    // ScalarParameter named "Roughness" wired into the Roughness input, and registers
    // it with the asset registry. Returns the material (and the created param via
    // OutParam) on success, or nullptr after emitting the failing assertion + cleaning
    // up. AssetPathPrefix disambiguates the two tests' assets; both get-node-details
    // tests share this identical setup.
    UMaterial* CreateRoughnessParamMaterial(
        FAutomationTestBase& Test,
        const TCHAR* AssetPathPrefix,
        FString& OutAssetPath,
        UMaterialExpressionScalarParameter*& OutParam)
    {
        OutParam = nullptr;
        OutAssetPath = FString::Printf(
            TEXT("/Game/__PW_GatewayTests/%s_%s"),
            AssetPathPrefix,
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

        UMaterialExpressionScalarParameter* Param = NewObject<UMaterialExpressionScalarParameter>(Material);
        if (!Test.TestNotNull(TEXT("Scalar parameter created"), Param))
        {
            CleanupTestAsset(OutAssetPath);
            return nullptr;
        }

        Param->ParameterName = TEXT("Roughness");
        if (!Param->MaterialExpressionGuid.IsValid())
        {
            Param->MaterialExpressionGuid = FGuid::NewGuid();
        }

        Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Param);
        Material->GetEditorOnlyData()->Roughness.Expression = Param;
        Material->PostEditChange();
        Material->MarkPackageDirty();

        FAssetRegistryModule::AssetCreated(Material);

        OutParam = Param;
        return Material;
    }

    // Creates a transient-package material carrying a single TextureSampleParameter2D with a
    // non-default Group and SortPriority. That family declares its own Group/SortPriority but
    // does NOT derive from UMaterialExpressionParameter, which is precisely the case the
    // detail payload used to drop. The node is deliberately left without a texture (the same
    // unassigned shape TestMaterialSetTextureSampleTexture builds) — the metadata under test
    // does not depend on one.
    UMaterialExpressionTextureSampleParameter2D* CreateTextureParamMaterial(
        FAutomationTestBase& Test,
        const TCHAR* AssetPathPrefix,
        FString& OutAssetPath)
    {
        OutAssetPath = FString::Printf(
            TEXT("/Game/__PW_GatewayTests/%s_%s"),
            AssetPathPrefix,
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

        UMaterialExpressionTextureSampleParameter2D* Param =
            NewObject<UMaterialExpressionTextureSampleParameter2D>(Material);
        if (!Test.TestNotNull(TEXT("Texture parameter created"), Param))
        {
            CleanupTestAsset(OutAssetPath);
            return nullptr;
        }

        Param->ParameterName = TEXT("BaseColorTex");
        Param->Group = TEXT("Textures");
        Param->SortPriority = 7; // CDO default is 32, so this also lands in the reflected diff.
        if (!Param->MaterialExpressionGuid.IsValid())
        {
            Param->MaterialExpressionGuid = FGuid::NewGuid();
        }

        Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Param);
        Material->PostEditChange();
        Material->MarkPackageDirty();

        FAssetRegistryModule::AssetCreated(Material);

        return Param;
    }
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGetNodeDetailsPayloadTest,
    "PinWright.material.graph.get_node_details.PayloadFields",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialGetNodeDetailsPayloadTest::RunTest(const FString& Parameters)
{
    FString AssetPath;
    UMaterialExpressionScalarParameter* Param = nullptr;
    if (!CreateRoughnessParamMaterial(*this, TEXT("MatGetNode"), AssetPath, Param))
        return true;

    // This test additionally asserts group/default-value pass through into the
    // payload; the handler reflects them straight off the live expression object.
    Param->Group = TEXT("Surface");
    Param->DefaultValue = 0.5f;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);
    Payload->SetStringField(TEXT("nodeId"), Param->MaterialExpressionGuid.ToString());

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("material.graph.get_node_details"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    FString ParameterName;
    TestTrue(TEXT("parameterName field present"),
        Capture.Result->TryGetStringField(TEXT("parameterName"), ParameterName));
    TestEqual(TEXT("parameterName == Roughness"), ParameterName, FString(TEXT("Roughness")));

    FString Group;
    TestTrue(TEXT("group field present"),
        Capture.Result->TryGetStringField(TEXT("group"), Group));
    TestEqual(TEXT("group == Surface"), Group, FString(TEXT("Surface")));

    const TSharedPtr<FJsonObject>* Properties = nullptr;
    const bool bHasProperties = Capture.Result->TryGetObjectField(TEXT("properties"), Properties);
    TestTrue(TEXT("properties object present"), bHasProperties);
    if (bHasProperties && Properties && Properties->IsValid())
    {
        // BuildClassPropertyJson wraps each reflected property in {type, value, ...}.
        const TSharedPtr<FJsonObject>* DefaultValueObj = nullptr;
        const bool bHasDefault = (*Properties)->TryGetObjectField(TEXT("DefaultValue"), DefaultValueObj);
        TestTrue(TEXT("properties.DefaultValue present"), bHasDefault);
        if (bHasDefault && DefaultValueObj && DefaultValueObj->IsValid())
        {
            double DefaultValue = 0.0;
            const bool bHasValue = (*DefaultValueObj)->TryGetNumberField(TEXT("value"), DefaultValue);
            TestTrue(TEXT("properties.DefaultValue.value present"), bHasValue);
            if (bHasValue)
            {
                TestEqual(TEXT("properties.DefaultValue.value == 0.5"), DefaultValue, 0.5, 0.0001);
            }
        }
    }

    const TArray<TSharedPtr<FJsonValue>>* Outputs = nullptr;
    const bool bHasOutputs = Capture.Result->TryGetArrayField(TEXT("outputs"), Outputs);
    TestTrue(TEXT("outputs field present"), bHasOutputs);
    if (bHasOutputs && Outputs)
    {
        TestTrue(TEXT("outputs non-empty"), Outputs->Num() >= 1);
    }

    TestTrue(TEXT("inputs field present"), Capture.Result->HasField(TEXT("inputs")));

    CleanupTestAsset(AssetPath);
    return true;
}


// Regression: omitting nodeId is the documented list-all mode and must return a
// success envelope carrying {availableNodes, nodeCount} — not a NODE_NOT_FOUND
// error. Before the fix the handler built the node list and then discarded it
// through SendError, so this would fail on bSuccess and on the structured payload.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGetNodeDetailsListAllTest,
    "PinWright.material.graph.get_node_details.ListAllMode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialGetNodeDetailsListAllTest::RunTest(const FString& Parameters)
{
    FString AssetPath;
    UMaterialExpressionScalarParameter* Param = nullptr;
    if (!CreateRoughnessParamMaterial(*this, TEXT("MatListAll"), AssetPath, Param))
        return true;
    (void)Param; // list-all mode addresses the material by path, not the node.

    // No nodeId field -> documented list-all mode.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("material.graph.get_node_details"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);

    // The core regression assertion: list-all mode is a success, not NODE_NOT_FOUND.
    TestTrue(TEXT("List-all mode returns success (not isError)"), Capture.bSuccess);
    TestEqual(TEXT("No error code emitted for list-all mode"), Capture.ErrorCode, FString());

    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    // The freshly-built node list must reach the caller as the success payload.
    const TArray<TSharedPtr<FJsonValue>>* AvailableNodes = nullptr;
    const bool bHasNodes = Capture.Result->TryGetArrayField(TEXT("availableNodes"), AvailableNodes);
    TestTrue(TEXT("availableNodes field present"), bHasNodes);
    if (bHasNodes && AvailableNodes)
    {
        TestTrue(TEXT("availableNodes non-empty"), AvailableNodes->Num() >= 1);
    }

    double NodeCount = 0.0;
    const bool bHasCount = Capture.Result->TryGetNumberField(TEXT("nodeCount"), NodeCount);
    TestTrue(TEXT("nodeCount field present"), bHasCount);
    if (bHasCount)
    {
        TestTrue(TEXT("nodeCount >= 1"), NodeCount >= 1.0);
    }

    CleanupTestAsset(AssetPath);
    return true;
}


// Regression: the shared detail payload named a parameter it could not describe. Group and
// SortPriority were read through a Cast<UMaterialExpressionParameter>, which fails for every
// parameter family that carries its own Group/SortPriority outside that hierarchy — texture
// sample, font sample, runtime virtual texture, sparse volume texture — while the virtual
// HasAParameterName() still emitted parameterName for them. Both canonical detail handlers
// share MGIRExpressionUtils::BuildExpressionDetailsJson, so the same assertions must hold on
// each; asserting both also closes the parity gap where only the graph route had an
// ordinary-node payload test. Before the fix, group and sortPriority are absent from both
// responses even though parameterName is present.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGetNodeDetailsTextureParameterTest,
    "PinWright.material.graph.get_node_details.TextureParameterMetadata",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialGetNodeDetailsTextureParameterTest::RunTest(const FString& Parameters)
{
    FString AssetPath;
    UMaterialExpressionTextureSampleParameter2D* Param =
        CreateTextureParamMaterial(*this, TEXT("MatTexParam"), AssetPath);
    if (!Param)
        return true;

    const FString NodeId = Param->MaterialExpressionGuid.ToString();

    const TCHAR* Methods[] = {
        TEXT("material.graph.get_node_details"),
        TEXT("material.authoring.get_material_node_details")
    };

    for (const TCHAR* Method : Methods)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("nodeId"), NodeId);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(Method, Payload, Capture);
        TestTrue(*FString::Printf(TEXT("[%s] handler found"), Method), bFound);
        TestTrue(*FString::Printf(TEXT("[%s] handler succeeded"), Method), Capture.bSuccess);

        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            continue;
        }

        FString ParameterName;
        TestTrue(*FString::Printf(TEXT("[%s] parameterName present"), Method),
            Capture.Result->TryGetStringField(TEXT("parameterName"), ParameterName));
        TestEqual(*FString::Printf(TEXT("[%s] parameterName == BaseColorTex"), Method),
            ParameterName, FString(TEXT("BaseColorTex")));

        // The two fields the cast dropped for this whole family.
        FString Group;
        const bool bHasGroup = Capture.Result->TryGetStringField(TEXT("group"), Group);
        TestTrue(*FString::Printf(TEXT("[%s] group present"), Method), bHasGroup);
        if (bHasGroup)
        {
            TestEqual(*FString::Printf(TEXT("[%s] group == Textures"), Method),
                Group, FString(TEXT("Textures")));
        }

        double SortPriority = 0.0;
        const bool bHasSortPriority =
            Capture.Result->TryGetNumberField(TEXT("sortPriority"), SortPriority);
        TestTrue(*FString::Printf(TEXT("[%s] sortPriority present"), Method), bHasSortPriority);
        if (bHasSortPriority)
        {
            TestEqual(*FString::Printf(TEXT("[%s] sortPriority == 7"), Method),
                SortPriority, 7.0, 0.0001);
        }

        // The non-default SortPriority must also survive the reflected non-default diff, and
        // the pin arrays are what make the payload usable for rewiring.
        const TSharedPtr<FJsonObject>* Properties = nullptr;
        const bool bHasProperties = Capture.Result->TryGetObjectField(TEXT("properties"), Properties);
        TestTrue(*FString::Printf(TEXT("[%s] properties object present"), Method), bHasProperties);
        if (bHasProperties && Properties && Properties->IsValid())
        {
            TestTrue(*FString::Printf(TEXT("[%s] properties.SortPriority present"), Method),
                (*Properties)->HasField(TEXT("SortPriority")));
        }

        TestTrue(*FString::Printf(TEXT("[%s] inputs present"), Method),
            Capture.Result->HasField(TEXT("inputs")));
        TestTrue(*FString::Printf(TEXT("[%s] outputs present"), Method),
            Capture.Result->HasField(TEXT("outputs")));
    }

    CleanupTestAsset(AssetPath);
    return true;
}
