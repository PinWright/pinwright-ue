// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Material/MaterialExpressionFactory.h"
#include "MaterialExpressionIO.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "UObject/Package.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialFunctionCallInputRawNameResolutionTest,
    "PinWright.material.authoring.connect_nodes.MaterialFunctionCallInputRawName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialFunctionCallInputRawNameResolutionTest::RunTest(const FString& Parameters)
{
    UMaterialExpressionMaterialFunctionCall* FuncCall =
        NewObject<UMaterialExpressionMaterialFunctionCall>(GetTransientPackage());
    if (!TestNotNull(TEXT("FuncCall created"), FuncCall))
    {
        return true;
    }

    UMaterialExpressionFunctionInput* FuncInputExpr =
        NewObject<UMaterialExpressionFunctionInput>(GetTransientPackage());
    if (!TestNotNull(TEXT("FuncInputExpr created"), FuncInputExpr))
    {
        return true;
    }
    FuncInputExpr->InputName = FName(TEXT("Speed"));
    FuncInputExpr->InputType = FunctionInput_Scalar;

    FFunctionExpressionInput Entry;
    Entry.ExpressionInput = FuncInputExpr;
    Entry.Input.InputName = FName(TEXT("Speed"));
    FuncCall->FunctionInputs.Add(Entry);

    FExpressionInput* ResolvedRaw =
        FMaterialExpressionFactory::FindExpressionInputByName(FuncCall, TEXT("Speed"));
    TestNotNull(TEXT("Resolved by raw name 'Speed'"), ResolvedRaw);
    if (ResolvedRaw)
    {
        TestEqual(TEXT("Raw resolution points at FunctionInputs[0].Input"),
            ResolvedRaw, &FuncCall->FunctionInputs[0].Input);
    }

    FExpressionInput* ResolvedDecorated =
        FMaterialExpressionFactory::FindExpressionInputByName(FuncCall, TEXT("Speed (S)"));
    TestNotNull(TEXT("Resolved by decorated name 'Speed (S)'"), ResolvedDecorated);

    return true;
}
