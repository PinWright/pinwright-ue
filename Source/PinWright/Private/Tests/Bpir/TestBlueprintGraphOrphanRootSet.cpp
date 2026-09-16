// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-orphan-sweep-treats-enhanced-input-graph-as-dead.
//
// The shared reachability collector used to seed only from a hard-coded list of entry
// node CLASSES (UK2Node_Event and friends). UK2Node_EnhancedInputAction is a plain
// UK2Node that expands into bindings at compile time, so it matched none of them: on a
// UE5 Blueprint that takes player input, every input node AND everything downstream of it
// read as unreachable, and delete_orphaned_nodes would have swept the whole input graph
// while leaving the Blueprint compiling clean.
//
// The fix seeds from the engine's own compile root set (GatherRootSet in KismetCompiler.cpp
// with bIncludeNodesThatCouldBeExpandedToRootSet=true), whose operative clause is a SHAPE
// test — an impure UK2Node with no input pins at all — not a class list.
//
// Counterfactual: revert IsBlueprintEntryNode to the class-only list and
// EnhancedInputEntryNotOrphaned reports 2 orphans (the input node plus its wired handler)
// instead of the 1 planted dead node, and EntryRootSetPredicate's first TestTrue flips.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Dom/JsonObject.h"
#include "BpirGraphTestHelpers.h"

#include "Engine/Blueprint.h"
#include "K2Node.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_IfThenElse.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "UObject/UObjectGlobals.h"

#include "Handlers/Blueprint/BlueprintHandlerUtils.h"

namespace
{
    // Full object path of the Enhanced Input event node. Resolved by name on purpose:
    // the class lives in the EnhancedInput plugin's uncooked-only InputBlueprintNodes
    // module, which PinWright deliberately does not link (see IsEngineCompileRootSetNode).
    const TCHAR* const EnhancedInputActionNodeClassPath =
        TEXT("/Script/InputBlueprintNodes.K2Node_EnhancedInputAction");

    UClass* RootSetTest_FindEnhancedInputActionClass()
    {
        return FindObject<UClass>(nullptr, EnhancedInputActionNodeClassPath);
    }

    // FGraphNodeCreator/BpirGraphTestHelpers::AddNodeToGraph are templated on the concrete
    // type; this is the same finalize sequence driven from a runtime UClass.
    UEdGraphNode* RootSetTest_AddNodeOfClass(UEdGraph* Graph, UClass* NodeClass)
    {
        if (!Graph || !NodeClass)
        {
            return nullptr;
        }

        UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, NodeClass);
        Node->CreateNewGuid();
        Node->PostPlacedNewNode();
        Node->AllocateDefaultPins();
        Graph->AddNode(Node, /*bFromUI=*/true, /*bSelectNewNode=*/false);
        return Node;
    }

    UEdGraphPin* RootSetTest_FindExecOutputPin(UEdGraphNode* Node, const TCHAR* PreferredName)
    {
        if (!Node)
        {
            return nullptr;
        }

        UEdGraphPin* FirstExecOutput = nullptr;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin) continue;
            if (Pin->Direction != EGPD_Output) continue;
            if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;

            if (Pin->PinName == FName(PreferredName))
            {
                return Pin;
            }
            if (!FirstExecOutput)
            {
                FirstExecOutput = Pin;
            }
        }

        return FirstExecOutput;
    }

    bool RootSetTest_HasAnyInputPin(UEdGraphNode* Node)
    {
        if (!Node)
        {
            return true;
        }

        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Input)
            {
                return true;
            }
        }
        return false;
    }
} // namespace

