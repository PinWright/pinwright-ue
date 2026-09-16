// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "EdGraph/EdGraph.h"

using namespace CompilerTestUtils;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirForwardReferenceFunctionTest,
    "PinWright.bpir.compiler.forward_reference.FunctionCallsLaterFunction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirForwardReferenceFunctionTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("ForwardRefTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry function A() {\n")
        TEXT("    call B(Target: self)\n")
        TEXT("}\n")
        TEXT("\n")
        TEXT("entry function B() {\n")
        TEXT("    call PrintString(InString: \"hello\")\n")
        TEXT("}\n"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded with forward-reference from A to B"), Result.bSuccess);

    // Both function graphs must exist on the blueprint
    bool bFoundA = false;
    bool bFoundB = false;
    for (const UEdGraph* Graph : BP->FunctionGraphs)
    {
        if (Graph)
        {
            FString Name = Graph->GetFName().ToString();
            if (Name.Equals(TEXT("A"), ESearchCase::IgnoreCase)) { bFoundA = true; }
            if (Name.Equals(TEXT("B"), ESearchCase::IgnoreCase)) { bFoundB = true; }
        }
    }
    TestTrue(TEXT("Function graph A exists"), bFoundA);
    TestTrue(TEXT("Function graph B exists"), bFoundB);
    return true;
}
