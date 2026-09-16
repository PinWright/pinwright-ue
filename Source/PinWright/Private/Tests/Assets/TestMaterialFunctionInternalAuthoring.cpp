// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for F-material-function-internal-authoring: the node-add / connect / remove RPC
// family must be able to author a UMaterialFunction's internal graph, not only a UMaterial's.
//
// Before the fix every node-add/connect RPC resolved its target via a UMaterial-only loader, so a
// valid UMaterialFunction path was rejected with [ASSET_NOT_FOUND] "Could not load Material." and a
// function created through this API was a hollow shell (inputs/outputs but no body). This test drives
// the production handlers end-to-end against a real persisted function:
//   create_material_function -> add_function_input -> add_function_output
//   material.authoring.add_math_node   (the ticket's repro step 2)
//   material.graph.add_expression      (the ticket's repro step 3)
//   material.graph.connect_nodes       (wire input -> Multiply -> FunctionOutput)
//   material.graph.remove_node
// and asserts the function's own ExpressionCollection actually gains/wires/loses the nodes. If the
// fix is reverted, add_math_node / add_expression go back to returning an error on the function path
// and the success + container-membership assertions below fail.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"

#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMultiply.h"

namespace
{
    // Find an expression in the function's collection by GUID string, or nullptr.
    UMaterialExpression* FindFunctionExprByGuid(UMaterialFunction* Function, const FString& Guid)
    {
        if (!Function || Guid.IsEmpty()) return nullptr;
        for (const TObjectPtr<UMaterialExpression>& Expr : Function->GetExpressions())
        {
            if (Expr && Expr->MaterialExpressionGuid.ToString() == Guid)
            {
                return Expr.Get();
            }
        }
        return nullptr;
    }

