// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-get-graph-details-light-inventory-undiscoverable.
//
// blueprint.graph.get_graph_details already had a light default (omit includeNodeDetails ->
// per node just {nodeId,nodeName,nodeTitle} next to a top-level nodeCount), while
// includeNodeDetails:true switches to the heavy full-pins BuildNodeDetailsJson shape that
// overflows the inline budget and spills to disk. But get_graph_details was the LONE
// graph-inspection reader that had no namesOnly/fields projection lever (its two siblings
// blueprint.graph.get_nodes and get_node_details_batch already honor namesOnly/fields via
// FHandlerContext::ReadFieldProjection), so a caller's reflexive namesOnly guess was rejected
// UNKNOWN_PARAMS. The fix adds the same projection lever: fields (allow-list) / namesOnly
// (shorthand) parsed by ReadFieldProjection and applied through the shared BuildNodeDetailsJson
// field filter, taking precedence over includeNodeDetails so an overflowed includeNodeDetails:true
// call can add namesOnly to the same args to dial back to an inline shape.
//
// Counterfactual (what fails if the fix is reverted):
//  - fields/namesOnly are no longer registered params  -> the TestNotNull registration checks fail.
//  - the handler no longer reads the projection, so namesOnly falls through to the default light
//    {nodeId,nodeName,nodeTitle} shape which carries NO nodeType/x/y  -> the "namesOnly adds x/y/
//    nodeType" TestEqual checks fail; a fields:["nodeId","x"] call still returns nodeTitle and no x
//    -> those checks fail; and includeNodeDetails:true + namesOnly:true still emits the heavy pins
//    -> the precedence "drops pins" check fails.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Tests/Bpir/CompilerTestUtils.h"
#include "Dom/JsonObject.h"

#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "EdGraph/EdGraph.h"

namespace
{
    // Returns the number of entries in the response's `nodes` array whose object carries FieldName.
    // Returns -1 if the `nodes` array is missing (a hard failure signal, not "zero matched").
    int32 CountNodesWithField(const TSharedPtr<FJsonObject>& Result, const TCHAR* FieldName)
    {
        const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
        {
            return -1;
        }
        int32 Count = 0;
        for (const TSharedPtr<FJsonValue>& NodeVal : *Nodes)
        {
            const TSharedPtr<FJsonObject>* NodeObj = nullptr;
            if (NodeVal.IsValid() && NodeVal->TryGetObject(NodeObj) && NodeObj && (*NodeObj).IsValid()
                && (*NodeObj)->HasField(FieldName))
            {
                ++Count;
            }
        }
        return Count;
    }

    int32 NumNodes(const TSharedPtr<FJsonObject>& Result)
    {
        const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
        {
            return -1;
        }
        return Nodes->Num();
    }

