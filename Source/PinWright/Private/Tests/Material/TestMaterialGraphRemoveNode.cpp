// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for B-material-remove-incomplete. remove_node must use the engine's
// material-editing deletion path rather than only dropping the expression collection entry.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/Material/MaterialTestHelpers.h"
#include "UObject/ObjectMacros.h"

#include "Material/MaterialExpressionFactory.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialFunction.h"

namespace PinWrightMaterialGraphRemoveNodeTests
{
    UMaterialExpression* CreateExpression(FAutomationTestBase& Test, UMaterial* Material,
        UClass* ExpressionClass)
    {
        const FCreateResult Result = FMaterialExpressionFactory::Create(
            Material, ExpressionClass, nullptr, FVector2D(0.0, 0.0));
        Test.TestTrue(TEXT("Material expression created through the production factory"),
            Result.IsSuccess());
        if (!Result.IsSuccess())
        {
            return nullptr;
        }
        if (!Result.Expression->MaterialExpressionGuid.IsValid())
        {
            Result.Expression->MaterialExpressionGuid = FGuid::NewGuid();
        }
        return Result.Expression;
    }

    UMaterialExpression* CreateExpression(FAutomationTestBase& Test, UMaterialFunction* Function,
        UClass* ExpressionClass)
    {
        const FCreateResult Result = FMaterialExpressionFactory::Create(
            Function, ExpressionClass, nullptr, FVector2D(0.0, 0.0));
        Test.TestTrue(TEXT("Function expression created through the production factory"),
            Result.IsSuccess());
        if (!Result.IsSuccess())
        {
            return nullptr;
        }
        if (!Result.Expression->MaterialExpressionGuid.IsValid())
        {
            Result.Expression->MaterialExpressionGuid = FGuid::NewGuid();
        }
        return Result.Expression;
    }

