// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBpirExecTargetLeadsPureNode.cpp
// Regression test for B-bpir-exec-target-leads-pure-node.
//
// A labeled exec target ([pin -> @label]) that points at a block whose FIRST
// instruction is a `call` to a PURE UK2Node (one with no exec pins, emitted via the
// generic-node lane, e.g. `%x = call K2Node_ConvertAsset(...)`) must skip that pure
// binding and wire to the first genuinely-impure instruction in the block.
//
// Pre-fix: FindFirstImpureAtLabel selected the target on `bBpirImpure || bNodeImpure`.
// A pure generic K2Node keeps its authored opcode `Call` (RestoreExecIfPure only runs
// on the CallFunction lane), so `Inst.IsImpure()` is true and the pure node was
// mis-selected. It has no exec INPUT pin, so GetExecInputPin returned null, WireExecPins
// Step 3 silently dropped the edge, and the post-wire verification (Step 3b) then
// hard-failed with "labeled exec target ... was not wired (target label resolves to
// inst[M])".
//
// Fixed behaviour: a node is a valid labeled exec TARGET only if it has an exec INPUT
// pin able to receive control, so FindFirstImpureAtLabel skips the pure node and resolves
// to the following impure instruction.
//
// This synthetic fixture uses `call K2Node_GetEnumeratorName()` as the pure generic-lane
// node (UK2Node_GetEnumeratorName::IsNodePure() == true; AllocateDefaultPins creates only
// a Byte input + Name output, no exec pins) standing in for the ticket's ConvertAsset.
// A branch's [true -> @closedo] clause stands in for the ticket's `cast<T> [success ->
// @closedo]` upstream — the defect is in label resolution, independent of the upstream
// node's type.
//
// Counterfactual: if FindFirstImpureAtLabel reverts to trusting the authored opcode, the
// compile FAILS the Result.bSuccess assertion (post-wire verification error), and the
// branch's True pin is left with zero downstream links.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_GetEnumeratorName.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

using namespace CompilerTestUtils;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationExecTargetLeadsPureNodeTest,
    "PinWright.bpir.compiler.integration.ExecTargetLeadsPureNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationExecTargetLeadsPureNodeTest::RunTest(const FString& Parameters)
{
    // 1. Transient AActor BP with an int member 'Counter' so `set Counter = 1` has a
    //    member-variable target to emit an impure UK2Node_VariableSet against.
    UBlueprint* BP = CreateTransientTestBP(TEXT("ExecTargetLeadsPureNodeBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FEdGraphPinType IntType;
    IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
    const bool bVarAdded = FBlueprintEditorUtils::AddMemberVariable(
        BP, TEXT("Counter"), IntType);
    TestTrue(TEXT("Counter member variable added"), bVarAdded);
    if (!bVarAdded) return false;
    FKismetEditorUtilities::CompileBlueprint(BP);

    // 2. Compile a body where a branch's [true -> @closedo] clause targets a block that
    //    LEADS with a pure `call K2Node_GetEnumeratorName()` binding, followed by the
    //    impure `set Counter = 1`. This is the ticket's repro shape.
    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %v = pure RandomBool()\n")
        TEXT("    %b = branch(%v) [true -> @closedo]\n")
        TEXT("@closedo:\n")
        TEXT("    %name = call K2Node_GetEnumeratorName()\n")
        TEXT("    set Counter = 1\n")
        TEXT("}"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    // Primary counterfactual: pre-fix this fails with the post-wire verification error.
    TestTrue(TEXT("Compile of [true -> @closedo] leading with a pure call node succeeded"),
        Result.bSuccess);
    if (!Result.bSuccess) return false;
    TestEqual(TEXT("Compile reported no errors"), Result.Errors.Num(), 0);

    // 3. Precondition guard: the emitted GetEnumeratorName node is genuinely pure (no
    //    exec pins). If this ever regresses, the test would no longer exercise the bug.
    UK2Node_GetEnumeratorName* PureNode = FindNodeOfType<UK2Node_GetEnumeratorName>(BP);
    TestNotNull(TEXT("Pure K2Node_GetEnumeratorName placed at @closedo"), PureNode);
    if (PureNode)
    {
        // The fix's production predicate keys on the exec INPUT pin (GetExecInputPin), so
        // assert exactly that property via the same engine schema helper the compiler mirrors.
        const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
        TestNull(TEXT("Leading node has no exec INPUT pin — the bug precondition"),
            K2Schema->FindExecutionPin(*PureNode, EGPD_Input));
    }

    // 4. Locate the branch and the impure Counter VariableSet (the intended target).
    UK2Node_IfThenElse* Branch = FindNodeOfType<UK2Node_IfThenElse>(BP);
    TestNotNull(TEXT("Branch (UK2Node_IfThenElse) placed"), Branch);

    UK2Node_VariableSet* CounterSet = FindNodeOfType<UK2Node_VariableSet>(BP);
    TestNotNull(TEXT("Counter VariableSet placed"), CounterSet);
    if (CounterSet)
    {
        TestEqual(TEXT("VariableSet targets 'Counter'"),
            CounterSet->GetVarName(), FName(TEXT("Counter")));
    }

    // 5. The branch's True pin must be wired to the Counter VariableSet — resolution
    //    skipped the pure node and advanced to the first genuinely-impure instruction.
    if (Branch && CounterSet)
    {
        UEdGraphPin* TruePin = Branch->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
        TestNotNull(TEXT("Branch True (Then) exec output pin exists"), TruePin);
        if (TruePin)
        {
            TestEqual(TEXT("Branch True pin has exactly one downstream exec link"),
                TruePin->LinkedTo.Num(), 1);
            if (TruePin->LinkedTo.Num() == 1)
            {
                TestTrue(TEXT("Branch True is wired to the Counter VariableSet, past the pure node"),
                    TruePin->LinkedTo[0]->GetOwningNode() == CounterSet);
            }
        }

        // And the impure target actually received the exec edge.
        UEdGraphPin* SetExecIn = CounterSet->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
        TestNotNull(TEXT("Counter VariableSet exec input pin exists"), SetExecIn);
        if (SetExecIn)
        {
            TestEqual(TEXT("Counter VariableSet exec input has exactly one upstream link"),
                SetExecIn->LinkedTo.Num(), 1);
        }
    }

    return true;
}
