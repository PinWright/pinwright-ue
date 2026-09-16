// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for composite-subgraph entry-point discovery.
// Verifies that K2Node_Event nodes placed inside a K2Node_Composite::BoundGraph
// are correctly treated as entry points by all four affected tools.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Dom/JsonObject.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_Composite.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Tunnel.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "Kismet/KismetSystemLibrary.h"
#include "UObject/Package.h"

#include "Decompiler/BpirDecompiler.h"

// ============================================================================
// Shared fixture
// ============================================================================

namespace
{
    // Blueprint structure created by BuildCompositeFixture():
    //
    //   EventGraph
    //   └─ UK2Node_Composite ("ActivationNode")
    //      └─ BoundGraph ("ActivationNode")
    //         ├─ Event Tick (UK2Node_Event)    ← entry point lives here
    //         ├─ InnerPrint (UK2Node_CallFunction PrintString)
    //         └─ OutputSourceNode (K2Node_Tunnel exit)
    //   OuterPrint (UK2Node_CallFunction PrintString) ← wired to composite output
    //
    // Pre-fix: OuterPrint and the composite appear as orphans in EventGraph
    // because the reachability BFS found no entry in EventGraph->Nodes.

    struct FCompositeFixture
    {
        UBlueprint* BP = nullptr;
        UEdGraph* EventGraph = nullptr;
        UK2Node_Composite* CompositeNode = nullptr;
        UEdGraph* BoundGraph = nullptr;
        UK2Node_Event* TickEvent = nullptr;
        UK2Node_CallFunction* InnerPrint = nullptr;
        UK2Node_CallFunction* OuterPrint = nullptr;
    };

    template<typename T>
    T* MakeNode(UEdGraph* Graph)
    {
        T* Node = NewObject<T>(Graph);
        Node->CreateNewGuid();
        Node->PostPlacedNewNode();
        Node->AllocateDefaultPins();
        Graph->AddNode(Node, /*bFromUI=*/true, /*bSelectNewNode=*/false);
        return Node;
    }

    void WireExecByName(UEdGraphNode* Src, const FName SrcPinName,
                        UEdGraphNode* Dst, const FName DstPinName)
    {
        UEdGraphPin* SrcPin = Src->FindPin(SrcPinName, EGPD_Output);
        UEdGraphPin* DstPin = Dst->FindPin(DstPinName, EGPD_Input);
        if (SrcPin && DstPin)
        {
            SrcPin->MakeLinkTo(DstPin);
        }
    }

