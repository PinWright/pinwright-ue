// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBpirInsertEmptyTrailingLabelReattach.cpp
// Regression test for B-bpir-statement-cast-success-unwired-replace-shared-topology
// (manifestation #2: insert_bpir_before_node drops a branch [true -> @continue] exec
// target when the inserted body terminates at an empty trailing label).
//
// Pre-fix: InsertCodeAfterNode's trailing-reattachment scan selected the last impure
// node's generic ExecOutputPin and ignored labeled exec targets. When the inserted
// body ended at `branch [true -> @continue]` followed by an empty `@continue:`
// label, the branch's True pin's exec target was effectively silently dropped — the
// original downstream node (the one the caller asked to insert before) was left
// orphaned with zero exec-input links.
//
// Fixed behaviour: when the last impure instruction has labeled exec targets that
// resolve to empty/missing labels, the reattachment picks that labeled exit pin
// instead of the generic ExecOutputPin. The True branch is wired straight through
// to the original downstream node.
//
// Counterfactual: if the reattachment scan reverts to using the generic
// ExecOutputPin, the target VariableSet's exec input has zero linked entries after
// the insert and the test FAILS.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

using namespace CompilerTestUtils;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationInsertEmptyTrailingLabelReattachTest,
    "PinWright.bpir.compiler.integration.InsertEmptyTrailingLabelReattach",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationInsertEmptyTrailingLabelReattachTest::RunTest(const FString& Parameters)
{
    // 1. Create a transient AActor BP with an int member 'Counter' so the BPIR
    //    compiler has a member variable target for `set Counter = ...`.
    UBlueprint* BP = CreateTransientTestBP(TEXT("InsertEmptyTrailingLabelBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FEdGraphPinType IntType;
    IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
    const bool bVarAdded = FBlueprintEditorUtils::AddMemberVariable(
        BP, TEXT("Counter"), IntType);
    TestTrue(TEXT("Counter member variable added"), bVarAdded);
    if (!bVarAdded) return false;
    FKismetEditorUtilities::CompileBlueprint(BP);

    // 2. Plant a baseline body: BeginPlay -> set $Counter = 1. The VariableSet is the
    //    target node we will insert before. Mirrors the originating bug repro's
    //    Tick @after: first-impure target.
    {
        FBpirCompiler Setup(BP);
        FCompileResult SetupResult = Setup.Compile(
            TEXT("entry event BeginPlay() {\n")
            TEXT("    set Counter = 1\n")
            TEXT("}"));
        if (!SetupResult.bSuccess)
        {
            for (const FCompileError& Err : SetupResult.Errors)
            {
                AddError(FString::Printf(TEXT("Setup compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Setup compile (BeginPlay -> set Counter) succeeded"), SetupResult.bSuccess);
        if (!SetupResult.bSuccess) return false;
    }

    // 3. Locate the original Counter VariableSet node — the insert target.
    UK2Node_VariableSet* TargetSetNode = nullptr;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        if (!Graph) continue;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_VariableSet* SetNode = ::Cast<UK2Node_VariableSet>(Node);
            if (SetNode && SetNode->GetVarName() == FName(TEXT("Counter")))
            {
                TargetSetNode = SetNode;
                break;
            }
        }
        if (TargetSetNode) break;
    }
    TestNotNull(TEXT("Counter VariableSet node located"), TargetSetNode);
    if (!TargetSetNode) return false;

    // Capture the target's exec input pin and confirm it has exactly one upstream
    // link (from BeginPlay) before the insert.
    UEdGraphPin* TargetExecIn = TargetSetNode->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
    TestNotNull(TEXT("Target VariableSet exec input pin exists"), TargetExecIn);
    if (!TargetExecIn) return false;
    TestEqual(TEXT("Target VariableSet exec input has exactly 1 link before insert"),
        TargetExecIn->LinkedTo.Num(), 1);

    // 4. Insert BPIR ending at `branch [true -> @continue]` followed by an empty
    //    trailing `@continue:` label. Mirrors the ticket's #2 repro body.
    FBpirCompiler Compiler(BP);
    FCompileResult InsertResult = Compiler.InsertCodeBeforeNode(
        TargetSetNode,
        TEXT("%v = pure RandomBool()\n")
        TEXT("%b = branch(%v) [true -> @continue]\n")
        TEXT("\n")
        TEXT("@continue:\n"));

    if (!InsertResult.bSuccess)
    {
        for (const FCompileError& Err : InsertResult.Errors)
        {
            AddError(FString::Printf(TEXT("Insert compile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("InsertCodeBeforeNode (branch + empty trailing label) succeeded"),
        InsertResult.bSuccess);
    if (!InsertResult.bSuccess) return false;
    TestEqual(TEXT("Insert reported no compile errors"), InsertResult.Errors.Num(), 0);

    // 5. Locate the inserted Branch (UK2Node_IfThenElse) node.
    UK2Node_IfThenElse* InsertedBranch = nullptr;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        if (!Graph) continue;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_IfThenElse* Branch = ::Cast<UK2Node_IfThenElse>(Node))
            {
                InsertedBranch = Branch;
                break;
            }
        }
        if (InsertedBranch) break;
    }
    TestNotNull(TEXT("Inserted Branch (UK2Node_IfThenElse) node exists"), InsertedBranch);
    if (!InsertedBranch) return false;

    // 6. The Counter VariableSet's exec input must still have exactly one link —
    //    the reattachment must have wired it to the branch's True pin instead of
    //    leaving it orphaned.
    TestEqual(TEXT("Target VariableSet exec input still has exactly 1 link after insert"),
        TargetExecIn->LinkedTo.Num(), 1);

    // 7. The link must come from the Branch's True pin (not the original BeginPlay
    //    or some other path).
    if (TargetExecIn->LinkedTo.Num() >= 1)
    {
        UEdGraphPin* UpstreamPin = TargetExecIn->LinkedTo[0];
        TestNotNull(TEXT("Target exec input's upstream link is valid"), UpstreamPin);
        if (UpstreamPin)
        {
            TestTrue(TEXT("Target exec input is now driven by the inserted Branch"),
                UpstreamPin->GetOwningNode() == InsertedBranch);
            TestEqual(TEXT("Target exec input is wired from Branch's True pin"),
                UpstreamPin->PinName, UEdGraphSchema_K2::PN_Then);
        }
    }

    // 8. The branch's True pin should LinkTo exactly the target's exec input.
    //    UK2Node_IfThenElse uses PN_Then for the True exec output pin.
    UEdGraphPin* TruePin = InsertedBranch->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
    TestNotNull(TEXT("Branch True pin exists"), TruePin);
    if (TruePin)
    {
        TestEqual(TEXT("Branch True pin has exactly 1 outgoing link"),
            TruePin->LinkedTo.Num(), 1);
        if (TruePin->LinkedTo.Num() == 1)
        {
            TestTrue(TEXT("Branch True pin links to the original VariableSet"),
                TruePin->LinkedTo[0]->GetOwningNode() == TargetSetNode);
        }
    }

    return true;
}
