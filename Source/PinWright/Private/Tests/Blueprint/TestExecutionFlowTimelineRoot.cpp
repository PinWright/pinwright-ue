// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for E-execflow-no-event-entry-timeline-root.
// A Blueprint whose EventGraph is driven only by an auto-play-style K2Node_Timeline
// (no K2Node_Event / K2Node_FunctionEntry node) must not fail get_execution_flow's
// auto-start / bulk-discovery modes with a bare NODE_NOT_FOUND. The shared latent-root
// collector (BlueprintHandlerUtils::CollectLatentExecRootNodes) discovers the Timeline
// (exec-output-only root: Update/Finished drive the chain while Play/Stop are unwired)
// so default-start, entryPointsOnly, and includeAllEntryPoints all resolve it.
//
// Counterfactual: if CollectLatentExecRootNodes (or its fallback call in
// get_execution_flow) is reverted, EntryNodes stays empty, StartNode stays null, and the
// handler returns [NODE_NOT_FOUND] — every TestTrue below flips to failure.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Tests/Bpir/CompilerTestUtils.h"
#include "Tests/Bpir/BpirGraphTestHelpers.h"
#include "Dom/JsonObject.h"

#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_Timeline.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_IfThenElse.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "Kismet/KismetSystemLibrary.h"

#include "Handlers/Blueprint/BlueprintHandlerUtils.h"

namespace
{
    // Blueprint structure created by BuildTimelineRootFixture():
    //
    //   EventGraph
    //   ├─ K2Node_Timeline ("Bounce")  ← exec-output-only root (Play/Stop unwired)
    //   │     Update ──► DownstreamPrint.Execute
    //   └─ DownstreamPrint (UK2Node_CallFunction PrintString)
    //
    // The graph contains zero K2Node_Event / K2Node_FunctionEntry nodes, exactly like
    // the shipped Epic asset BP_Timeline_Ball in the ticket repro.
    struct FTimelineRootFixture
    {
        UBlueprint* BP = nullptr;
        UEdGraph* EventGraph = nullptr;
        UK2Node_Timeline* Timeline = nullptr;
        UK2Node_CallFunction* DownstreamPrint = nullptr;
    };

    FTimelineRootFixture BuildTimelineRootFixture(FAutomationTestBase& Test)
    {
        FTimelineRootFixture F;

        F.BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TimelineRootBP"));
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

        // Reproduce the repro precondition the whole fixture depends on: an EventGraph with
        // no event/function-entry node, so the latent-root fallback path is actually reached.
        // FKismetEditorUtilities::CreateBlueprint(AActor) spawns the editor's default event
        // placeholders (ReceiveBeginPlay / ReceiveActorBeginOverlap, driven by the
        // [DefaultEventNodes] config + bSpawnDefaultBlueprintNodes) into the ubergraph; the
        // shipped BP_Timeline_Ball asset in the ticket has none. Strip every entry node
        // (matching the production IsBlueprintEntryNode predicate, so "entry" means exactly
        // what the handler treats as one) to make the fixture deterministic regardless of
        // the editor's default-node setting.
        {
            TArray<UEdGraphNode*> EntryNodesToRemove;
            for (UEdGraphNode* Node : F.EventGraph->Nodes)
            {
                if (Node && BlueprintHandlerUtils::IsBlueprintEntryNode(Node))
                {
                    EntryNodesToRemove.Add(Node);
                }
            }
            for (UEdGraphNode* Node : EntryNodesToRemove)
            {
                FBlueprintEditorUtils::RemoveNode(F.BP, Node, /*bDontRecompile=*/true);
            }
        }

        // Create the Timeline through the production create_node handler so the timeline
        // template is registered and the node's default pins (Play/Stop/Update/...) exist.
        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("assetPath"), F.BP->GetPathName());
        CreatePayload->SetStringField(TEXT("nodeType"), TEXT("Timeline"));
        CreatePayload->SetStringField(TEXT("target"), TEXT("Bounce"));
        // x/y are required by the create_node handler (it RequireNumber()s both and errors
        // out if either is absent) — without them the Timeline is never created.
        CreatePayload->SetNumberField(TEXT("x"), 120.0);
        CreatePayload->SetNumberField(TEXT("y"), 240.0);