    FCompositeFixture BuildCompositeFixture()
    {
        FCompositeFixture F;

        const FName Name = *FString::Printf(TEXT("TestCompositeBP_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::DigitsLower).Left(8));
        F.BP = FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(),
            GetTransientPackage(),
            Name,
            BPTYPE_Normal,
            UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass(),
            FName(TEXT("PinWrightTests")));

        if (!F.BP || F.BP->UbergraphPages.Num() == 0)
        {
            return F;
        }

        F.EventGraph = F.BP->UbergraphPages[0];

        // -- Create the composite node in EventGraph --
        F.CompositeNode = NewObject<UK2Node_Composite>(F.EventGraph);
        F.CompositeNode->CreateNewGuid();
        F.CompositeNode->PostPlacedNewNode();
        F.EventGraph->AddNode(F.CompositeNode, /*bFromUI=*/true, /*bSelectNewNode=*/false);

        // PostPlacedNewNode creates the bound graph and paired tunnel nodes. Keep those
        // pointers intact so traversal sees the same composite shape Unreal creates.
        F.BoundGraph = F.CompositeNode->BoundGraph;
        if (!F.BoundGraph || !F.CompositeNode->InputSinkNode || !F.CompositeNode->OutputSourceNode)
        {
            return F;
        }

        FBlueprintEditorUtils::RenameGraph(F.BoundGraph, TEXT("ActivationNode"));

        FEdGraphPinType ExecPinType;
        ExecPinType.PinCategory = UEdGraphSchema_K2::PC_Exec;
        F.CompositeNode->OutputSourceNode->CreateUserDefinedPin(
            UEdGraphSchema_K2::PN_Then,
            ExecPinType,
            EGPD_Input);

        // -- Add Event Tick inside BoundGraph --
        F.TickEvent = NewObject<UK2Node_Event>(F.BoundGraph);
        F.TickEvent->CreateNewGuid();
        F.TickEvent->EventReference.SetExternalMember(TEXT("ReceiveTick"), AActor::StaticClass());
        F.TickEvent->bOverrideFunction = true;
        F.TickEvent->PostPlacedNewNode();
        F.TickEvent->AllocateDefaultPins();
        F.BoundGraph->AddNode(F.TickEvent, /*bFromUI=*/true, /*bSelectNewNode=*/false);
        F.TickEvent->ReconstructNode();

        // -- Add inner PrintString inside BoundGraph --
        F.InnerPrint = NewObject<UK2Node_CallFunction>(F.BoundGraph);
        F.InnerPrint->CreateNewGuid();
        F.InnerPrint->FunctionReference.SetExternalMember(
            GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
            UKismetSystemLibrary::StaticClass());
        F.InnerPrint->PostPlacedNewNode();
        F.InnerPrint->AllocateDefaultPins();
        F.BoundGraph->AddNode(F.InnerPrint, /*bFromUI=*/true, /*bSelectNewNode=*/false);
        F.InnerPrint->ReconstructNode();

        // Wire Event Tick → InnerPrint inside BoundGraph
        WireExecByName(F.TickEvent, UEdGraphSchema_K2::PN_Then,
                       F.InnerPrint, UEdGraphSchema_K2::PN_Execute);

        // Wire InnerPrint → OutputSourceNode (exit tunnel) if output source node has an exec input
        if (F.CompositeNode->OutputSourceNode)
        {
            UEdGraphPin* InnerPrintThen = F.InnerPrint->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
            // OutputSourceNode is the exit tunnel; its input exec pin accepts flow from the body
            UEdGraphPin* ExitExec = nullptr;
            for (UEdGraphPin* Pin : F.CompositeNode->OutputSourceNode->Pins)
            {
                if (Pin && Pin->Direction == EGPD_Input
                    && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                {
                    ExitExec = Pin;
                    break;
                }
            }
            if (InnerPrintThen && ExitExec)
            {
                InnerPrintThen->MakeLinkTo(ExitExec);
            }
        }

        // -- Add outer PrintString in EventGraph, wired to composite output --
        F.OuterPrint = NewObject<UK2Node_CallFunction>(F.EventGraph);
        F.OuterPrint->CreateNewGuid();
        F.OuterPrint->FunctionReference.SetExternalMember(
            GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
            UKismetSystemLibrary::StaticClass());
        F.OuterPrint->PostPlacedNewNode();
        F.OuterPrint->AllocateDefaultPins();
        F.EventGraph->AddNode(F.OuterPrint, /*bFromUI=*/true, /*bSelectNewNode=*/false);
        F.OuterPrint->ReconstructNode();

        // Wire composite output exec → OuterPrint.  The composite's output exec pin mirrors
        // the OutputSourceNode's input pins; find the first exec output on the composite.
        UEdGraphPin* CompositeOutExec = nullptr;
        for (UEdGraphPin* Pin : F.CompositeNode->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Output
                && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                CompositeOutExec = Pin;
                break;
            }
        }
        UEdGraphPin* OuterPrintExec = F.OuterPrint->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
        if (CompositeOutExec && OuterPrintExec)
        {
            CompositeOutExec->MakeLinkTo(OuterPrintExec);
        }

        return F;
    }
} // namespace

// ============================================================================
// Test 1: Orphan handler — composite entry-point false-positive
// ============================================================================
// If the recursion in BuildExecReachabilitySet (or IsBlueprintEntryNode) is reverted,
// this assertion fails because the inner K2Node_Event is never added to the entry-point
// set, so the BFS starts only from the (nonexistent) outer event nodes and the composite
// plus outer PrintString land in the orphan list.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompositeEntryPointOrphanFalsePositiveTest,
    "PinWright.blueprint.graph.find_orphaned_nodes.CompositeEntryPointFalsePositive",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompositeEntryPointOrphanFalsePositiveTest::RunTest(const FString& Parameters)
{
    FCompositeFixture F = BuildCompositeFixture();
    if (!F.BP)
    {
        AddError(TEXT("Failed to build composite fixture"));
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), F.BP->GetPathName());
    Payload->SetBoolField(TEXT("includeDataOnly"), false);

    FTestResponseCapture Capture;
    bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.graph.find_orphaned_nodes"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    if (Capture.Result.IsValid())
    {
        const double OrphanedCount = Capture.Result->GetNumberField(TEXT("orphanedCount"));
        TestEqual(TEXT("orphanedCount == 0 — composite-tunneled events count as roots"), OrphanedCount, 0.0);

        // Double-check: composite node and outer PrintString must NOT appear
        const TArray<TSharedPtr<FJsonValue>>* OrphanedNodes = nullptr;
        if (Capture.Result->TryGetArrayField(TEXT("orphanedNodes"), OrphanedNodes) && OrphanedNodes)
        {
            for (const TSharedPtr<FJsonValue>& Val : *OrphanedNodes)
            {
                TSharedPtr<FJsonObject> Obj = Val->AsObject();
                if (Obj.IsValid())
                {
                    TestFalse(TEXT("Composite node must not appear as orphan"),
                        Obj->GetStringField(TEXT("nodeType")).Contains(TEXT("Composite")));
                }
            }
        }
    }
    else
    {
        AddError(TEXT("No result JSON returned"));
    }
    return true;
}

