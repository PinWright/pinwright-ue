// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for E-material-main-output-no-node-readback:
// the documented "Main" sentinel that connect_nodes / break_connections accept must
// also be inspectable through the read RPCs, and get_material_info must surface the
// shading model + main-node input wiring. Before the fix:
//   * material.authoring.get_material_node_details(nodeId:"Main")   -> NOT_FOUND
//   * material.graph.get_node_details(nodeId:"Main")                -> NODE_NOT_FOUND
//   * get_material_info omitted both shadingModel and the main-node inputs
// so "is BaseColor/Roughness/... wired?" had no node-detail readback route.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace
{
    // Builds a transient material under __PW_GatewayTests with a single ScalarParameter
    // wired into the main node's Roughness input (exactly what connect_nodes("Main") would
    // produce) and DefaultLit shading. Returns the material and the wired param's GUID; the
    // main node should now be readable back through the "Main" sentinel.
    UMaterial* CreateMaterialWithWiredRoughness(
        FAutomationTestBase& Test,
        FString& OutAssetPath,
        FString& OutParamGuid)
    {
        OutAssetPath = FString::Printf(
            TEXT("/Game/__PW_GatewayTests/MatMainReadback_%s"),
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
        OutParamGuid = Param->MaterialExpressionGuid.ToString();

        Material->SetShadingModel(MSM_DefaultLit);
        Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Param);
        Material->GetEditorOnlyData()->Roughness.Expression = Param;
        Material->PostEditChange();
        Material->MarkPackageDirty();

        FAssetRegistryModule::AssetCreated(Material);
        return Material;
    }

    // Shared payload-shape assertions for the Main node-detail readback (both read RPCs
    // emit the same BuildMainNodeDetailsJson shape).
    void AssertMainNodeDetails(
        FAutomationTestBase& Test,
        const TSharedPtr<FJsonObject>& Result,
        const FString& ExpectedParamGuid)
    {
        FString NodeId;
        Test.TestTrue(TEXT("nodeId present"), Result->TryGetStringField(TEXT("nodeId"), NodeId));
        Test.TestEqual(TEXT("nodeId == Main"), NodeId, FString(TEXT("Main")));

        bool bIsMain = false;
        Test.TestTrue(TEXT("isMainOutput present"), Result->TryGetBoolField(TEXT("isMainOutput"), bIsMain));
        Test.TestTrue(TEXT("isMainOutput == true"), bIsMain);

        FString ShadingModel;
        Test.TestTrue(TEXT("shadingModel present"), Result->TryGetStringField(TEXT("shadingModel"), ShadingModel));
        Test.TestEqual(TEXT("shadingModel == DefaultLit"), ShadingModel, FString(TEXT("DefaultLit")));

        // The wired Roughness input must be reported with the connected expression's GUID.
        Test.TestTrue(TEXT("inputs contains a Roughness entry"),
            JsonArrayHasObjectWithStringField(Result, TEXT("inputs"), TEXT("name"), TEXT("Roughness")));
        Test.TestTrue(TEXT("inputs Roughness wired to the param GUID"),
            JsonArrayHasObjectWithStringField(Result, TEXT("inputs"), TEXT("connectedNodeId"), ExpectedParamGuid));
    }
}


// Regression: material.graph.get_node_details(nodeId:"Main") must resolve the main output
// node (success), not NODE_NOT_FOUND. Reverting the sentinel branch fails this on bSuccess.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphMainNodeReadbackTest,
    "PinWright.material.graph.get_node_details.MainSentinel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialGraphMainNodeReadbackTest::RunTest(const FString& Parameters)
{
    FString AssetPath, ParamGuid;
    UMaterial* Material = CreateMaterialWithWiredRoughness(*this, AssetPath, ParamGuid);
    if (!Material)
        return true;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);
    Payload->SetStringField(TEXT("nodeId"), TEXT("Main"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("material.graph.get_node_details"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Main sentinel resolves to success (not NODE_NOT_FOUND)"), Capture.bSuccess);
    TestEqual(TEXT("No error code for Main sentinel"), Capture.ErrorCode, FString());

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        AssertMainNodeDetails(*this, Capture.Result, ParamGuid);
    }

    CleanupTestAsset(AssetPath);
    return true;
}


// Regression: material.authoring.get_material_node_details(nodeId:"Main") must resolve the
// main output node (success), not NOT_FOUND. Same sentinel, sibling read namespace.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringMainNodeReadbackTest,
    "PinWright.material.authoring.get_material_node_details.MainSentinel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialAuthoringMainNodeReadbackTest::RunTest(const FString& Parameters)
{
    FString AssetPath, ParamGuid;
    UMaterial* Material = CreateMaterialWithWiredRoughness(*this, AssetPath, ParamGuid);
    if (!Material)
        return true;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);
    Payload->SetStringField(TEXT("nodeId"), TEXT("Main"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("material.authoring.get_material_node_details"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Main sentinel resolves to success (not NOT_FOUND)"), Capture.bSuccess);
    TestEqual(TEXT("No error code for Main sentinel"), Capture.ErrorCode, FString());

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        AssertMainNodeDetails(*this, Capture.Result, ParamGuid);
    }

    CleanupTestAsset(AssetPath);
    return true;
}


// Regression: get_material_info must surface the shadingModel (settable at create) and the
// main-node input wiring, so the create/get param surfaces are symmetric and the caller can
// answer "what feeds Roughness?" off the same readback.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialInfoShadingModelAndMainInputsTest,
    "PinWright.material.authoring.get_material_info.ShadingModelAndMainInputs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialInfoShadingModelAndMainInputsTest::RunTest(const FString& Parameters)
{
    FString AssetPath, ParamGuid;
    UMaterial* Material = CreateMaterialWithWiredRoughness(*this, AssetPath, ParamGuid);
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
        FString ShadingModel;
        TestTrue(TEXT("shadingModel field present"),
            Capture.Result->TryGetStringField(TEXT("shadingModel"), ShadingModel));
        TestEqual(TEXT("shadingModel == DefaultLit"), ShadingModel, FString(TEXT("DefaultLit")));

        TestTrue(TEXT("mainInputs field present"), Capture.Result->HasField(TEXT("mainInputs")));
        TestTrue(TEXT("mainInputs contains a Roughness entry"),
            JsonArrayHasObjectWithStringField(Capture.Result, TEXT("mainInputs"), TEXT("name"), TEXT("Roughness")));
        TestTrue(TEXT("mainInputs Roughness wired to the param GUID"),
            JsonArrayHasObjectWithStringField(Capture.Result, TEXT("mainInputs"), TEXT("connectedNodeId"), ParamGuid));
    }

    CleanupTestAsset(AssetPath);
    return true;
}