    // Dispatches blueprint.graph.get_graph_details on the fixture graph with the given extra args
    // merged in, and returns the successful Result object (or null after AddError on any failure).
    TSharedPtr<FJsonObject> FetchGraph(FAutomationTestBase& Test,
        UBlueprint* BP, UEdGraph* Graph,
        const TFunction<void(const TSharedPtr<FJsonObject>&)>& AddArgs,
        const TCHAR* Label)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());
        Payload->SetStringField(TEXT("graphName"), Graph->GetName());
        if (AddArgs)
        {
            AddArgs(Payload);
        }

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("blueprint.graph.get_graph_details"), Payload, Capture);
        Test.TestTrue(*FString::Printf(TEXT("[%s] handler registered"), Label), bFound);
        Test.TestTrue(*FString::Printf(TEXT("[%s] call succeeds"), Label), Capture.bSuccess);
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            Test.AddError(*FString::Printf(TEXT("[%s] get_graph_details did not succeed"), Label));
            return nullptr;
        }
        return Capture.Result;
    }
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetGraphDetailsFieldProjectionTest,
    "PinWright.blueprint.graph.get_graph_details.FieldProjection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGetGraphDetailsFieldProjectionTest::RunTest(const FString& Parameters)
{
    using namespace CompilerTestUtils;

    // Discoverability contract: the projection levers must be registered params so the dispatcher
    // accepts them (its UNKNOWN_PARAMS gate builds its known set from exactly these) and callers can
    // find them. Reverting the fix drops these registrations -> TestNotNull fails.
    TestNotNull(TEXT("fields param is registered"),
        GetRegisteredParamSpec(TEXT("blueprint.graph.get_graph_details"), TEXT("fields")));
    TestNotNull(TEXT("namesOnly param is registered"),
        GetRegisteredParamSpec(TEXT("blueprint.graph.get_graph_details"), TEXT("namesOnly")));

    // Fixture: a transient Actor Blueprint whose EventGraph holds a heavy default-laden
    // PrintString CallFunction (InString / TextColor FLinearColor / Duration / bPrintToScreen /
    // bPrintToLog pin defaults) — exactly the per-node bloat that overflows under includeNodeDetails.
    UBlueprint* BP = CreateTransientTestBP(TEXT("GraphDetailsProjection"));
    if (!BP || BP->UbergraphPages.Num() == 0)
    {
        AddError(TEXT("Failed to create transient test blueprint with an EventGraph"));
        return true; // hard failure, not a skip
    }
    UEdGraph* EventGraph = BP->UbergraphPages[0];
    UK2Node_CallFunction* PrintNode = SpawnPrintStringCall(EventGraph, 320, 176);
    if (!PrintNode)
    {
        AddError(TEXT("Failed to spawn PrintString node in EventGraph"));
        return true; // hard failure, not a skip
    }

    // 1) Default (omit includeNodeDetails): the light inventory. Every node carries the id fields
    //    and NO heavy pins; the default light shape also carries no x (backward-compat guard —
    //    the fix must not change this path).
    if (TSharedPtr<FJsonObject> Def = FetchGraph(*this, BP, EventGraph, nullptr, TEXT("default")))
    {
        const int32 Total = NumNodes(Def);
        TestTrue(TEXT("default: graph has at least one node"), Total >= 1);
        TestEqual(TEXT("default: every node has nodeId"), CountNodesWithField(Def, TEXT("nodeId")), Total);
        TestEqual(TEXT("default: every node has nodeTitle"), CountNodesWithField(Def, TEXT("nodeTitle")), Total);
        TestEqual(TEXT("default: no node carries the heavy pins"), CountNodesWithField(Def, TEXT("pins")), 0);
        TestEqual(TEXT("default: light shape omits x"), CountNodesWithField(Def, TEXT("x")), 0);
    }

    // 2) includeNodeDetails:true: the heavy shape. Every node gains a pins array — this both proves
    //    the fixture really carries the bloat and pins the unchanged heavy path.
    if (TSharedPtr<FJsonObject> Heavy = FetchGraph(*this, BP, EventGraph,
        [](const TSharedPtr<FJsonObject>& P) { P->SetBoolField(TEXT("includeNodeDetails"), true); },
        TEXT("includeNodeDetails")))
    {
        const int32 Total = NumNodes(Heavy);
        TestTrue(TEXT("includeNodeDetails: graph has at least one node"), Total >= 1);
        TestEqual(TEXT("includeNodeDetails: every node has pins"), CountNodesWithField(Heavy, TEXT("pins")), Total);
    }

    // 3) namesOnly:true: the projected light identification set. Every node gains nodeType/x/y (which
    //    the default light shape lacks) and drops the heavy pins. The x/y/nodeType presence is the
    //    counterfactual — with the fix reverted, namesOnly is ignored and the default shape (no
    //    x/y/nodeType) comes back, flipping these to failure.
    if (TSharedPtr<FJsonObject> Names = FetchGraph(*this, BP, EventGraph,
        [](const TSharedPtr<FJsonObject>& P) { P->SetBoolField(TEXT("namesOnly"), true); },
        TEXT("namesOnly")))
    {
        const int32 Total = NumNodes(Names);
        TestTrue(TEXT("namesOnly: graph has at least one node"), Total >= 1);
        TestEqual(TEXT("namesOnly: every node has nodeType"), CountNodesWithField(Names, TEXT("nodeType")), Total);
        TestEqual(TEXT("namesOnly: every node has x"), CountNodesWithField(Names, TEXT("x")), Total);
        TestEqual(TEXT("namesOnly: every node has y"), CountNodesWithField(Names, TEXT("y")), Total);
        TestEqual(TEXT("namesOnly: no node carries the heavy pins"), CountNodesWithField(Names, TEXT("pins")), 0);
    }

    // 4) fields:["nodeId","x"]: only the requested keys survive. x present, nodeTitle projected out,
    //    pins projected out. On the un-fixed handler `fields` is ignored -> default light shape ->
    //    nodeTitle present and no x, flipping both checks to failure.
    if (TSharedPtr<FJsonObject> Proj = FetchGraph(*this, BP, EventGraph,
        [](const TSharedPtr<FJsonObject>& P)
        {
            TArray<TSharedPtr<FJsonValue>> F;
            F.Add(MakeShared<FJsonValueString>(TEXT("nodeId")));
            F.Add(MakeShared<FJsonValueString>(TEXT("x")));
            P->SetArrayField(TEXT("fields"), F);
        },
        TEXT("fields")))
    {
        const int32 Total = NumNodes(Proj);
        TestTrue(TEXT("fields: graph has at least one node"), Total >= 1);
        TestEqual(TEXT("fields: every node keeps x"), CountNodesWithField(Proj, TEXT("x")), Total);
        TestEqual(TEXT("fields: every node keeps nodeId"), CountNodesWithField(Proj, TEXT("nodeId")), Total);
        TestEqual(TEXT("fields: projects out nodeTitle"), CountNodesWithField(Proj, TEXT("nodeTitle")), 0);
        TestEqual(TEXT("fields: projects out pins"), CountNodesWithField(Proj, TEXT("pins")), 0);
    }

    // 5) Precedence: namesOnly:true wins over includeNodeDetails:true — the recovery path where a
    //    caller who overflowed with includeNodeDetails:true adds namesOnly to the same args. The
    //    light projection drops the pins. On the un-fixed handler namesOnly is ignored and
    //    includeNodeDetails:true still emits the heavy pins -> this check flips to failure.
    if (TSharedPtr<FJsonObject> Both = FetchGraph(*this, BP, EventGraph,
        [](const TSharedPtr<FJsonObject>& P)
        {
            P->SetBoolField(TEXT("includeNodeDetails"), true);
            P->SetBoolField(TEXT("namesOnly"), true);
        },
        TEXT("precedence")))
    {
        const int32 Total = NumNodes(Both);
        TestTrue(TEXT("precedence: graph has at least one node"), Total >= 1);
        TestEqual(TEXT("precedence: namesOnly overrides includeNodeDetails and drops pins"),
            CountNodesWithField(Both, TEXT("pins")), 0);
        TestEqual(TEXT("precedence: projected shape still carries x"),
            CountNodesWithField(Both, TEXT("x")), Total);
    }

    return true;
}