        FTestResponseCapture CreateCapture;
        const bool bCreateFound = InvokeHandlerWithCapture(
            TEXT("blueprint.graph.create_node"), CreatePayload, CreateCapture);
        if (!bCreateFound || !CreateCapture.bSuccess || !CreateCapture.Result.IsValid())
        {
            Test.AddError(TEXT("Failed to create Timeline node via create_node handler"));
            return F;
        }

        FString TimelineNodeId;
        CreateCapture.Result->TryGetStringField(TEXT("nodeId"), TimelineNodeId);
        for (UEdGraphNode* Node : F.EventGraph->Nodes)
        {
            if (Node && Node->NodeGuid.ToString().Equals(TimelineNodeId, ESearchCase::IgnoreCase))
            {
                F.Timeline = Cast<UK2Node_Timeline>(Node);
                break;
            }
        }
        if (!F.Timeline)
        {
            Test.AddError(TEXT("Could not locate created Timeline node in EventGraph"));
            return F;
        }

        // Add a downstream PrintString and wire Timeline.Update (exec output) → its exec input.
        F.DownstreamPrint = NewObject<UK2Node_CallFunction>(F.EventGraph);
        F.DownstreamPrint->CreateNewGuid();
        F.DownstreamPrint->FunctionReference.SetExternalMember(
            GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
            UKismetSystemLibrary::StaticClass());
        F.DownstreamPrint->PostPlacedNewNode();
        F.DownstreamPrint->AllocateDefaultPins();
        F.EventGraph->AddNode(F.DownstreamPrint, /*bFromUI=*/true, /*bSelectNewNode=*/false);
        F.DownstreamPrint->ReconstructNode();

        UEdGraphPin* UpdatePin = F.Timeline->FindPin(FName(TEXT("Update")), EGPD_Output);
        UEdGraphPin* PrintExecPin = F.DownstreamPrint->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
        if (!UpdatePin || !PrintExecPin)
        {
            Test.AddError(TEXT("Could not find Timeline Update output or PrintString exec input pin"));
            return F;
        }
        UpdatePin->MakeLinkTo(PrintExecPin);

        return F;
    }
} // namespace

// ============================================================================
// Unit: the shared latent-root collector recognizes the timeline, excludes
// nodes whose exec input is wired from upstream.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTimelineRootLatentCollectorTest,
    "PinWright.blueprint.graph.get_execution_flow.TimelineRootLatentCollector",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTimelineRootLatentCollectorTest::RunTest(const FString& Parameters)
{
    FTimelineRootFixture F = BuildTimelineRootFixture(*this);
    if (!F.BP || !F.Timeline || !F.DownstreamPrint)
    {
        return true;
    }

    // No event/function entry exists, so the entry collector is empty.
    TArray<UEdGraphNode*> EntryNodes;
    BlueprintHandlerUtils::CollectEntryNodesRecursive(F.EventGraph, EntryNodes);
    TestEqual(TEXT("EventGraph has no IsBlueprintEntryNode entries"), EntryNodes.Num(), 0);

    // The latent-root collector finds the Timeline (exec-output-only root) ...
    TArray<UEdGraphNode*> LatentRoots;
    BlueprintHandlerUtils::CollectLatentExecRootNodes(F.EventGraph, LatentRoots);
    TestTrue(TEXT("Latent-root collector found the Timeline"),
        LatentRoots.Contains(F.Timeline));
    // ... and does NOT mistake the downstream PrintString (exec input wired) for a root.
    TestFalse(TEXT("Downstream PrintString (wired exec input) is not a latent root"),
        LatentRoots.Contains(F.DownstreamPrint));
    return true;
}

