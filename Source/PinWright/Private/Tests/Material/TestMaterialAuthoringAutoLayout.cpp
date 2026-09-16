// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for material.authoring.auto_layout.
//
// Builds a material in a sandbox package with two UMaterialExpressionConstant
// nodes at (0,0), invokes the handler via the dispatcher, and asserts both
// nodes were moved (FMGIRLayoutEngine only repositions expressions whose
// (x,y) are still (0,0), so seeding both at the origin guarantees both are
// eligible).
//
// Counterfactual: if the new handler registration is reverted, the
// dispatcher returns METHOD_NOT_FOUND for material.authoring.auto_layout
// and InvokeHandlerWithCapture returns false.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAutoLayoutTest,
    "PinWright.material.authoring.auto_layout.ReflowsUnpositionedExpressions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialAuthoringAutoLayoutTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/AutoLayout_%s"),
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

    // Two constants seeded at (0,0) so FMGIRLayoutEngine considers both
    // unpositioned and reflows both into a non-zero layout.
    UMaterialExpressionConstant* ConstA = NewObject<UMaterialExpressionConstant>(Material);
    UMaterialExpressionConstant* ConstB = NewObject<UMaterialExpressionConstant>(Material);
    if (!TestNotNull(TEXT("ConstA created"), ConstA) ||
        !TestNotNull(TEXT("ConstB created"), ConstB))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }
    ConstA->MaterialExpressionGuid = FGuid::NewGuid();
    ConstB->MaterialExpressionGuid = FGuid::NewGuid();
    ConstA->MaterialExpressionEditorX = 0;
    ConstA->MaterialExpressionEditorY = 0;
    ConstB->MaterialExpressionEditorX = 0;
    ConstB->MaterialExpressionEditorY = 0;
    Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(ConstA);
    Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(ConstB);
    Material->PostEditChange();
    FAssetRegistryModule::AssetCreated(Material);

    // Dispatch the handler. Asset is in-memory but reachable via LoadObject
    // because the package is registered.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("material.authoring.auto_layout"), Payload, Capture);
    TestTrue(TEXT("handler registered"), bFound);
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    if (!TestTrue(TEXT("response reports success"), Capture.bSuccess))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    // expressionsLaidOut counts all expressions on the asset (>=2 here).
    if (Capture.Result.IsValid())
    {
        double LaidOut = 0.0;
        if (Capture.Result->TryGetNumberField(TEXT("expressionsLaidOut"), LaidOut))
        {
            TestTrue(TEXT("expressionsLaidOut >= 1"), LaidOut >= 1.0);
        }
        FString Target;
        if (Capture.Result->TryGetStringField(TEXT("target"), Target))
        {
            TestEqual(TEXT("target echoes assetPath"), Target, AssetPath);
        }
    }

    // Load-bearing assertion: both constants moved away from (0,0).
    TestTrue(TEXT("ConstA was repositioned"),
        ConstA->MaterialExpressionEditorX != 0 || ConstA->MaterialExpressionEditorY != 0);
    TestTrue(TEXT("ConstB was repositioned"),
        ConstB->MaterialExpressionEditorX != 0 || ConstB->MaterialExpressionEditorY != 0);

    CleanupTestAsset(AssetPath);
    return true;
}
