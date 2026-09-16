// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-get-node-details-batch-no-projection-spills.
// blueprint.graph.get_node_details_batch always emitted the full per-node property
// set (nodeId/nodeName/nodeType/nodeTitle/nodeComment/x/y + the heavy pins-with-defaults
// array + nodeState) for every node, so even a 2-node positions cross-check overflowed
// the inline budget and spilled to disk. The fix mirrors blueprint.graph.get_nodes'
// projection lever onto the batch: an optional `fields` allow-list / `namesOnly` shorthand
// (parsed by the shared FHandlerContext::ReadFieldProjection, applied inside the shared
// BuildNodeDetailsJson) narrows each node's `details` object to the requested keys, so a
// targeted positions/wiring cross-check stays inline. `includePinDefaults` sheds the
// per-pin default payload (the dominant bloat) while keeping the pins adjacency.
//
// Counterfactual: revert the projection and get_node_details_batch again calls
// BuildNodeDetailsJson(TargetNode) bare, so the fields:["nodeId","x","y"] and namesOnly
// calls below would still carry the heavy `pins`/`nodeState` keys — every "projected out"
// TestFalse flips to failure, and the param-registration TestNotNull checks fail too.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Tests/Bpir/CompilerTestUtils.h"
#include "Dom/JsonObject.h"

#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_CallFunction.h"
#include "EdGraph/EdGraph.h"

namespace
{
    // Fixture: a transient Actor Blueprint whose EventGraph holds a single PrintString
    // UK2Node_CallFunction. PrintString is exactly the heavy default-laden node the ticket
    // cites (InString, TextColor FLinearColor, Duration, bPrintToScreen/bPrintToLog pin
    // defaults) — the per-node shape that overflows the inline budget in a batch.
    struct FBatchProjectionFixture
    {
        UBlueprint* BP = nullptr;
        UEdGraph* EventGraph = nullptr;
        UK2Node_CallFunction* PrintNode = nullptr;
        FString NodeGuid;
    };

    FBatchProjectionFixture BuildBatchProjectionFixture(FAutomationTestBase& Test)
    {
        FBatchProjectionFixture F;

        F.BP = CompilerTestUtils::CreateTransientTestBP(TEXT("BatchProjectionBP"));
        if (!F.BP)
        {
            Test.AddError(TEXT("Failed to create transient test Blueprint"));
            return F;
        }

        F.EventGraph = FBlueprintEditorUtils::FindEventGraph(F.BP);
        if (!F.EventGraph)
        {
            Test.AddError(TEXT("Blueprint has no EventGraph"));
            return F;
        }

        F.PrintNode = CompilerTestUtils::SpawnPrintStringCall(F.EventGraph, 320, 176);
        if (!F.PrintNode)
        {
            Test.AddError(TEXT("Failed to spawn PrintString node in EventGraph"));
            return F;
        }
        F.NodeGuid = F.PrintNode->NodeGuid.ToString();
        return F;
    }

