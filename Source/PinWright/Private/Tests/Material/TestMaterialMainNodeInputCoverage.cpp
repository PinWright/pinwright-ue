// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialMainNodeInputCoverageTest,
    "PinWright.material.graph.connect_nodes.MainNodeInputCoverage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialMainNodeInputCoverageTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/MainNodeInputs_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    UPackage* Pkg = CreatePackage(*AssetPath);
    if (!TestNotNull(TEXT("Package created"), Pkg))
        return true;

    UMaterial* Material = NewObject<UMaterial>(
        Pkg,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        RF_Public | RF_Standalone);

    if (!TestNotNull(TEXT("Material created"), Material))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    UMaterialExpressionScalarParameter* ScalarExpr = NewObject<UMaterialExpressionScalarParameter>(Material);
    if (!TestNotNull(TEXT("Scalar parameter created"), ScalarExpr))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }
    if (!ScalarExpr->MaterialExpressionGuid.IsValid())
    {
        ScalarExpr->MaterialExpressionGuid = FGuid::NewGuid();
    }
    Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(ScalarExpr);
    Material->PostEditChange();
    FAssetRegistryModule::AssetCreated(Material);

    UMaterialEditorOnlyData* D = Material->GetEditorOnlyData();
    const FString SourceGuid = ScalarExpr->MaterialExpressionGuid.ToString();

    auto ConnectAndCheck = [&](const TCHAR* InputName, FExpressionInput& TargetInput)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("sourceNodeId"), SourceGuid);
        Payload->SetStringField(TEXT("targetNodeId"), FString());
        Payload->SetStringField(TEXT("inputName"), InputName);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("material.graph.connect_nodes"), Payload, Capture);
        TestTrue(FString::Printf(TEXT("Handler found for %s"), InputName), bFound);
        TestTrue(FString::Printf(TEXT("connect_nodes succeeded for %s"), InputName), Capture.bSuccess);
        TestEqual(FString::Printf(TEXT("%s wired to ScalarExpr"), InputName),
            TargetInput.Expression, static_cast<UMaterialExpression*>(ScalarExpr));
    };

    ConnectAndCheck(TEXT("ClearCoat"), D->ClearCoat);
    ConnectAndCheck(TEXT("Refraction"), D->Refraction);
    ConnectAndCheck(TEXT("Anisotropy"), D->Anisotropy);
    ConnectAndCheck(TEXT("PixelDepthOffset"), D->PixelDepthOffset);
    ConnectAndCheck(TEXT("Displacement"), D->Displacement);
    ConnectAndCheck(TEXT("WorldPositionOffset"), D->WorldPositionOffset);
    ConnectAndCheck(TEXT("CustomizedUVs[0]"), D->CustomizedUVs[0]);
    ConnectAndCheck(TEXT("CustomizedUVs[7]"), D->CustomizedUVs[7]);

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("sourceNodeId"), SourceGuid);
        Payload->SetStringField(TEXT("targetNodeId"), FString());
        Payload->SetStringField(TEXT("inputName"), TEXT("CustomizedUVs[8]"));

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("material.graph.connect_nodes"), Payload, Capture);
        TestTrue(TEXT("Handler found for out-of-range CustomizedUVs"), bFound);
        TestFalse(TEXT("CustomizedUVs[8] rejected"), Capture.bSuccess);
        TestEqual(TEXT("CustomizedUVs[8] ErrorCode == INVALID_PIN"),
            Capture.ErrorCode, FString(TEXT("INVALID_PIN")));
    }

    CleanupTestAsset(AssetPath);
    return true;
}
