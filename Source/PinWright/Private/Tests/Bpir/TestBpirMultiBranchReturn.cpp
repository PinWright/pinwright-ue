// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBpirMultiBranchReturn.cpp
// Regression tests for B-bpir-second-return-branch-data-dropped.
//
// A function with a `return` in each branch used to collapse onto ONE
// UK2Node_FunctionResult: both branch execs were wired to it, only the LAST
// branch's data pins were wired, the earlier branch's producers were left
// orphaned, and the compile answered compiled:true with no errors or warnings.
// The decompile of such a graph showed the tell — `branch(%n) [false -> @merge,
// true -> @merge]`, a branch whose two outputs go to the same label.
//
// The fix gives every `return` statement its own result node. Several
// UK2Node_FunctionResult nodes per function graph are legal (the engine's
// FKCHandler_FunctionResult merges them) and that is what a human authors in the
// editor: one Return node per branch.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Decompiler/BpirDecompiler.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"

using namespace CompilerTestUtils;

namespace
{
    // Two branches, each ending in its own `return`, each returning two values
    // produced by that branch so both result-pin connections are exercised.
    const TCHAR* const MultiBranchReturnSource =
        TEXT("entry function PickValue(bool bUseHigh, int Base) -> (int Value, int Doubled) {\n")
        TEXT("    %b = branch($bUseHigh) [true -> @high, false -> @low]\n")
        TEXT("\n")
        TEXT("@high:\n")
        TEXT("    %h = pure Add_IntInt(A: $Base, B: 100)\n")
        TEXT("    %hd = pure Multiply_IntInt(A: %h, B: 2)\n")
        TEXT("    return (Value: %h, Doubled: %hd)\n")
        TEXT("\n")
        TEXT("@low:\n")
        TEXT("    %l = pure Add_IntInt(A: $Base, B: 1)\n")
        TEXT("    %ld = pure Multiply_IntInt(A: %l, B: 2)\n")
        TEXT("    return (Value: %l, Doubled: %ld)\n")
        TEXT("}\n");

    UEdGraph* FindMultiBranchReturnGraph(UBlueprint* BP, const TCHAR* GraphName)
    {
        if (!BP)
        {
            return nullptr;
        }
        for (UEdGraph* Graph : BP->FunctionGraphs)
        {
            if (Graph && Graph->GetName() == GraphName)
            {
                return Graph;
            }
        }
        return nullptr;
    }

    TArray<UK2Node_FunctionResult*> CollectMultiBranchReturnResults(UEdGraph* Graph)
    {
        TArray<UK2Node_FunctionResult*> Results;
        if (!Graph)
        {
            return Results;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Node))
            {
                Results.Add(Result);
            }
        }
        return Results;
    }

    UEdGraphNode* MultiBranchReturnLinkSource(UEdGraphPin* Pin)
    {
        if (!Pin || Pin->LinkedTo.Num() != 1 || !Pin->LinkedTo[0])
        {
            return nullptr;
        }
        return Pin->LinkedTo[0]->GetOwningNodeUnchecked();
    }

    int32 CountMultiBranchReturnOccurrences(const FString& Text, const FString& Needle)
    {
        int32 Count = 0;
        int32 Cursor = 0;
        while (Cursor <= Text.Len() - Needle.Len())
        {
            const int32 Found = Text.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, Cursor);
            if (Found == INDEX_NONE)
            {
                break;
            }
            ++Count;
            Cursor = Found + Needle.Len();
        }
        return Count;
    }

    // Pull the `[a -> @x, b -> @y]` exec targets off the first `branch(` line.
    // Returns false when the line or the two targets cannot be found.
    bool MultiBranchReturnBranchTargets(const FString& Text, FString& OutFirst, FString& OutSecond)
    {
        TArray<FString> Lines;
        Text.ParseIntoArrayLines(Lines);
        for (const FString& Line : Lines)
        {
            if (!Line.Contains(TEXT("branch(")))
            {
                continue;
            }
            int32 Open = INDEX_NONE;
            int32 Close = INDEX_NONE;
            if (!Line.FindChar(TEXT('['), Open) || !Line.FindLastChar(TEXT(']'), Close) || Close <= Open)
            {
                continue;
            }

            TArray<FString> Arms;
            Line.Mid(Open + 1, Close - Open - 1).ParseIntoArray(Arms, TEXT(","), /*InCullEmpty=*/true);

            TArray<FString> Targets;
            for (const FString& Arm : Arms)
            {
                const int32 Arrow = Arm.Find(TEXT("->"));
                if (Arrow == INDEX_NONE)
                {
                    continue;
                }
                FString Target = Arm.Mid(Arrow + 2).TrimStartAndEnd();
                Target.RemoveFromStart(TEXT("@"));
                Targets.Add(Target);
            }

            if (Targets.Num() == 2)
            {
                OutFirst = Targets[0];
                OutSecond = Targets[1];
                return true;
            }
        }
        return false;
    }
}