// ============================================================================
// Test 2: Decompiler — composite entry-point listed
// ============================================================================
// If BpirDecompiler is reverted (GraphWalker::FindEntryPoints no longer descends into
// child graphs), this assertion fails because no inner K2Node_Event is ever passed to
// EmitEntrySignature and the BPIR output omits the normalized Tick signature.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompositeEntryPointDecompilerListsTest,
    "PinWright.bpir.decompiler.CompositeEntryPointListed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompositeEntryPointDecompilerListsTest::RunTest(const FString& Parameters)
{
    FCompositeFixture F = BuildCompositeFixture();
    if (!F.BP)
    {
        AddError(TEXT("Failed to build composite fixture"));
        return false;
    }

    FBpirDecompiler Decompiler(F.BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    TestTrue(TEXT("Decompiler reports tunneled Event Tick"),
        Result.BpirText.Contains(TEXT("entry event Tick")));
    return true;
}

// ============================================================================
// Test 3: get_execution_flow — composite entry listed in entryPoints
// ============================================================================
// If the inline IsEntryNode lambda in get_execution_flow is reverted to top-level-only,
// this assertion fails because EntryNodes (built from TargetGraph->Nodes) is empty for
// an EventGraph that contains only a composite node and no direct K2Node_Event.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompositeEntryPointExecutionFlowTest,
    "PinWright.blueprint.graph.get_execution_flow.CompositeEntryListed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompositeEntryPointExecutionFlowTest::RunTest(const FString& Parameters)
{
    FCompositeFixture F = BuildCompositeFixture();
    if (!F.BP)
    {
        AddError(TEXT("Failed to build composite fixture"));
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), F.BP->GetPathName());
    Payload->SetStringField(TEXT("graphName"), F.EventGraph->GetName());
    Payload->SetBoolField(TEXT("entryPointsOnly"), true);
    Payload->SetBoolField(TEXT("includeAllEntryPoints"), true);

    FTestResponseCapture Capture;
    bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.graph.get_execution_flow"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    if (Capture.Result.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* EntryPoints = nullptr;
        bool bFoundTickEntry = false;
        if (Capture.Result->TryGetArrayField(TEXT("entryPoints"), EntryPoints) && EntryPoints)
        {
            for (const TSharedPtr<FJsonValue>& Val : *EntryPoints)
            {
                TSharedPtr<FJsonObject> Obj = Val->AsObject();
                if (!Obj.IsValid()) continue;

                const FString NodeType = Obj->GetStringField(TEXT("nodeType"));
                const FString NodeTitle = Obj->GetStringField(TEXT("nodeTitle"));
                if (NodeType.Contains(TEXT("K2Node_Event")) &&
                    (NodeTitle.Contains(TEXT("Tick")) || NodeTitle.Contains(TEXT("ReceiveTick"))))
                {
                    bFoundTickEntry = true;
                    break;
                }
            }
        }
        TestTrue(TEXT("entryPoints contains tunneled Event Tick from composite subgraph"), bFoundTickEntry);
    }
    else
    {
        AddError(TEXT("No result JSON returned"));
    }
    return true;
}