// ============================================================================
// get_execution_flow — default auto-start succeeds on a timeline-rooted graph.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTimelineRootDefaultStartTest,
    "PinWright.blueprint.graph.get_execution_flow.TimelineRootDefaultStart",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTimelineRootDefaultStartTest::RunTest(const FString& Parameters)
{
    FTimelineRootFixture F = BuildTimelineRootFixture(*this);
    if (!F.BP || !F.Timeline || !F.DownstreamPrint)
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), F.BP->GetPathName());
    Payload->SetStringField(TEXT("graphName"), F.EventGraph->GetName());
    // No startNodeId — exercise the auto-start path that previously returned NODE_NOT_FOUND.

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("blueprint.graph.get_execution_flow"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Auto-start succeeds on a timeline-rooted graph (no NODE_NOT_FOUND)"),
        Capture.bSuccess);
    TestNotEqual(TEXT("Error code is not NODE_NOT_FOUND"),
        Capture.ErrorCode, FString(TEXT("NODE_NOT_FOUND")));

    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    // The default start node must be the Timeline.
    FString StartNodeId;
    Capture.Result->TryGetStringField(TEXT("startNodeId"), StartNodeId);
    TestEqual(TEXT("Default start node is the Timeline"),
        StartNodeId, F.Timeline->NodeGuid.ToString());

    // entryPoints surfaces the Timeline, flagged as an inferred latent root.
    bool bFlag = false;
    Capture.Result->TryGetBoolField(TEXT("entryPointsAreLatentRoots"), bFlag);
    TestTrue(TEXT("entryPointsAreLatentRoots flag set"), bFlag);
    TestTrue(TEXT("entryPoints contains the Timeline node"),
        JsonArrayHasObjectWithStringField(Capture.Result, TEXT("entryPoints"),
            TEXT("nodeId"), F.Timeline->NodeGuid.ToString()));

    // The chain walks past the Timeline into the downstream PrintString.
    TestTrue(TEXT("executionChain reaches downstream PrintString"),
        JsonArrayHasObjectWithStringField(Capture.Result, TEXT("executionChain"),
            TEXT("nodeId"), F.DownstreamPrint->NodeGuid.ToString()));
    return true;
}

// ============================================================================
// get_execution_flow — entryPointsOnly and includeAllEntryPoints both resolve
// the timeline root instead of erroring.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTimelineRootBulkModesTest,
    "PinWright.blueprint.graph.get_execution_flow.TimelineRootBulkModes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTimelineRootBulkModesTest::RunTest(const FString& Parameters)
{
    FTimelineRootFixture F = BuildTimelineRootFixture(*this);
    if (!F.BP || !F.Timeline || !F.DownstreamPrint)
    {
        return true;
    }

    auto RunMode = [&](const TCHAR* ModeField)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), F.BP->GetPathName());
        Payload->SetStringField(TEXT("graphName"), F.EventGraph->GetName());
        Payload->SetBoolField(ModeField, true);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("blueprint.graph.get_execution_flow"), Payload, Capture);
        TestTrue(*FString::Printf(TEXT("Handler found (%s)"), ModeField), bFound);
        TestTrue(*FString::Printf(TEXT("%s mode succeeds on timeline root"), ModeField),
            Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            TestTrue(*FString::Printf(TEXT("%s mode lists the Timeline entry point"), ModeField),
                JsonArrayHasObjectWithStringField(Capture.Result, TEXT("entryPoints"),
                    TEXT("nodeId"), F.Timeline->NodeGuid.ToString()));
        }
    };

    RunMode(TEXT("entryPointsOnly"));
    RunMode(TEXT("includeAllEntryPoints"));
    return true;
}