// ============================================================================
// Bpir.Compiler.MultiBranchReturnKeepsBothBranches
//
// Compiles the two-branch function above and asserts the graph shape the fix
// guarantees: one FunctionResult per return, each branch's own values wired into
// its own result node, and no producer left dangling.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirMultiBranchReturnKeepsBothBranchesTest,
    "PinWright.bpir.compiler.MultiBranchReturnKeepsBothBranches",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirMultiBranchReturnKeepsBothBranchesTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("MultiBranchReturnBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(MultiBranchReturnSource);
    for (const FCompileError& Err : Result.Errors)
    {
        AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
    }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    UEdGraph* Graph = FindMultiBranchReturnGraph(BP, TEXT("PickValue"));
    TestNotNull(TEXT("Function graph 'PickValue' exists"), Graph);
    if (!Graph) return false;

    // 1. One result node per `return` statement — the defect produced exactly one.
    TArray<UK2Node_FunctionResult*> Results = CollectMultiBranchReturnResults(Graph);
    TestEqual(TEXT("Each return statement owns a FunctionResult node"), Results.Num(), 2);
    if (Results.Num() != 2) return false;

    // 2. Both outputs wired on BOTH result nodes, from distinct producers.
    TArray<UEdGraphNode*> ValueSources;
    TArray<UEdGraphNode*> DoubledSources;
    for (UK2Node_FunctionResult* ResultNode : Results)
    {
        UEdGraphPin* ValuePin = ResultNode->FindPin(TEXT("Value"), EGPD_Input);
        UEdGraphPin* DoubledPin = ResultNode->FindPin(TEXT("Doubled"), EGPD_Input);
        TestNotNull(TEXT("Result node carries the 'Value' output pin"), ValuePin);
        TestNotNull(TEXT("Result node carries the 'Doubled' output pin"), DoubledPin);

        UEdGraphNode* ValueSource = MultiBranchReturnLinkSource(ValuePin);
        UEdGraphNode* DoubledSource = MultiBranchReturnLinkSource(DoubledPin);
        TestNotNull(TEXT("'Value' is wired on every return"), ValueSource);
        TestNotNull(TEXT("'Doubled' is wired on every return"), DoubledSource);
        if (ValueSource) ValueSources.Add(ValueSource);
        if (DoubledSource) DoubledSources.Add(DoubledSource);
    }
    TestEqual(TEXT("Both returns wired a 'Value'"), ValueSources.Num(), 2);
    TestEqual(TEXT("Both returns wired a 'Doubled'"), DoubledSources.Num(), 2);
    if (ValueSources.Num() == 2)
    {
        TestTrue(TEXT("Each branch returns its OWN 'Value' producer, not a shared one"),
            ValueSources[0] != ValueSources[1]);
    }
    if (DoubledSources.Num() == 2)
    {
        TestTrue(TEXT("Each branch returns its OWN 'Doubled' producer, not a shared one"),
            DoubledSources[0] != DoubledSources[1]);
    }

    // 3. The branch's two exec outputs must land on DIFFERENT nodes. The defect's
    //    signature was `[false -> @merge, true -> @merge]` — both arms on one node.
    UK2Node_IfThenElse* BranchNode = FindNodeOfType<UK2Node_IfThenElse>(Graph);
    TestNotNull(TEXT("Branch node exists"), BranchNode);
    if (BranchNode)
    {
        UEdGraphNode* ThenTarget = MultiBranchReturnLinkSource(
            BranchNode->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output));
        UEdGraphNode* ElseTarget = MultiBranchReturnLinkSource(
            BranchNode->FindPin(UEdGraphSchema_K2::PN_Else, EGPD_Output));
        TestNotNull(TEXT("Branch 'then' is wired"), ThenTarget);
        TestNotNull(TEXT("Branch 'else' is wired"), ElseTarget);
        if (ThenTarget && ElseTarget)
        {
            TestTrue(TEXT("Branch arms reach different nodes (not one merged return)"),
                ThenTarget != ElseTarget);
        }
    }

    // 4. No producer left orphaned: every emitted pure call feeds something. The
    //    defect left the losing branch's nodes in the graph with nothing attached.
    int32 CallNodeCount = 0;
    int32 OrphanedCallNodes = 0;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
        if (!Call) continue;
        ++CallNodeCount;

        bool bAnyOutputLinked = false;
        for (UEdGraphPin* Pin : Call->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Output && Pin->LinkedTo.Num() > 0)
            {
                bAnyOutputLinked = true;
                break;
            }
        }
        if (!bAnyOutputLinked)
        {
            ++OrphanedCallNodes;
            AddError(FString::Printf(TEXT("Orphaned node left in graph: %s"),
                *Call->GetNodeTitle(ENodeTitleType::ListView).ToString()));
        }
    }
    TestEqual(TEXT("Both branches' producers were emitted"), CallNodeCount, 4);
    TestEqual(TEXT("No producer left orphaned"), OrphanedCallNodes, 0);

    return true;
}

