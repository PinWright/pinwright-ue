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

// B-bpir-forward-call-loses-parameter-pins: a call to a function declared in the SAME
// compile must see that function's parameters. AddFunctionGraph regenerates the skeleton
// before SetupFunction adds the parameter pins, so without the Phase 1.5 recompile the
// callee's UFunction is a parameterless stub and the call node carries only `self`
// ("Could not find target pin 'Count' ... Available pins: self").
// Counterfactual: drop `CreatedFunctionGraphs.Num() > 0` from NeedsSkeletonRecompile in
// BpirCompiler.cpp and this compile fails on both call sites.
namespace TestBpirForwardReferenceHelpers
{
    static UK2Node_CallFunction* FindCallInGraph(const UEdGraph* Graph, FName FunctionName)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
            if (Call && Call->FunctionReference.GetMemberName() == FunctionName)
            {
                return Call;
            }
        }
        return nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirForwardReferenceParamPinsTest,
    "PinWright.bpir.compiler.forward_reference.SameCompileCalleeKeepsParamPins",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirForwardReferenceParamPinsTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("ForwardRefParamsBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry function Caller() {\n")
        TEXT("    %r = call ParamHelper(Count: 3, bFlag: true)\n")
        TEXT("    return\n")
        TEXT("}\n")
        TEXT("\n")
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call ParamHelper(Count: 7, bFlag: false)\n")
        TEXT("}\n")
        TEXT("\n")
        TEXT("entry function ParamHelper(int Count, bool bFlag) -> int {\n")
        TEXT("    call PrintString(InString: \"helper\")\n")
        TEXT("    %sum = pure Add_IntInt(A: 2, B: 3)\n")
        TEXT("    return %sum\n")
        TEXT("}\n"));

    for (const FCompileError& Err : Result.Errors)
    {
        AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
    }
    TestTrue(TEXT("Same-compile call with arguments to a later function compiles"), Result.bSuccess);

    TArray<UEdGraph*> CallerGraphs;
    CallerGraphs.Append(BP->FunctionGraphs);
    CallerGraphs.Append(BP->UbergraphPages);
    // Both call sites (function body and event graph) must carry the callee's parameters.
    int32 CallSites = 0;
    for (UEdGraph* Graph : CallerGraphs)
    {
        if (!Graph) continue;
        UK2Node_CallFunction* Call = TestBpirForwardReferenceHelpers::FindCallInGraph(Graph, TEXT("ParamHelper"));
        if (!Call) continue;
        ++CallSites;
        UEdGraphPin* CountPin = Call->FindPin(TEXT("Count"), EGPD_Input);
        UEdGraphPin* FlagPin = Call->FindPin(TEXT("bFlag"), EGPD_Input);
        TestNotNull(*FString::Printf(TEXT("Call in '%s' has a 'Count' pin"), *Graph->GetName()), CountPin);
        TestNotNull(*FString::Printf(TEXT("Call in '%s' has a 'bFlag' pin"), *Graph->GetName()), FlagPin);
        if (CountPin)
        {
            const FString Expected = Graph->GetName() == TEXT("Caller") ? TEXT("3") : TEXT("7");
            TestEqual(*FString::Printf(TEXT("Call in '%s' carries its Count literal"), *Graph->GetName()),
                CountPin->DefaultValue, Expected);
        }
        UEdGraphPin* ReturnPin = Call->FindPin(UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output);
        TestTrue(*FString::Printf(TEXT("Call in '%s' has the callee's int ReturnValue pin"), *Graph->GetName()),
            ReturnPin && ReturnPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int);
    }
    TestEqual(TEXT("Both ParamHelper call sites were emitted"), CallSites, 2);
    return true;
}