// ============================================================================
// Test 5: get_execution_flow — WorkQueue bridge crosses composite boundary
// ============================================================================
// Counterfactual: if the WorkQueue bridge in BlueprintGraphHandler.cpp::BuildExecutionChain
// is reverted, the chain assertion below fails because BFS terminates at the composite
// node boundary and never enqueues OuterPrint (the outer-graph PrintString wired to the
// composite's output exec pin); if IsBlueprintEntryNode is reverted so composite entry
// points are no longer discovered, the entry-list assertion in
// FCompositeEntryPointExecutionFlowTest fails first because EntryNodes is empty.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompositeEntryPointExecutionChainBridgeTest,
    "PinWright.blueprint.graph.get_execution_flow.CompositeChainBridge",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompositeEntryPointExecutionChainBridgeTest::RunTest(const FString& Parameters)
{
    FCompositeFixture F = BuildCompositeFixture();
    if (!F.BP)
    {
        AddError(TEXT("Failed to build composite fixture"));
        return false;
    }

    // Walk the full execution chain (entryPointsOnly=false) starting from the inner
    // Event Tick.  The bridge must hop: EventTick -> InnerPrint -> ExitTunnel ->
    // (outer) CompositeNode -> OuterPrint.  OuterPrint lives in EventGraph, not in
    // BoundGraph, so it is only reachable if the WorkQueue bridge is in place.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), F.BP->GetPathName());
    Payload->SetStringField(TEXT("graphName"), F.EventGraph->GetName());
    // entryPointsOnly defaults to false — omit it to exercise the chain walk path
    Payload->SetBoolField(TEXT("includeAllEntryPoints"), true);

    FTestResponseCapture Capture;
    bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.graph.get_execution_flow"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    if (!Capture.Result.IsValid())
    {
        AddError(TEXT("No result JSON returned"));
        return true;
    }

    // Locate the outer PrintString's node GUID so we can look for it in the chain
    const FString OuterPrintGuid = F.OuterPrint ? F.OuterPrint->NodeGuid.ToString() : FString();
    TestFalse(TEXT("OuterPrint node must exist in fixture"), OuterPrintGuid.IsEmpty());

    // The chain for the inner Tick entry should contain the outer PrintString node.
    // It surfaces either in executionChain (default start) or in allExecutionChains.
    bool bOuterPrintReached = false;

    auto ScanChainForOuterPrint = [&](const TArray<TSharedPtr<FJsonValue>>* ChainArr)
    {
        if (!ChainArr) return;
        for (const TSharedPtr<FJsonValue>& Val : *ChainArr)
        {
            TSharedPtr<FJsonObject> Obj = Val->AsObject();
            if (!Obj.IsValid()) continue;
            FString NodeId;
            Obj->TryGetStringField(TEXT("nodeId"), NodeId);
            if (NodeId == OuterPrintGuid)
            {
                bOuterPrintReached = true;
            }
        }
    };

    // Check the primary executionChain
    {
        const TArray<TSharedPtr<FJsonValue>>* Chain = nullptr;
        Capture.Result->TryGetArrayField(TEXT("executionChain"), Chain);
        ScanChainForOuterPrint(Chain);
    }

    // Also scan allExecutionChains in case a different start node was chosen
    if (!bOuterPrintReached)
    {
        const TArray<TSharedPtr<FJsonValue>>* AllChains = nullptr;
        if (Capture.Result->TryGetArrayField(TEXT("allExecutionChains"), AllChains) && AllChains)
        {
            for (const TSharedPtr<FJsonValue>& ChainVal : *AllChains)
            {
                TSharedPtr<FJsonObject> ChainObj = ChainVal->AsObject();
                if (!ChainObj.IsValid()) continue;
                const TArray<TSharedPtr<FJsonValue>>* Chain = nullptr;
                ChainObj->TryGetArrayField(TEXT("executionChain"), Chain);
                ScanChainForOuterPrint(Chain);
            }
        }
    }

    TestTrue(
        TEXT("executionChain reaches OuterPrint (outer-graph node) via WorkQueue bridge"),
        bOuterPrintReached);
    return true;
}

// ============================================================================
// Test 4: list_graphs — composite subgraph surfaced
// ============================================================================
// If list_graphs is reverted to only iterating the four top-level lists, this assertion
// fails because EventGraph->SubGraphs (which holds ActivationNode) is never visited.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompositeEntryPointListGraphsTest,
    "PinWright.blueprint.graph.list_graphs.CompositeSubgraphSurfaced",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompositeEntryPointListGraphsTest::RunTest(const FString& Parameters)
{
    FCompositeFixture F = BuildCompositeFixture();
    if (!F.BP)
    {
        AddError(TEXT("Failed to build composite fixture"));
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), F.BP->GetPathName());

    FTestResponseCapture Capture;
    bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.graph.list_graphs"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    if (Capture.Result.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
        bool bFoundInner = false;
        if (Capture.Result->TryGetArrayField(TEXT("graphs"), Graphs) && Graphs)
        {
            for (const TSharedPtr<FJsonValue>& Val : *Graphs)
            {
                TSharedPtr<FJsonObject> Obj = Val->AsObject();
                if (!Obj.IsValid()) continue;

                FString GraphName;
                Obj->TryGetStringField(TEXT("name"), GraphName);
                if (GraphName == TEXT("ActivationNode"))
                {
                    bFoundInner = true;
                    break;
                }
            }
        }
        TestTrue(TEXT("list_graphs surfaces composite subgraph 'ActivationNode'"), bFoundInner);
    }
    else
    {
        AddError(TEXT("No result JSON returned"));
    }
    return true;
}