// ============================================================================
// Test: an Enhanced Input event node and the handler wired to it are not orphans.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrphanDetectionEnhancedInputEntryNotOrphanedTest,
    "PinWright.blueprint.graph.find_orphaned_nodes.EnhancedInputEntryNotOrphaned",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrphanDetectionEnhancedInputEntryNotOrphanedTest::RunTest(const FString& Parameters)
{
    UClass* InputNodeClass = RootSetTest_FindEnhancedInputActionClass();
    if (!InputNodeClass)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("%s is not loaded on this host (EnhancedInput plugin disabled or the class moved modules), so the ticket's node shape cannot be built."),
                EnhancedInputActionNodeClassPath));
        return true;
    }

    UBlueprint* BP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph")); return false; }

    // A conventional live chain, so the graph is not degenerate.
    UK2Node_Event* BeginPlayNode = BpirGraphTestHelpers::EnsureBeginPlayNode(EventGraph);
    BpirGraphTestHelpers::WireExec(BeginPlayNode, BpirGraphTestHelpers::AddPrintStringNode(EventGraph));

    UEdGraphNode* InputNode = RootSetTest_AddNodeOfClass(EventGraph, InputNodeClass);
    if (!InputNode) { AddError(TEXT("Failed to create the Enhanced Input event node")); return false; }

    // The premise the fix rests on. If the engine ever gives this node an input pin, the
    // shape clause stops applying and this test would be asserting the wrong thing.
    if (RootSetTest_HasAnyInputPin(InputNode))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("%s now allocates input pins, so it is no longer the output-only shape this regression covers."),
                EnhancedInputActionNodeClassPath));
        return true;
    }

    UEdGraphPin* TriggeredPin = RootSetTest_FindExecOutputPin(InputNode, TEXT("Triggered"));
    if (!TriggeredPin) { AddError(TEXT("Enhanced Input node exposed no exec output pin")); return false; }

    UK2Node_CallFunction* Handler = BpirGraphTestHelpers::AddPrintStringNode(EventGraph);
    UEdGraphPin* HandlerExecIn = Handler ? Handler->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input) : nullptr;
    if (!HandlerExecIn) { AddError(TEXT("Handler node has no exec input pin")); return false; }
    TriggeredPin->MakeLinkTo(HandlerExecIn);

    // Counterfactual anchor: a genuinely dead node, so the test cannot pass by the sweep
    // having become a no-op.
    UK2Node_IfThenElse* DeadBranch = BpirGraphTestHelpers::AddNodeToGraph<UK2Node_IfThenElse>(EventGraph);
    const FString DeadBranchId = DeadBranch->NodeGuid.ToString();
    const FString InputNodeId = InputNode->NodeGuid.ToString();
    const FString HandlerId = Handler->NodeGuid.ToString();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.graph.find_orphaned_nodes"), Payload, Capture);
    TestTrue(TEXT("find_orphaned_nodes handler found"), bFound);
    TestTrue(TEXT("find_orphaned_nodes succeeded"), Capture.bSuccess);

    if (!Capture.Result.IsValid())
    {
        AddError(TEXT("No result JSON returned from find_orphaned_nodes"));
        return true;
    }

    const double OrphanedCount = Capture.Result->GetNumberField(TEXT("orphanedCount"));
    TestEqual(TEXT("only the planted dead node is reported"), OrphanedCount, 1.0);

    const TArray<TSharedPtr<FJsonValue>>* OrphanedNodes = nullptr;
    if (Capture.Result->TryGetArrayField(TEXT("orphanedNodes"), OrphanedNodes) && OrphanedNodes)
    {
        bool bReportedInputNode = false;
        bool bReportedHandler = false;
        bool bReportedDeadBranch = false;
        for (const TSharedPtr<FJsonValue>& Value : *OrphanedNodes)
        {
            const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
            if (!Entry.IsValid()) continue;

            const FString NodeId = Entry->GetStringField(TEXT("nodeId"));
            bReportedInputNode |= NodeId.Equals(InputNodeId, ESearchCase::IgnoreCase);
            bReportedHandler |= NodeId.Equals(HandlerId, ESearchCase::IgnoreCase);
            bReportedDeadBranch |= NodeId.Equals(DeadBranchId, ESearchCase::IgnoreCase);
        }

        TestFalse(TEXT("the Enhanced Input event node is not reported orphaned"), bReportedInputNode);
        TestFalse(TEXT("the handler wired to its Triggered pin is not reported orphaned"), bReportedHandler);
        TestTrue(TEXT("the planted dead node is still reported orphaned"), bReportedDeadBranch);
    }
    else
    {
        AddError(TEXT("Expected an orphanedNodes array"));
    }

    return true;
}

// ============================================================================
// Test: the entry predicate itself — shape, not class list — and its discrimination.
// This is what get_execution_flow's `entryPoints` and the BPIR decompiler read.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrphanDetectionEntryRootSetPredicateTest,
    "PinWright.blueprint.graph.find_orphaned_nodes.EntryRootSetPredicate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrphanDetectionEntryRootSetPredicateTest::RunTest(const FString& Parameters)
{
    UClass* InputNodeClass = RootSetTest_FindEnhancedInputActionClass();
    if (!InputNodeClass)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("%s is not loaded on this host, so the entry predicate cannot be probed with an out-of-module event-shaped node."),
                EnhancedInputActionNodeClassPath));
        return true;
    }

    UBlueprint* BP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph")); return false; }

    UEdGraphNode* InputNode = RootSetTest_AddNodeOfClass(EventGraph, InputNodeClass);
    if (!InputNode) { AddError(TEXT("Failed to create the Enhanced Input event node")); return false; }

    // Not a UK2Node_Event — that is the whole reason a class-typed seed missed it.
    TestFalse(TEXT("fixture node does not derive from UK2Node_Event"), InputNode->IsA<UK2Node_Event>());

    TestTrue(TEXT("an out-of-module event-shaped node counts as an entry"),
        BlueprintHandlerUtils::IsBlueprintEntryNode(InputNode));

    // Discrimination: an impure node that HAS input pins is still sweepable, so the
    // broadened predicate did not simply stop reporting anything.
    UK2Node_IfThenElse* DeadBranch = BpirGraphTestHelpers::AddNodeToGraph<UK2Node_IfThenElse>(EventGraph);
    TestFalse(TEXT("an unwired Branch is not promoted to an entry"),
        BlueprintHandlerUtils::IsBlueprintEntryNode(DeadBranch));

    // The collector that backs get_execution_flow's entryPoints must list it too.
    TArray<UEdGraphNode*> EntryNodes;
    BlueprintHandlerUtils::CollectEntryNodesRecursive(EventGraph, EntryNodes);
    TestTrue(TEXT("CollectEntryNodesRecursive lists the input node"), EntryNodes.Contains(InputNode));

    return true;
}