    // Invoke a handler, assert it was found and returned success, and return the "nodeId" field.
    FString InvokeExpectNodeId(FAutomationTestBase& Test, const FString& Method,
        const TSharedPtr<FJsonObject>& Payload, const TCHAR* Label)
    {
        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(Method, Payload, Capture);
        Test.TestTrue(*FString::Printf(TEXT("%s handler registered"), Label), bFound);
        Test.TestTrue(*FString::Printf(TEXT("%s returned success (function path accepted)"), Label),
            Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            return Capture.Result->GetStringField(TEXT("nodeId"));
        }
        return FString();
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialFunctionInternalAuthoring,
    "PinWright.Material.Authoring.FunctionInternalAuthoring",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialFunctionInternalAuthoring::RunTest(const FString& Parameters)
{
    const FString NameSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString FuncName = FString::Printf(TEXT("MF_InternalAuthoring_%s"), *NameSuffix);
    const FString FuncDir = TEXT("/Game/PinWrightTests");
    const FString FuncPackagePath = FString::Printf(TEXT("%s/%s"), *FuncDir, *FuncName);
    const FString FuncObjectPath = FString::Printf(TEXT("%s.%s"), *FuncPackagePath, *FuncName);

    ON_SCOPE_EXIT { CleanupTestAsset(FuncPackagePath); };

    // ---- create the material function asset -----------------------------
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), FuncName);
        Payload->SetStringField(TEXT("path"), FuncDir);
        Payload->SetBoolField(TEXT("save"), false);
        TestTrue(TEXT("create_material_function handler found"),
            InvokeHandler(TEXT("material.authoring.create_material_function"), Payload));
    }

    UMaterialFunction* Function = LoadObject<UMaterialFunction>(nullptr, *FuncObjectPath);
    TestNotNull(TEXT("material function asset created"), Function);
    if (!Function) return true;

    // ---- add a function input (scalar "Brightness") ---------------------
    FString InputNodeId;
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), FuncObjectPath);
        Payload->SetStringField(TEXT("inputName"), TEXT("Brightness"));
        Payload->SetStringField(TEXT("inputType"), TEXT("Scalar"));
        Payload->SetNumberField(TEXT("x"), -400.0);
        Payload->SetNumberField(TEXT("y"), 0.0);
        InputNodeId = InvokeExpectNodeId(*this, TEXT("material.authoring.add_function_input"),
            Payload, TEXT("add_function_input"));
    }
    TestFalse(TEXT("input node id non-empty"), InputNodeId.IsEmpty());

    // ---- add a function output ------------------------------------------
    FString OutputNodeId;
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), FuncObjectPath);
        Payload->SetStringField(TEXT("inputName"), TEXT("Result"));
        Payload->SetNumberField(TEXT("x"), 200.0);
        Payload->SetNumberField(TEXT("y"), 0.0);
        OutputNodeId = InvokeExpectNodeId(*this, TEXT("material.authoring.add_function_output"),
            Payload, TEXT("add_function_output"));
    }
    TestFalse(TEXT("output node id non-empty"), OutputNodeId.IsEmpty());

    // ---- material.authoring.add_math_node on the FUNCTION path (repro step 2) ----
    // Pre-fix: returned [ASSET_NOT_FOUND] "Could not load Material." Post-fix: creates a Multiply
    // node inside the function's expression collection.
    FString MultiplyNodeId;
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), FuncObjectPath);
        Payload->SetStringField(TEXT("operation"), TEXT("Multiply"));
        Payload->SetNumberField(TEXT("x"), 0.0);
        Payload->SetNumberField(TEXT("y"), 0.0);
        MultiplyNodeId = InvokeExpectNodeId(*this, TEXT("material.authoring.add_math_node"),
            Payload, TEXT("add_math_node"));
    }
    TestFalse(TEXT("multiply node id non-empty"), MultiplyNodeId.IsEmpty());

    UMaterialExpression* MultiplyExpr = FindFunctionExprByGuid(Function, MultiplyNodeId);
    TestNotNull(TEXT("add_math_node wrote Multiply into the function's expression collection"),
        MultiplyExpr);
    TestTrue(TEXT("created node is a UMaterialExpressionMultiply"),
        MultiplyExpr && MultiplyExpr->IsA<UMaterialExpressionMultiply>());

    // ---- material.graph.add_expression on the FUNCTION path (repro step 3) ----
    FString ConstNodeId;
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), FuncObjectPath);
        Payload->SetStringField(TEXT("expressionClass"), TEXT("Constant"));
        Payload->SetNumberField(TEXT("x"), -200.0);
        Payload->SetNumberField(TEXT("y"), 120.0);
        ConstNodeId = InvokeExpectNodeId(*this, TEXT("material.graph.add_expression"),
            Payload, TEXT("material.graph.add_expression"));
    }
    TestFalse(TEXT("constant node id non-empty"), ConstNodeId.IsEmpty());
    TestNotNull(TEXT("add_expression wrote Constant into the function's expression collection"),
        FindFunctionExprByGuid(Function, ConstNodeId));

    // ---- wire input -> Multiply.A via material.graph.connect_nodes ------
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), FuncObjectPath);
        Payload->SetStringField(TEXT("sourceNodeId"), InputNodeId);
        Payload->SetStringField(TEXT("targetNodeId"), MultiplyNodeId);
        Payload->SetStringField(TEXT("inputName"), TEXT("A"));
        FTestResponseCapture Capture;
        TestTrue(TEXT("connect_nodes handler found"),
            InvokeHandlerWithCapture(TEXT("material.graph.connect_nodes"), Payload, Capture));
        TestTrue(TEXT("connect_nodes input->Multiply.A succeeded on function"), Capture.bSuccess);
    }

    // Verify the wire actually landed on Multiply's A input.
    if (UMaterialExpressionMultiply* Mul = Cast<UMaterialExpressionMultiply>(MultiplyExpr))
    {
        TestNotNull(TEXT("Multiply.A is wired to the function input"), Mul->A.Expression);
        TestEqual(TEXT("Multiply.A connects to the FunctionInput node"),
            (UMaterialExpression*)Mul->A.Expression,
            FindFunctionExprByGuid(Function, InputNodeId));
    }

    // ---- wire Multiply -> FunctionOutput.A ------------------------------
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), FuncObjectPath);
        Payload->SetStringField(TEXT("sourceNodeId"), MultiplyNodeId);
        Payload->SetStringField(TEXT("targetNodeId"), OutputNodeId);
        Payload->SetStringField(TEXT("inputName"), TEXT("A"));
        FTestResponseCapture Capture;
        TestTrue(TEXT("connect_nodes handler found (output wire)"),
            InvokeHandlerWithCapture(TEXT("material.graph.connect_nodes"), Payload, Capture));
        TestTrue(TEXT("connect_nodes Multiply->FunctionOutput.A succeeded"), Capture.bSuccess);
    }

    if (UMaterialExpressionFunctionOutput* Out =
            Cast<UMaterialExpressionFunctionOutput>(FindFunctionExprByGuid(Function, OutputNodeId)))
    {
        TestEqual(TEXT("FunctionOutput.A connects to the Multiply node"),
            (UMaterialExpression*)Out->A.Expression, MultiplyExpr);
    }

    // ---- 'Main' is rejected on a function (no main output node) ---------
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), FuncObjectPath);
        Payload->SetStringField(TEXT("sourceNodeId"), MultiplyNodeId);
        Payload->SetStringField(TEXT("targetNodeId"), TEXT("Main"));
        Payload->SetStringField(TEXT("inputName"), TEXT("BaseColor"));
        FTestResponseCapture Capture;
        TestTrue(TEXT("connect_nodes handler found (Main rejection)"),
            InvokeHandlerWithCapture(TEXT("material.graph.connect_nodes"), Payload, Capture));
        TestFalse(TEXT("connect_nodes to 'Main' is rejected on a function"), Capture.bSuccess);
        TestEqual(TEXT("Main-on-function error code is INVALID_PIN"),
            Capture.ErrorCode, FString(TEXT("INVALID_PIN")));
    }

    // ---- remove the Constant node via material.graph.remove_node --------
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), FuncObjectPath);
        Payload->SetStringField(TEXT("nodeId"), ConstNodeId);
        FTestResponseCapture Capture;
        TestTrue(TEXT("remove_node handler found"),
            InvokeHandlerWithCapture(TEXT("material.graph.remove_node"), Payload, Capture));
        TestTrue(TEXT("remove_node succeeded on function"), Capture.bSuccess);
    }
    TestNull(TEXT("removed Constant is gone from the function's expression collection"),
        FindFunctionExprByGuid(Function, ConstNodeId));

    return true;
}