// ============================================================================
// find_orphaned_nodes — a Timeline remains live when another entry point exists.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTimelineRootOrphanSweepMixedGraphTest,
    "PinWright.blueprint.graph.find_orphaned_nodes.TimelineWithLiveEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTimelineRootOrphanSweepMixedGraphTest::RunTest(const FString& Parameters)
{
    FTimelineRootFixture F = BuildTimelineRootFixture(*this);
    if (!F.BP || !F.Timeline || !F.DownstreamPrint)
    {
        return true;
    }

    // Keep the auto-play-shaped Timeline in a graph that also has a conventional entry.
    // This is the case the get_execution_flow latent-root fallback cannot cover.
    UK2Node_Event* BeginPlayNode = BpirGraphTestHelpers::EnsureBeginPlayNode(F.EventGraph);
    UK2Node_CallFunction* LivePrint = BpirGraphTestHelpers::AddPrintStringNode(F.EventGraph);
    if (!BeginPlayNode || !LivePrint)
    {
        AddError(TEXT("Failed to create the live BeginPlay chain"));
        return true;
    }
    BpirGraphTestHelpers::WireExec(BeginPlayNode, LivePrint);

    // Counterfactual anchor: the sweep must still report a genuinely dead exec node.
    UK2Node_IfThenElse* DeadBranch =
        BpirGraphTestHelpers::AddNodeToGraph<UK2Node_IfThenElse>(F.EventGraph);
    if (!DeadBranch)
    {
        AddError(TEXT("Failed to create the dead Branch node"));
        return true;
    }

    const FString TimelineId = F.Timeline->NodeGuid.ToString();
    const FString DownstreamPrintId = F.DownstreamPrint->NodeGuid.ToString();
    const FString LivePrintId = LivePrint->NodeGuid.ToString();
    const FString DeadBranchId = DeadBranch->NodeGuid.ToString();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), F.BP->GetPathName());
    Payload->SetStringField(TEXT("graphName"), F.EventGraph->GetName());

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("blueprint.graph.find_orphaned_nodes"), Payload, Capture);
    TestTrue(TEXT("find_orphaned_nodes handler found"), bFound);
    TestTrue(TEXT("find_orphaned_nodes succeeded"), Capture.bSuccess);

    if (!Capture.Result.IsValid())
    {
        AddError(TEXT("No result JSON returned from find_orphaned_nodes"));
        return true;
    }

    const double OrphanedCount = Capture.Result->GetNumberField(TEXT("orphanedCount"));
    TestEqual(TEXT("only the planted dead Branch is reported"), OrphanedCount, 1.0);

    const TArray<TSharedPtr<FJsonValue>>* OrphanedNodes = nullptr;
    if (Capture.Result->TryGetArrayField(TEXT("orphanedNodes"), OrphanedNodes) && OrphanedNodes)
    {
        bool bReportedTimeline = false;
        bool bReportedDownstreamPrint = false;
        bool bReportedLivePrint = false;
        bool bReportedDeadBranch = false;
        for (const TSharedPtr<FJsonValue>& Value : *OrphanedNodes)
        {
            const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
            if (!Entry.IsValid())
            {
                continue;
            }

            const FString NodeId = Entry->GetStringField(TEXT("nodeId"));
            bReportedTimeline |= NodeId.Equals(TimelineId, ESearchCase::IgnoreCase);
            bReportedDownstreamPrint |= NodeId.Equals(DownstreamPrintId, ESearchCase::IgnoreCase);
            bReportedLivePrint |= NodeId.Equals(LivePrintId, ESearchCase::IgnoreCase);
            bReportedDeadBranch |= NodeId.Equals(DeadBranchId, ESearchCase::IgnoreCase);
        }

        TestFalse(TEXT("Timeline is not reported orphaned"), bReportedTimeline);
        TestFalse(TEXT("Timeline downstream PrintString is not reported orphaned"),
            bReportedDownstreamPrint);
        TestFalse(TEXT("BeginPlay downstream PrintString is not reported orphaned"),
            bReportedLivePrint);
        TestTrue(TEXT("the planted dead Branch is reported orphaned"), bReportedDeadBranch);
    }
    else
    {
        AddError(TEXT("Expected an orphanedNodes array"));
    }

    return true;
}
