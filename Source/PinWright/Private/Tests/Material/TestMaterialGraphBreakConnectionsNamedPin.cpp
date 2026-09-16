// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"


namespace
{
    struct FBreakConnTestSetup
    {
        FString AssetPath;
        UMaterial* Material = nullptr;
        UMaterialExpressionMultiply* MulExpr = nullptr;
        UMaterialExpressionConstant* ConstA = nullptr;
        UMaterialExpressionConstant* ConstB = nullptr;
    };

    bool BuildMaterial(FAutomationTestBase& Test, FBreakConnTestSetup& Out, const TCHAR* Slug)
    {
        Out.AssetPath = FString::Printf(
            TEXT("/Game/__PW_GatewayTests/BreakConn%s_%s"),
            Slug,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));

        UPackage* Pkg = CreatePackage(*Out.AssetPath);
        if (!Test.TestNotNull(TEXT("Package created"), Pkg))
            return false;

        Out.Material = NewObject<UMaterial>(
            Pkg,
            FName(*FPackageName::GetLongPackageAssetName(Out.AssetPath)),
            RF_Public | RF_Standalone);

        if (!Test.TestNotNull(TEXT("Material created"), Out.Material))
            return false;

        Out.MulExpr = NewObject<UMaterialExpressionMultiply>(Out.Material);
        if (!Test.TestNotNull(TEXT("Multiply expression created"), Out.MulExpr))
            return false;
        if (!Out.MulExpr->MaterialExpressionGuid.IsValid())
        {
            Out.MulExpr->MaterialExpressionGuid = FGuid::NewGuid();
        }

        Out.ConstA = NewObject<UMaterialExpressionConstant>(Out.Material);
        if (!Test.TestNotNull(TEXT("Constant A expression created"), Out.ConstA))
            return false;
        if (!Out.ConstA->MaterialExpressionGuid.IsValid())
        {
            Out.ConstA->MaterialExpressionGuid = FGuid::NewGuid();
        }

        Out.ConstB = NewObject<UMaterialExpressionConstant>(Out.Material);
        if (!Test.TestNotNull(TEXT("Constant B expression created"), Out.ConstB))
            return false;
        if (!Out.ConstB->MaterialExpressionGuid.IsValid())
        {
            Out.ConstB->MaterialExpressionGuid = FGuid::NewGuid();
        }

        Out.Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Out.MulExpr);
        Out.Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Out.ConstA);
        Out.Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Out.ConstB);
        Out.Material->PostEditChange();
        FAssetRegistryModule::AssetCreated(Out.Material);

        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphBreakConnectionsNamedPinTest,
    "PinWright.material.graph.break_connections.NamedPinClearsInput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialGraphBreakConnectionsNamedPinTest::RunTest(const FString& Parameters)
{
    FBreakConnTestSetup S;
    if (!BuildMaterial(*this, S, TEXT("NamedPin")))
    {
        if (!S.AssetPath.IsEmpty()) CleanupTestAsset(S.AssetPath);
        return true;
    }

    S.MulExpr->A.Expression = S.ConstA;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), S.AssetPath);
    Payload->SetStringField(TEXT("nodeId"), S.MulExpr->MaterialExpressionGuid.ToString());
    Payload->SetStringField(TEXT("pinName"), TEXT("A"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("material.graph.break_connections"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("break_connections succeeded"), Capture.bSuccess);

    TestEqual(TEXT("MulExpr->A.Expression cleared"),
        S.MulExpr->A.Expression, static_cast<UMaterialExpression*>(nullptr));

    if (TestTrue(TEXT("Result JSON present"), Capture.Result.IsValid()))
    {
        bool bBroken = false;
        TestTrue(TEXT("broken bool present"), Capture.Result->TryGetBoolField(TEXT("broken"), bBroken));
        TestTrue(TEXT("broken == true"), bBroken);

        const TArray<TSharedPtr<FJsonValue>>* PinsArrayPtr = nullptr;
        if (TestTrue(TEXT("pinsBroken array present"),
            Capture.Result->TryGetArrayField(TEXT("pinsBroken"), PinsArrayPtr)) && PinsArrayPtr)
        {
            TestEqual(TEXT("pinsBroken length == 1"), PinsArrayPtr->Num(), 1);
            if (PinsArrayPtr->Num() == 1)
            {
                TestEqual(TEXT("pinsBroken[0] == A"),
                    (*PinsArrayPtr)[0]->AsString(), FString(TEXT("A")));
            }
        }
    }

    CleanupTestAsset(S.AssetPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphBreakConnectionsAllPinsTest,
    "PinWright.material.graph.break_connections.AllPinsCleared",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialGraphBreakConnectionsAllPinsTest::RunTest(const FString& Parameters)
{
    FBreakConnTestSetup S;
    if (!BuildMaterial(*this, S, TEXT("AllPins")))
    {
        if (!S.AssetPath.IsEmpty()) CleanupTestAsset(S.AssetPath);
        return true;
    }

    S.MulExpr->A.Expression = S.ConstA;
    S.MulExpr->B.Expression = S.ConstB;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), S.AssetPath);
    Payload->SetStringField(TEXT("nodeId"), S.MulExpr->MaterialExpressionGuid.ToString());
    // No pinName field — should clear all populated inputs.

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("material.graph.break_connections"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("break_connections succeeded"), Capture.bSuccess);

    TestEqual(TEXT("MulExpr->A.Expression cleared"),
        S.MulExpr->A.Expression, static_cast<UMaterialExpression*>(nullptr));
    TestEqual(TEXT("MulExpr->B.Expression cleared"),
        S.MulExpr->B.Expression, static_cast<UMaterialExpression*>(nullptr));

    if (TestTrue(TEXT("Result JSON present"), Capture.Result.IsValid()))
    {
        const TArray<TSharedPtr<FJsonValue>>* PinsArrayPtr = nullptr;
        if (TestTrue(TEXT("pinsBroken array present"),
            Capture.Result->TryGetArrayField(TEXT("pinsBroken"), PinsArrayPtr)) && PinsArrayPtr)
        {
            TestEqual(TEXT("pinsBroken length == 2"), PinsArrayPtr->Num(), 2);
            bool bSawA = false;
            bool bSawB = false;
            for (const TSharedPtr<FJsonValue>& V : *PinsArrayPtr)
            {
                const FString Name = V->AsString();
                if (Name == TEXT("A")) bSawA = true;
                if (Name == TEXT("B")) bSawB = true;
            }
            TestTrue(TEXT("pinsBroken contains A"), bSawA);
            TestTrue(TEXT("pinsBroken contains B"), bSawB);
        }
    }

    CleanupTestAsset(S.AssetPath);
    return true;
}