// ============================================================================
// Bpir.RoundTrip.MultiBranchReturn
//
// Decompiling the same function must show BOTH returns with their own data, and
// the emitted text must recompile to the same shape. The defect decompiled to a
// single `@merge` block carrying one branch's values and printed the losing
// branch's nodes after the `return` line as dead text.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirMultiBranchReturnRoundTripTest,
    "PinWright.bpir.round_trip.MultiBranchReturn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirMultiBranchReturnRoundTripTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("MultiBranchReturnRoundTripBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(MultiBranchReturnSource);
    for (const FCompileError& Err : CompileResult.Errors)
    {
        AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.DecompileFunction(TEXT("PickValue"));
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess)
    {
        for (const FBpirWarning& Warn : DecompileResult.Warnings)
        {
            AddError(FString::Printf(TEXT("Decompile warning: %s"), *Warn.Text));
        }
        return false;
    }

    const FString& Output = DecompileResult.BpirText;
    AddInfo(FString::Printf(TEXT("Decompiled PickValue:\n%s"), *Output));

    // Both returns survive, each with its own named data.
    TestEqual(TEXT("Decompiled text carries both returns"),
        CountMultiBranchReturnOccurrences(Output, TEXT("return (")), 2);
    TestEqual(TEXT("Both branches' producers survive"),
        CountMultiBranchReturnOccurrences(Output, TEXT("Add_IntInt")), 2);
    TestTrue(TEXT("Return values are emitted by name (multi-output form)"),
        Output.Contains(TEXT("Value:")) && Output.Contains(TEXT("Doubled:")));

    // A branch whose two arms carry the same label is not a branch — that was the
    // defect's visible signature in the decompile.
    FString FirstTarget;
    FString SecondTarget;
    const bool bFoundTargets = MultiBranchReturnBranchTargets(Output, FirstTarget, SecondTarget);
    TestTrue(TEXT("Decompiled branch line exposes two exec targets"), bFoundTargets);
    if (bFoundTargets)
    {
        TestTrue(FString::Printf(TEXT("Branch arms carry distinct labels (got '@%s' and '@%s')"),
            *FirstTarget, *SecondTarget), FirstTarget != SecondTarget);
    }

    // The emitted text must recompile, and rebuild the same two-result shape.
    UBlueprint* BP2 = CreateTransientTestBP(TEXT("MultiBranchReturnRecompileBP"));
    TestNotNull(TEXT("Recompile target Blueprint created"), BP2);
    if (!BP2) return false;

    FBpirCompiler Compiler2(BP2);
    FCompileResult RecompileResult = Compiler2.Compile(Output);
    for (const FCompileError& Err : RecompileResult.Errors)
    {
        AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
    }
    TestTrue(TEXT("Re-compile of decompiled output succeeded"), RecompileResult.bSuccess);
    if (!RecompileResult.bSuccess) return false;

    UEdGraph* Graph2 = FindMultiBranchReturnGraph(BP2, TEXT("PickValue"));
    TestNotNull(TEXT("Re-compiled function graph exists"), Graph2);
    if (!Graph2) return false;

    TestEqual(TEXT("Re-compiled graph keeps one FunctionResult per return"),
        CollectMultiBranchReturnResults(Graph2).Num(), 2);

    return true;
}