    TSharedPtr<FJsonObject> MakeRemovePayload(const FString& AssetPath, const FString& NodeId)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("nodeId"), NodeId);
        return Payload;
    }

    bool HasParameterPointer(const UMaterial* Material, const UMaterialExpression* Expression)
    {
        if (!Material)
        {
            return false;
        }
        for (const TPair<FName, TArray<UMaterialExpression*>>& Pair : Material->EditorParameters)
        {
            if (Pair.Value.Contains(Expression))
            {
                return true;
            }
        }
        return false;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphRemoveNodeCleansMaterialExpressionTest,
    "PinWright.material.graph.remove_node.CleansUpMaterialExpression",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialGraphRemoveNodeCleansMaterialExpressionTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightMaterialGraphRemoveNodeTests;
    using namespace PinWrightMaterialTestHelpers;

    FString PackagePath;
    UMaterial* Material = CreateFixtureAsset<UMaterial>(*this, TEXT("RemoveNodeMaterial"),
        PackagePath);
    if (!Material)
    {
        return true;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(PackagePath); };

    UMaterialExpressionScalarParameter* Victim = Cast<UMaterialExpressionScalarParameter>(
        CreateExpression(*this, Material, UMaterialExpressionScalarParameter::StaticClass()));
    UMaterialExpressionMultiply* Survivor = Cast<UMaterialExpressionMultiply>(
        CreateExpression(*this, Material, UMaterialExpressionMultiply::StaticClass()));
    TestNotNull(TEXT("Scalar parameter victim exists"), Victim);
    TestNotNull(TEXT("Multiply survivor exists"), Survivor);
    if (!Victim || !Survivor || !Material->GetEditorOnlyData())
    {
        return true;
    }

    Victim->ParameterName = FName(TEXT("VictimParameter"));
    Survivor->A.Expression = Victim;
    Material->GetEditorOnlyData()->BaseColor.Expression = Victim;
    Material->BuildEditorParameterList();

    const FString AssetPath = Material->GetPathName();
    const FString VictimId = Victim->MaterialExpressionGuid.ToString();
    const int32 ExpressionCountBefore = ExpressionCount(Material);
    TestTrue(TEXT("parameter bookkeeping contains the victim before deletion"),
        HasParameterPointer(Material, Victim));

    FTestResponseCapture Capture;
    TestTrue(TEXT("material.graph.remove_node handler is registered"),
        InvokeHandlerWithCapture(TEXT("material.graph.remove_node"),
            MakeRemovePayload(AssetPath, VictimId), Capture));
    TestTrue(TEXT("material.graph.remove_node responded"), Capture.bWasCalled);
    TestTrue(TEXT("material.graph.remove_node succeeds for a material"), Capture.bSuccess);

    bool bRemoved = false;
    if (Capture.Result.IsValid())
    {
        TestTrue(TEXT("remove_node response carries removed"),
            Capture.Result->TryGetBoolField(TEXT("removed"), bRemoved));
    }
    TestTrue(TEXT("remove_node reports graph deletion"), bRemoved);
    TestEqual(TEXT("material expression collection removes the victim"),
        ExpressionCount(Material), ExpressionCountBefore - 1);
    TestFalse(TEXT("material expression collection no longer contains the victim"),
        Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Contains(Victim));
    TestNull(TEXT("native delete clears a surviving downstream input"), Survivor->A.Expression);
    TestNull(TEXT("native delete clears the main material input"),
        Material->GetEditorOnlyData()->BaseColor.Expression);
    TestFalse(TEXT("native delete removes the victim from parameter bookkeeping"),
        HasParameterPointer(Material, Victim));
    // ::IsValid rather than the Garbage flag directly: MarkAsGarbage sets PendingKill instead of
    // Garbage on UE 5.3, where pending-kill support still exists. Same assertion on every engine.
    TestFalse(TEXT("native delete invalidates the victim"), IsValid(Victim));

    FTestResponseCapture ReadbackCapture;
    TestTrue(TEXT("material.graph.get_node_details handler is registered"),
        InvokeHandlerWithCapture(TEXT("material.graph.get_node_details"),
            MakeRemovePayload(AssetPath, VictimId), ReadbackCapture));
    TestFalse(TEXT("removed node is absent from graph readback"), ReadbackCapture.bSuccess);
    TestEqual(TEXT("removed node readback returns NODE_NOT_FOUND"),
        ReadbackCapture.ErrorCode, FString(TEXT("NODE_NOT_FOUND")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphRemoveNodeCleansMaterialFunctionExpressionTest,
    "PinWright.material.graph.remove_node.CleansUpMaterialFunctionExpression",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialGraphRemoveNodeCleansMaterialFunctionExpressionTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightMaterialGraphRemoveNodeTests;
    using namespace PinWrightMaterialTestHelpers;

    FString PackagePath;
    UMaterialFunction* Function = CreateFixtureAsset<UMaterialFunction>(*this,
        TEXT("RemoveNodeFunction"), PackagePath);
    if (!Function)
    {
        return true;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(PackagePath); };

    UMaterialExpression* Victim = CreateExpression(*this, Function,
        UMaterialExpressionScalarParameter::StaticClass());
    UMaterialExpressionFunctionOutput* Survivor = Cast<UMaterialExpressionFunctionOutput>(
        CreateExpression(*this, Function, UMaterialExpressionFunctionOutput::StaticClass()));
    TestNotNull(TEXT("function scalar victim exists"), Victim);
    TestNotNull(TEXT("FunctionOutput survivor exists"), Survivor);
    if (!Victim || !Survivor || !Function->GetEditorOnlyData())
    {
        return true;
    }

    Survivor->A.Expression = Victim;
    const FString AssetPath = Function->GetPathName();
    const FString VictimId = Victim->MaterialExpressionGuid.ToString();
    const int32 ExpressionCountBefore = ExpressionCount(Function);

    FTestResponseCapture Capture;
    TestTrue(TEXT("material.graph.remove_node handler is registered for a function"),
        InvokeHandlerWithCapture(TEXT("material.graph.remove_node"),
            MakeRemovePayload(AssetPath, VictimId), Capture));
    TestTrue(TEXT("material.graph.remove_node succeeds for a function"), Capture.bSuccess);

    bool bRemoved = false;
    if (Capture.Result.IsValid())
    {
        TestTrue(TEXT("function remove_node response carries removed"),
            Capture.Result->TryGetBoolField(TEXT("removed"), bRemoved));
    }
    TestTrue(TEXT("function remove_node reports graph deletion"), bRemoved);
    TestEqual(TEXT("function expression collection removes the victim"),
        ExpressionCount(Function), ExpressionCountBefore - 1);
    TestFalse(TEXT("function expression collection no longer contains the victim"),
        Function->GetEditorOnlyData()->ExpressionCollection.Expressions.Contains(Victim));
    TestNull(TEXT("native function delete clears the FunctionOutput input"), Survivor->A.Expression);
    // See the material case above for why ::IsValid rather than the Garbage flag.
    TestFalse(TEXT("native function delete invalidates the victim"), IsValid(Victim));
    return true;
}
