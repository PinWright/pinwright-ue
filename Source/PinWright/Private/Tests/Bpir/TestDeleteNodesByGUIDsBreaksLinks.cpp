// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests that FBpirCompiler::DeleteNodesByGUIDs breaks sibling pin LinkedTo arrays.
// Regression test for B-bp-saved-state-corruption-mcp-edits:
//   raw Graph->RemoveNode() does NOT call BreakNodeLinks, leaving stale pointers that
//   crash FKismetCompilerContext::ReplaceConvertibleDelegates on next BP load.
#include "Misc/AutomationTest.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_ExecutionSequence.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Compiler/BpirCompiler.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirDeleteNodesByGUIDsBreaksLinksTest,
    "PinWright.bpir.delete_nodes_by_guids.BreaksLinkedTo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirDeleteNodesByGUIDsBreaksLinksTest::RunTest(const FString& Parameters)
{
    // Transient BP is sufficient: this test calls FBpirCompiler::DeleteNodesByGUIDs
    // directly and does not go through the RPC handler path (which uses LoadBlueprintAsset).
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        GetTransientPackage(),
        FName(*FString::Printf(TEXT("TestDeleteNodesByGUIDs_%d"), FMath::Rand())),
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());

    if (!TestNotNull(TEXT("Blueprint created"), BP))
        return true;

    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(BP);
    if (!TestNotNull(TEXT("EventGraph exists"), EventGraph))
        return true;

    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    if (!TestNotNull(TEXT("K2 schema valid"), Schema))
        return true;

    // Node A: custom event whose exec-output we will wire to Node B.
    UK2Node_CustomEvent* NodeA = NewObject<UK2Node_CustomEvent>(EventGraph);
    NodeA->CustomFunctionName = FName(TEXT("EventA_DeleteTest"));
    NodeA->CreateNewGuid();
    EventGraph->AddNode(NodeA, /*bFromUI=*/false, /*bSelectNewNode=*/false);
    NodeA->AllocateDefaultPins();

    // Node B: execution sequence — custom events have no exec-input pin,
    // so we need a node that exposes PN_Execute (the "then -> execute" link target).
    UK2Node_ExecutionSequence* NodeB = NewObject<UK2Node_ExecutionSequence>(EventGraph);
    NodeB->CreateNewGuid();
    EventGraph->AddNode(NodeB, /*bFromUI=*/false, /*bSelectNewNode=*/false);
    NodeB->AllocateDefaultPins();

    // Find Node A's exec-output (the "then" pin) and Node B's exec-input ("execute").
    UEdGraphPin* PinAOut = NodeA->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
    UEdGraphPin* PinBIn  = NodeB->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);

    if (!TestNotNull(TEXT("Node A exec-output pin"), PinAOut) ||
        !TestNotNull(TEXT("Node B exec-input pin"),  PinBIn))
    {
        return true;
    }

    // Wire A.then -> B.execute
    const FPinConnectionResponse ConnectResponse = Schema->CanCreateConnection(PinAOut, PinBIn);
    if (!TestTrue(TEXT("Pins can be connected"), ConnectResponse.Response != CONNECT_RESPONSE_DISALLOW))
        return true;

    Schema->TryCreateConnection(PinAOut, PinBIn);

    // Precondition: B's exec-input should now have one LinkedTo entry (pointing at A).
    if (!TestEqual(TEXT("B exec-input LinkedTo before deletion"), PinBIn->LinkedTo.Num(), 1))
        return true;

    // Delete Node A via the API under test.
    TArray<FGuid> ToDelete;
    ToDelete.Add(NodeA->NodeGuid);
    const bool bDeleted = FBpirCompiler::DeleteNodesByGUIDs(BP, ToDelete);
    TestTrue(TEXT("DeleteNodesByGUIDs returned true"), bDeleted);

    // The fix: FBlueprintEditorUtils::RemoveNode calls BreakNodeLinks before removing,
    // so B's exec-input pin's LinkedTo must now be empty.
    TestEqual(TEXT("B exec-input LinkedTo after deletion (must be 0 — no stale pointer)"),
        PinBIn->LinkedTo.Num(), 0);

    // B itself must still be in the graph (only A was deleted).
    TestTrue(TEXT("Node B still present in graph"), EventGraph->Nodes.Contains(NodeB));

    return true;
}