    // Dispatches blueprint.graph.get_node_details_batch for the single fixture node with the
    // given extra args merged in, and returns that node's nested `details` object (found by
    // the item's top-level nodeId echo). Adds a test error and returns null if the call did
    // not succeed or the item/details are missing.
    TSharedPtr<FJsonObject> FetchDetails(FAutomationTestBase& Test,
        const FBatchProjectionFixture& F,
        const TFunction<void(const TSharedPtr<FJsonObject>&)>& AddArgs,
        const TCHAR* Label)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), F.BP->GetPathName());
        Payload->SetStringField(TEXT("graphName"), F.EventGraph->GetName());
        TArray<TSharedPtr<FJsonValue>> Ids;
        Ids.Add(MakeShared<FJsonValueString>(F.NodeGuid));
        Payload->SetArrayField(TEXT("nodeIds"), Ids);
        if (AddArgs)
        {
            AddArgs(Payload);
        }

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("blueprint.graph.get_node_details_batch"), Payload, Capture);
        Test.TestTrue(*FString::Printf(TEXT("[%s] handler registered"), Label), bFound);
        Test.TestTrue(*FString::Printf(TEXT("[%s] call succeeds"), Label), Capture.bSuccess);
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            Test.AddError(*FString::Printf(TEXT("[%s] get_node_details_batch did not succeed"), Label));
            return nullptr;
        }

        // The per-item top-level nodeId echo stays regardless of projection, so results
        // remain correlatable; find the item by it, then read its `details` sub-object.
        TSharedPtr<FJsonObject> Item =
            JsonArrayFindObjectByStringField(Capture.Result, TEXT("items"), TEXT("nodeId"), F.NodeGuid);
        if (!Item.IsValid())
        {
            Test.AddError(*FString::Printf(TEXT("[%s] no items[] entry echoing the node id"), Label));
            return nullptr;
        }
        const TSharedPtr<FJsonObject>* DetailsPtr = nullptr;
        if (!Item->TryGetObjectField(TEXT("details"), DetailsPtr) || !DetailsPtr || !(*DetailsPtr).IsValid())
        {
            Test.AddError(*FString::Printf(TEXT("[%s] item has no details object"), Label));
            return nullptr;
        }
        return *DetailsPtr;
    }
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetNodeDetailsBatchFieldProjectionTest,
    "PinWright.blueprint.graph.get_node_details_batch.FieldProjection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGetNodeDetailsBatchFieldProjectionTest::RunTest(const FString& Parameters)
{
    // Discoverability contract: the narrowing levers must be registered params so callers
    // can find them. Reverting the fix drops these registrations (TestNotNull fails).
    TestNotNull(TEXT("fields param is registered"),
        GetRegisteredParamSpec(TEXT("blueprint.graph.get_node_details_batch"), TEXT("fields")));
    TestNotNull(TEXT("namesOnly param is registered"),
        GetRegisteredParamSpec(TEXT("blueprint.graph.get_node_details_batch"), TEXT("namesOnly")));
    TestNotNull(TEXT("includePinDefaults param is registered"),
        GetRegisteredParamSpec(TEXT("blueprint.graph.get_node_details_batch"), TEXT("includePinDefaults")));

    FBatchProjectionFixture F = BuildBatchProjectionFixture(*this);
    if (!F.BP || !F.EventGraph || !F.PrintNode)
    {
        return true; // BuildBatchProjectionFixture already AddError'd — this is a failure, not a skip.
    }

    // 1) No projection — the full per-node shape (unchanged default). The heavy pins +
    //    nodeState keys are present, proving the fixture node really carries the bloat.
    if (TSharedPtr<FJsonObject> Full = FetchDetails(*this, F, nullptr, TEXT("full")))
    {
        TestTrue(TEXT("full shape has pins"), Full->HasField(TEXT("pins")));
        TestTrue(TEXT("full shape has nodeState"), Full->HasField(TEXT("nodeState")));
        TestTrue(TEXT("full shape has nodeTitle"), Full->HasField(TEXT("nodeTitle")));
        TestTrue(TEXT("full shape has x"), Full->HasField(TEXT("x")));
        TestTrue(TEXT("full shape has y"), Full->HasField(TEXT("y")));
    }

    // 2) fields:["nodeId","x","y"] — only the requested keys survive; the heavy pins /
    //    nodeState and the other identity keys are projected out. This is the assertion
    //    that flips to failure if the projection lever is reverted.
    if (TSharedPtr<FJsonObject> Proj = FetchDetails(*this, F,
        [](const TSharedPtr<FJsonObject>& P)
        {
            TArray<TSharedPtr<FJsonValue>> Fields;
            Fields.Add(MakeShared<FJsonValueString>(TEXT("nodeId")));
            Fields.Add(MakeShared<FJsonValueString>(TEXT("x")));
            Fields.Add(MakeShared<FJsonValueString>(TEXT("y")));
            P->SetArrayField(TEXT("fields"), Fields);
        }, TEXT("fields")))
    {
        TestTrue(TEXT("fields projection keeps x"), Proj->HasField(TEXT("x")));
        TestTrue(TEXT("fields projection keeps y"), Proj->HasField(TEXT("y")));
        TestTrue(TEXT("fields projection keeps nodeId"), Proj->HasField(TEXT("nodeId")));
        TestFalse(TEXT("fields projection drops pins"), Proj->HasField(TEXT("pins")));
        TestFalse(TEXT("fields projection drops nodeState"), Proj->HasField(TEXT("nodeState")));
        TestFalse(TEXT("fields projection drops nodeTitle"), Proj->HasField(TEXT("nodeTitle")));
        TestFalse(TEXT("fields projection drops nodeComment"), Proj->HasField(TEXT("nodeComment")));
    }

    // 3) namesOnly:true — the light identification set (nodeId/nodeName/nodeType/nodeTitle/
    //    x/y) with the heavy pins/nodeState dropped.
    if (TSharedPtr<FJsonObject> Names = FetchDetails(*this, F,
        [](const TSharedPtr<FJsonObject>& P) { P->SetBoolField(TEXT("namesOnly"), true); },
        TEXT("namesOnly")))
    {
        TestTrue(TEXT("namesOnly keeps nodeTitle"), Names->HasField(TEXT("nodeTitle")));
        TestTrue(TEXT("namesOnly keeps x"), Names->HasField(TEXT("x")));
        TestTrue(TEXT("namesOnly keeps y"), Names->HasField(TEXT("y")));
        TestFalse(TEXT("namesOnly drops pins"), Names->HasField(TEXT("pins")));
        TestFalse(TEXT("namesOnly drops nodeState"), Names->HasField(TEXT("nodeState")));
    }

    return true;
}
