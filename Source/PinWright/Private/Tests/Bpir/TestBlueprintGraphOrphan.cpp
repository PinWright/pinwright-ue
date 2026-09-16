// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for blueprint.graph.find_orphaned_nodes (read) and
// blueprint.graph.delete_orphaned_nodes (mutate) RPC handlers.
#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Dom/JsonObject.h"
#include "BpirGraphTestHelpers.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "K2Node_Knot.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_MathExpression.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "Kismet/KismetSystemLibrary.h"
#include "UObject/Package.h"

namespace
{
    // Create a PrintString call node wired to the given source node.
    UK2Node_CallFunction* AddWiredPrintString(UEdGraph* EventGraph, UEdGraphNode* Src)
    {
        UK2Node_CallFunction* PrintNode = BpirGraphTestHelpers::AddPrintStringNode(EventGraph);
        BpirGraphTestHelpers::WireExec(Src, PrintNode);
        return PrintNode;
    }

    UEdGraphPin* FindFirstDataPin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
    {
        if (!Node)
        {
            return nullptr;
        }

        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin) continue;
            if (Pin->Direction != Direction) continue;
            if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
            return Pin;
        }

        return nullptr;
    }

    void WireData(UEdGraphPin* Src, UEdGraphPin* Dst)
    {
        if (Src && Dst)
        {
            Src->MakeLinkTo(Dst);
        }
    }

    UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FGuid& NodeGuid)
    {
        if (!Graph)
        {
            return nullptr;
        }

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && Node->NodeGuid == NodeGuid)
            {
                return Node;
            }
        }

        return nullptr;
    }
} // namespace

// ============================================================================
// Test 1: HandlerRegistered
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrphanDetectionHandlerRegisteredTest,
    "PinWright.bpir.graph_orphan.HandlerRegistered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrphanDetectionHandlerRegisteredTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("handler registered"),
        IsHandlerRegistered(TEXT("blueprint.graph.find_orphaned_nodes")));
    return true;
}

// ============================================================================
// Test 2: BasicOrphan
// BeginPlay -> PrintString (reachable) + disconnected IfThenElse (orphan)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrphanDetectionBasicOrphanTest,
    "PinWright.blueprint.graph.find_orphaned_nodes.BasicOrphan",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrphanDetectionBasicOrphanTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph")); return false; }

    UK2Node_Event* BeginPlayNode = BpirGraphTestHelpers::EnsureBeginPlayNode(EventGraph);
    AddWiredPrintString(EventGraph, BeginPlayNode);

    // Add an orphaned branch node — no incoming exec connection
    UK2Node_IfThenElse* OrphanedBranch = BpirGraphTestHelpers::AddNodeToGraph<UK2Node_IfThenElse>(EventGraph);
    (void)OrphanedBranch;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());

    FTestResponseCapture Capture;
    bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.graph.find_orphaned_nodes"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler responded"), Capture.bWasCalled);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    if (Capture.Result.IsValid())
    {
        double OrphanedCount = Capture.Result->GetNumberField(TEXT("orphanedCount"));
        TestEqual(TEXT("orphanedCount == 1"), OrphanedCount, 1.0);

        const TArray<TSharedPtr<FJsonValue>>* OrphanedNodes = nullptr;
        if (Capture.Result->TryGetArrayField(TEXT("orphanedNodes"), OrphanedNodes) && OrphanedNodes && OrphanedNodes->Num() > 0)
        {
            TSharedPtr<FJsonObject> FirstOrphan = (*OrphanedNodes)[0]->AsObject();
            if (FirstOrphan.IsValid())
            {
                FString NodeType = FirstOrphan->GetStringField(TEXT("nodeType"));
                TestTrue(TEXT("orphanedNodes[0].nodeType contains IfThenElse"),
                    NodeType.Contains(TEXT("IfThenElse")));
            }
        }
        else
        {
            AddError(TEXT("Expected orphanedNodes array with at least 1 entry"));
        }
    }
    else
    {
        AddError(TEXT("No result JSON returned"));
    }
    return true;
}

// ============================================================================
// Test 3: NoOrphans
// BeginPlay -> PrintString (all connected, no orphans)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrphanDetectionNoOrphansTest,
    "PinWright.blueprint.graph.find_orphaned_nodes.NoOrphans",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrphanDetectionNoOrphansTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph")); return false; }

    UK2Node_Event* BeginPlayNode = BpirGraphTestHelpers::EnsureBeginPlayNode(EventGraph);
    AddWiredPrintString(EventGraph, BeginPlayNode);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());

    FTestResponseCapture Capture;
    bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.graph.find_orphaned_nodes"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    if (Capture.Result.IsValid())
    {
        double OrphanedCount = Capture.Result->GetNumberField(TEXT("orphanedCount"));
        TestEqual(TEXT("orphanedCount == 0"), OrphanedCount, 0.0);
    }
    else
    {
        AddError(TEXT("No result JSON returned"));
    }
    return true;
}

// ============================================================================
// Test 4: CommentNodeNotFlagged
// Comment node (decorative) should not be flagged; only IfThenElse is orphan
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrphanDetectionCommentNodeNotFlaggedTest,
    "PinWright.blueprint.graph.find_orphaned_nodes.CommentNodeNotFlagged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrphanDetectionCommentNodeNotFlaggedTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph")); return false; }

    UK2Node_Event* BeginPlayNode = BpirGraphTestHelpers::EnsureBeginPlayNode(EventGraph);
    AddWiredPrintString(EventGraph, BeginPlayNode);

    UEdGraphNode_Comment* CommentNode = BpirGraphTestHelpers::AddNodeToGraph<UEdGraphNode_Comment>(EventGraph);
    (void)CommentNode;

    // Add an orphaned IfThenElse for contrast
    UK2Node_IfThenElse* OrphanedBranch = BpirGraphTestHelpers::AddNodeToGraph<UK2Node_IfThenElse>(EventGraph);
    (void)OrphanedBranch;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());

    FTestResponseCapture Capture;
    bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.graph.find_orphaned_nodes"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    if (Capture.Result.IsValid())
    {
        double OrphanedCount = Capture.Result->GetNumberField(TEXT("orphanedCount"));
        TestEqual(TEXT("orphanedCount == 1 (only IfThenElse)"), OrphanedCount, 1.0);

        // Verify no orphan has nodeType containing "Comment"
        const TArray<TSharedPtr<FJsonValue>>* OrphanedNodes = nullptr;
        if (Capture.Result->TryGetArrayField(TEXT("orphanedNodes"), OrphanedNodes) && OrphanedNodes)
        {
            for (const TSharedPtr<FJsonValue>& Val : *OrphanedNodes)
            {
                TSharedPtr<FJsonObject> Obj = Val->AsObject();
                if (Obj.IsValid())
                {
                    FString NodeType = Obj->GetStringField(TEXT("nodeType"));
                    TestFalse(TEXT("No orphan is a Comment node"),
                        NodeType.Contains(TEXT("Comment")));
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
// Test 5: PureNodeIncludedByDefault
// Disconnected VariableGet (pure, no exec pins) should be flagged by default
// because includeDataOnly now defaults to true.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrphanDetectionPureNodeIncludedByDefaultTest,
    "PinWright.blueprint.graph.find_orphaned_nodes.PureNodeIncludedByDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrphanDetectionPureNodeIncludedByDefaultTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph")); return false; }

    // Add a member variable so the VariableGet node has something to reference
    FEdGraphPinType PinType;
    PinType.PinCategory = UEdGraphSchema_K2::PC_Float;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("TestVar"), PinType);

    // Add a disconnected VariableGet node (pure — no exec pins)
    UK2Node_VariableGet* VarGetNode = BpirGraphTestHelpers::AddNodeToGraph<UK2Node_VariableGet>(EventGraph);
    VarGetNode->VariableReference.SetSelfMember(FName(TEXT("TestVar")));
    VarGetNode->ReconstructNode();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());
    // includeDataOnly defaults to true — pure nodes should be flagged

    FTestResponseCapture Capture;
    bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.graph.find_orphaned_nodes"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    if (Capture.Result.IsValid())
    {
        double OrphanedCount = Capture.Result->GetNumberField(TEXT("orphanedCount"));
        TestTrue(TEXT("orphanedCount >= 1 (pure nodes included by default)"), OrphanedCount >= 1.0);
    }
    else
    {
        AddError(TEXT("No result JSON returned"));
    }
    return true;
}

// ============================================================================
// Test 5b: PureNodeExcludedWhenOptOut
// Disconnected VariableGet should NOT be flagged when includeDataOnly=false
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrphanDetectionPureNodeExcludedWhenOptOutTest,
    "PinWright.blueprint.graph.find_orphaned_nodes.PureNodeExcludedWhenOptOut",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrphanDetectionPureNodeExcludedWhenOptOutTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph")); return false; }

    // Add a member variable so the VariableGet node has something to reference
    FEdGraphPinType PinType;
    PinType.PinCategory = UEdGraphSchema_K2::PC_Float;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("TestVar"), PinType);

    // Add a disconnected VariableGet node (pure — no exec pins)
    UK2Node_VariableGet* VarGetNode = BpirGraphTestHelpers::AddNodeToGraph<UK2Node_VariableGet>(EventGraph);
    VarGetNode->VariableReference.SetSelfMember(FName(TEXT("TestVar")));
    VarGetNode->ReconstructNode();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());
    Payload->SetBoolField(TEXT("includeDataOnly"), false);

    FTestResponseCapture Capture;
    bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.graph.find_orphaned_nodes"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    if (Capture.Result.IsValid())
    {
        double OrphanedCount = Capture.Result->GetNumberField(TEXT("orphanedCount"));
        TestEqual(TEXT("orphanedCount == 0 (pure nodes excluded when opted out)"), OrphanedCount, 0.0);
    }
    else
    {
        AddError(TEXT("No result JSON returned"));
    }
    return true;
}

// ============================================================================
// Test 6: PureNodeIncludedWhenOptIn
// Disconnected VariableGet should be flagged when includeDataOnly=true
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrphanDetectionPureNodeIncludedWhenOptInTest,
    "PinWright.blueprint.graph.find_orphaned_nodes.PureNodeIncludedWhenOptIn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrphanDetectionPureNodeIncludedWhenOptInTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph")); return false; }

    // Add a member variable
    FEdGraphPinType PinType;
    PinType.PinCategory = UEdGraphSchema_K2::PC_Float;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("TestVar"), PinType);

    // Add a disconnected VariableGet node (pure)
    UK2Node_VariableGet* VarGetNode = BpirGraphTestHelpers::AddNodeToGraph<UK2Node_VariableGet>(EventGraph);
    VarGetNode->VariableReference.SetSelfMember(FName(TEXT("TestVar")));
    VarGetNode->ReconstructNode();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());
    Payload->SetBoolField(TEXT("includeDataOnly"), true);

    FTestResponseCapture Capture;
    bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.graph.find_orphaned_nodes"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    if (Capture.Result.IsValid())
    {
        double OrphanedCount = Capture.Result->GetNumberField(TEXT("orphanedCount"));
        TestTrue(TEXT("orphanedCount >= 1"), OrphanedCount >= 1.0);

        // Verify disconnected VariableGet appears in the orphaned nodes
        const TArray<TSharedPtr<FJsonValue>>* OrphanedNodes = nullptr;
        bool bFoundVarGet = false;
        if (Capture.Result->TryGetArrayField(TEXT("orphanedNodes"), OrphanedNodes) && OrphanedNodes)
        {
            for (const TSharedPtr<FJsonValue>& Val : *OrphanedNodes)
            {
                TSharedPtr<FJsonObject> Obj = Val->AsObject();
                if (Obj.IsValid())
                {
                    FString NodeType = Obj->GetStringField(TEXT("nodeType"));
                    if (NodeType.Contains(TEXT("VariableGet")))
                    {
                        bFoundVarGet = true;
                        break;
                    }
                }
            }
        }
        TestTrue(TEXT("Disconnected VariableGet appears in orphanedNodes"), bFoundVarGet);
    }
    else
    {
        AddError(TEXT("No result JSON returned"));
    }
    return true;
}

// ============================================================================
// Test 7: MixedOrphanChainIncludedWhenOptIn
// Orphaned PrintString fed by VariableGet -> Knot should only fully clean up
// when includeDataOnly=true.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrphanDetectionMixedOrphanChainIncludedWhenOptInTest,
    "PinWright.blueprint.graph.find_orphaned_nodes.MixedOrphanChainIncludedWhenOptIn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrphanDetectionMixedOrphanChainIncludedWhenOptInTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph")); return false; }

    UK2Node_Event* BeginPlayNode = BpirGraphTestHelpers::EnsureBeginPlayNode(EventGraph);
    AddWiredPrintString(EventGraph, BeginPlayNode);

    FEdGraphPinType PinType;
    PinType.PinCategory = UEdGraphSchema_K2::PC_String;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("TestMessage"), PinType);

    UK2Node_CallFunction* OrphanedPrint = BpirGraphTestHelpers::AddPrintStringNode(EventGraph);
    UK2Node_VariableGet* VarGetNode = BpirGraphTestHelpers::AddNodeToGraph<UK2Node_VariableGet>(EventGraph);
    VarGetNode->VariableReference.SetSelfMember(FName(TEXT("TestMessage")));
    VarGetNode->ReconstructNode();

    UK2Node_Knot* KnotNode = BpirGraphTestHelpers::AddNodeToGraph<UK2Node_Knot>(EventGraph);
    KnotNode->ReconstructNode();

    WireData(
        FindFirstDataPin(VarGetNode, EGPD_Output),
        FindFirstDataPin(KnotNode, EGPD_Input));
    WireData(
        FindFirstDataPin(KnotNode, EGPD_Output),
        OrphanedPrint->FindPin(TEXT("InString"), EGPD_Input));

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());
        Payload->SetBoolField(TEXT("includeDataOnly"), false);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.graph.find_orphaned_nodes"), Payload, Capture);
        TestTrue(TEXT("Exec-only handler found"), bFound);
        TestTrue(TEXT("Exec-only handler succeeded"), Capture.bSuccess);

        if (Capture.Result.IsValid())
        {
            const double OrphanedCount = Capture.Result->GetNumberField(TEXT("orphanedCount"));
            TestEqual(TEXT("Exec-only mode finds only the impure orphan"), OrphanedCount, 1.0);

            const TArray<TSharedPtr<FJsonValue>>* OrphanedNodes = nullptr;
            bool bFoundVariableGet = false;
            bool bFoundKnot = false;
            if (Capture.Result->TryGetArrayField(TEXT("orphanedNodes"), OrphanedNodes) && OrphanedNodes)
            {
                for (const TSharedPtr<FJsonValue>& Val : *OrphanedNodes)
                {
                    const TSharedPtr<FJsonObject> Obj = Val->AsObject();
                    if (!Obj.IsValid()) continue;

                    const FString NodeType = Obj->GetStringField(TEXT("nodeType"));
                    bFoundVariableGet |= NodeType.Contains(TEXT("VariableGet"));
                    bFoundKnot |= NodeType.Contains(TEXT("Knot"));
                }
            }

            TestFalse(TEXT("Exec-only mode excludes orphaned data source"), bFoundVariableGet);
            TestFalse(TEXT("Exec-only mode excludes orphaned knot"), bFoundKnot);
        }
    }

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());
        Payload->SetBoolField(TEXT("includeDataOnly"), true);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.graph.find_orphaned_nodes"), Payload, Capture);
        TestTrue(TEXT("Transitive handler found"), bFound);
        TestTrue(TEXT("Transitive handler succeeded"), Capture.bSuccess);

        if (Capture.Result.IsValid())
        {
            const double OrphanedCount = Capture.Result->GetNumberField(TEXT("orphanedCount"));
            TestEqual(TEXT("Transitive mode finds impure node plus data chain"), OrphanedCount, 3.0);

            const TArray<TSharedPtr<FJsonValue>>* OrphanedNodes = nullptr;
            bool bFoundVariableGet = false;
            bool bFoundKnot = false;
            bool bFoundExecNode = false;
            if (Capture.Result->TryGetArrayField(TEXT("orphanedNodes"), OrphanedNodes) && OrphanedNodes)
            {
                for (const TSharedPtr<FJsonValue>& Val : *OrphanedNodes)
                {
                    const TSharedPtr<FJsonObject> Obj = Val->AsObject();
                    if (!Obj.IsValid()) continue;

                    const FString NodeType = Obj->GetStringField(TEXT("nodeType"));
                    bFoundVariableGet |= NodeType.Contains(TEXT("VariableGet"));
                    bFoundKnot |= NodeType.Contains(TEXT("Knot"));
                    bFoundExecNode |= Obj->GetBoolField(TEXT("hasExecPins"));
                }
            }

            TestTrue(TEXT("Transitive mode includes orphaned data source"), bFoundVariableGet);
            TestTrue(TEXT("Transitive mode includes orphaned knot"), bFoundKnot);
            TestTrue(TEXT("Transitive mode still includes the impure orphan"), bFoundExecNode);
        }
    }
    return true;
}

// ============================================================================
// Test 8: DeleteMixedOrphanChain
// delete_orphaned_nodes + includeDataOnly=true deletes the full impure/pure orphan chain.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrphanDetectionDeleteMixedOrphanChainTest,
    "PinWright.blueprint.graph.find_orphaned_nodes.DeleteMixedOrphanChain",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrphanDetectionDeleteMixedOrphanChainTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph")); return false; }

    UK2Node_Event* BeginPlayNode = BpirGraphTestHelpers::EnsureBeginPlayNode(EventGraph);
    AddWiredPrintString(EventGraph, BeginPlayNode);

    FEdGraphPinType PinType;
    PinType.PinCategory = UEdGraphSchema_K2::PC_String;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("TestMessage"), PinType);

    UK2Node_CallFunction* OrphanedPrint = BpirGraphTestHelpers::AddPrintStringNode(EventGraph);
    UK2Node_VariableGet* VarGetNode = BpirGraphTestHelpers::AddNodeToGraph<UK2Node_VariableGet>(EventGraph);
    VarGetNode->VariableReference.SetSelfMember(FName(TEXT("TestMessage")));
    VarGetNode->ReconstructNode();

    UK2Node_Knot* KnotNode = BpirGraphTestHelpers::AddNodeToGraph<UK2Node_Knot>(EventGraph);
    KnotNode->ReconstructNode();

    WireData(
        FindFirstDataPin(VarGetNode, EGPD_Output),
        FindFirstDataPin(KnotNode, EGPD_Input));
    WireData(
        FindFirstDataPin(KnotNode, EGPD_Output),
        OrphanedPrint->FindPin(TEXT("InString"), EGPD_Input));

    const int32 NodeCountBefore = EventGraph->Nodes.Num();

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());
        Payload->SetBoolField(TEXT("includeDataOnly"), true);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.graph.delete_orphaned_nodes"), Payload, Capture);
        TestTrue(TEXT("Delete handler found"), bFound);
        TestTrue(TEXT("Delete handler succeeded"), Capture.bSuccess);

        if (Capture.Result.IsValid())
        {
            const double DeletedCount = Capture.Result->GetNumberField(TEXT("deletedCount"));
            TestEqual(TEXT("Delete removes the full orphan chain"), DeletedCount, 3.0);
        }
    }

    TestEqual(TEXT("Node count decreased by the full chain"), EventGraph->Nodes.Num(), NodeCountBefore - 3);

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());
        Payload->SetBoolField(TEXT("includeDataOnly"), true);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("blueprint.graph.find_orphaned_nodes"), Payload, Capture);
        TestTrue(TEXT("Post-delete find succeeded"), Capture.bSuccess);

        if (Capture.Result.IsValid())
        {
            const double OrphanedCount = Capture.Result->GetNumberField(TEXT("orphanedCount"));
            TestEqual(TEXT("No orphan chain remains after deletion"), OrphanedCount, 0.0);
        }
    }
    return true;
}

// ============================================================================
// Test: blueprint.graph.delete_node cleans only the newly orphaned delta.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeleteNodeCleansNewOrphanDeltaTest,
    "PinWright.blueprint.graph.delete_node.CleansNewOrphanDelta",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDeleteNodeCleansNewOrphanDeltaTest::RunTest(const FString& Parameters)
{
    // Counterfactual: without the delete_node cleanup hook, deletedNewOrphanCount is
    // zero or absent and the newly disconnected VariableGet remains. If the hook
    // deletes all orphans instead of only the delta, the pre-existing orphan is lost.

    UBlueprint* BP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph")); return false; }

    FEdGraphPinType PinType;
    PinType.PinCategory = UEdGraphSchema_K2::PC_String;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("DeltaMessage"), PinType);

    UK2Node_Event* BeginPlayNode = BpirGraphTestHelpers::EnsureBeginPlayNode(EventGraph);
    UK2Node_CallFunction* PrintNode = AddWiredPrintString(EventGraph, BeginPlayNode);

    UK2Node_VariableGet* NewlyOrphanedGet = BpirGraphTestHelpers::AddNodeToGraph<UK2Node_VariableGet>(EventGraph);
    NewlyOrphanedGet->VariableReference.SetSelfMember(FName(TEXT("DeltaMessage")));
    NewlyOrphanedGet->ReconstructNode();
    WireData(
        FindFirstDataPin(NewlyOrphanedGet, EGPD_Output),
        PrintNode->FindPin(TEXT("InString"), EGPD_Input));

    UK2Node_VariableGet* PreExistingOrphanGet = BpirGraphTestHelpers::AddNodeToGraph<UK2Node_VariableGet>(EventGraph);
    PreExistingOrphanGet->VariableReference.SetSelfMember(FName(TEXT("DeltaMessage")));
    PreExistingOrphanGet->ReconstructNode();

    const FGuid NewlyOrphanedGuid = NewlyOrphanedGet->NodeGuid;
    const FGuid PreExistingOrphanGuid = PreExistingOrphanGet->NodeGuid;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());
    Payload->SetStringField(TEXT("nodeId"), PrintNode->NodeGuid.ToString());

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.graph.delete_node"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    if (Capture.Result.IsValid())
    {
        double DeletedNewOrphanCount = 0.0;
        const bool bHasDeletedCount =
            Capture.Result->TryGetNumberField(TEXT("deletedNewOrphanCount"), DeletedNewOrphanCount);
        TestTrue(TEXT("deletedNewOrphanCount is reported"), bHasDeletedCount);
        TestEqual(TEXT("Exactly one newly orphaned node is deleted"), DeletedNewOrphanCount, 1.0);

        const TArray<TSharedPtr<FJsonValue>>* DeletedNewOrphans = nullptr;
        bool bReportedNewlyOrphanedGet = false;
        if (Capture.Result->TryGetArrayField(TEXT("deletedNewOrphans"), DeletedNewOrphans) && DeletedNewOrphans)
        {
            for (const TSharedPtr<FJsonValue>& Val : *DeletedNewOrphans)
            {
                const TSharedPtr<FJsonObject> Obj = Val->AsObject();
                if (!Obj.IsValid()) continue;

                FString NodeId;
                FString NodeType;
                Obj->TryGetStringField(TEXT("nodeId"), NodeId);
                Obj->TryGetStringField(TEXT("nodeType"), NodeType);
                bReportedNewlyOrphanedGet |=
                    NodeId == NewlyOrphanedGuid.ToString() && NodeType.Contains(TEXT("VariableGet"));
            }
        }
        TestTrue(TEXT("Deleted newly orphaned VariableGet is reported"), bReportedNewlyOrphanedGet);
    }
    else
    {
        AddError(TEXT("No result JSON returned"));
    }

    TestTrue(TEXT("Newly orphaned VariableGet is removed"),
        FindNodeByGuid(EventGraph, NewlyOrphanedGuid) == nullptr);
    TestTrue(TEXT("Pre-existing orphan VariableGet remains"),
        FindNodeByGuid(EventGraph, PreExistingOrphanGuid) != nullptr);
    return true;
}

// ============================================================================
// Test 9: ScanAllGraphsIncludesDelegateSignatureGraphs
// Omitting graphName must include delegate signature graphs in the scan.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrphanDetectionScanAllGraphsIncludesDelegateSignatureGraphsTest,
    "PinWright.blueprint.graph.find_orphaned_nodes.ScanAllGraphsIncludesDelegateSignatureGraphs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrphanDetectionScanAllGraphsIncludesDelegateSignatureGraphsTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    // Create event dispatcher: member variable + signature graph (mirrors FBlueprintEditor::AddNewDelegate_OnClicked)
    FEdGraphPinType DelegateType;
    DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("OnGraphCoverage"), DelegateType);

    UEdGraph* NewDelegateGraph = FBlueprintEditorUtils::CreateNewGraph(
        BP, TEXT("OnGraphCoverage"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    if (!NewDelegateGraph)
    {
        AddError(TEXT("Failed to create delegate signature graph"));
        return false;
    }
    NewDelegateGraph->bEditable = false;
    const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
    K2Schema->CreateDefaultNodesForGraph(*NewDelegateGraph);
    K2Schema->CreateFunctionGraphTerminators(*NewDelegateGraph, static_cast<UClass*>(nullptr));
    K2Schema->AddExtraFunctionFlags(NewDelegateGraph, FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public);
    K2Schema->MarkFunctionEntryAsEditable(NewDelegateGraph, true);
    BP->DelegateSignatureGraphs.Add(NewDelegateGraph);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

    TestTrue(TEXT("Delegate signature graph was created"), BP->DelegateSignatureGraphs.Num() > 0);
    if (BP->DelegateSignatureGraphs.Num() == 0)
    {
        return false;
    }

    UEdGraph* DelegateGraph = BP->DelegateSignatureGraphs[0];
    UK2Node_IfThenElse* OrphanedBranch = BpirGraphTestHelpers::AddNodeToGraph<UK2Node_IfThenElse>(DelegateGraph);
    (void)OrphanedBranch;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.graph.find_orphaned_nodes"), Payload, Capture);
    TestTrue(TEXT("All-graphs handler found"), bFound);
    TestTrue(TEXT("All-graphs handler succeeded"), Capture.bSuccess);

    if (Capture.Result.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* GraphsScanned = nullptr;
        bool bScannedDelegateGraph = false;
        if (Capture.Result->TryGetArrayField(TEXT("graphsScanned"), GraphsScanned) && GraphsScanned)
        {
            for (const TSharedPtr<FJsonValue>& Val : *GraphsScanned)
            {
                bScannedDelegateGraph |= (Val->AsString() == DelegateGraph->GetName());
            }
        }
        TestTrue(TEXT("Delegate signature graph is included in graphsScanned"), bScannedDelegateGraph);

        const TArray<TSharedPtr<FJsonValue>>* OrphanedNodes = nullptr;
        bool bFoundDelegateGraphOrphan = false;
        if (Capture.Result->TryGetArrayField(TEXT("orphanedNodes"), OrphanedNodes) && OrphanedNodes)
        {
            for (const TSharedPtr<FJsonValue>& Val : *OrphanedNodes)
            {
                const TSharedPtr<FJsonObject> Obj = Val->AsObject();
                if (!Obj.IsValid()) continue;

                if (Obj->GetStringField(TEXT("graphName")) == DelegateGraph->GetName())
                {
                    bFoundDelegateGraphOrphan = true;
                    break;
                }
            }
        }
        TestTrue(TEXT("Orphan in delegate signature graph is reported"), bFoundDelegateGraphOrphan);
    }
    return true;
}

// ============================================================================
// Test 7: DeleteOrphans
// BeginPlay -> PrintString + orphaned IfThenElse. delete_orphaned_nodes removes it.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrphanDetectionDeleteOrphansTest,
    "PinWright.blueprint.graph.find_orphaned_nodes.DeleteOrphans",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrphanDetectionDeleteOrphansTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph")); return false; }

    UK2Node_Event* BeginPlayNode = BpirGraphTestHelpers::EnsureBeginPlayNode(EventGraph);
    AddWiredPrintString(EventGraph, BeginPlayNode);

    // Add an orphaned branch node
    UK2Node_IfThenElse* OrphanedBranch = BpirGraphTestHelpers::AddNodeToGraph<UK2Node_IfThenElse>(EventGraph);
    (void)OrphanedBranch;

    const int32 NodeCountBefore = EventGraph->Nodes.Num();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());

    FTestResponseCapture Capture;
    bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.graph.delete_orphaned_nodes"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    if (Capture.Result.IsValid())
    {
        double DeletedCount = Capture.Result->GetNumberField(TEXT("deletedCount"));
        TestEqual(TEXT("deletedCount == 1"), DeletedCount, 1.0);
    }
    else
    {
        AddError(TEXT("No result JSON returned"));
    }

    const int32 NodeCountAfter = EventGraph->Nodes.Num();
    TestEqual(TEXT("Node count decreased by 1"), NodeCountAfter, NodeCountBefore - 1);
    return true;
}

// ============================================================================
// Test: LiveExecKnotNotFlagged
// BeginPlay -> UK2Node_Knot(exec) -> PrintString: knot must not be flagged as orphan.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrphanDetectionLiveExecKnotNotFlaggedTest,
    "PinWright.blueprint.graph.find_orphaned_nodes.LiveExecKnotNotFlagged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrphanDetectionLiveExecKnotNotFlaggedTest::RunTest(const FString& Parameters)
{
    // Counterfactual: If BlueprintHandlerUtils::BuildExecReachabilitySet is reverted to a
    // non-knot-aware exec walk that stops at the knot, this assertion fails because the BFS
    // would not reach PrintString and would mark the knot as orphan.

    UBlueprint* BP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph")); return false; }

    UK2Node_Event* BeginPlayNode = BpirGraphTestHelpers::EnsureBeginPlayNode(EventGraph);

    // Wire: BeginPlay -> Knot -> PrintString
    UK2Node_Knot* KnotNode = BpirGraphTestHelpers::AddNodeToGraph<UK2Node_Knot>(EventGraph);
    KnotNode->ReconstructNode();

    BpirGraphTestHelpers::WireExec(BeginPlayNode, KnotNode);

    UK2Node_CallFunction* PrintNode = BpirGraphTestHelpers::AddPrintStringNode(EventGraph);
    BpirGraphTestHelpers::WireExec(KnotNode, PrintNode);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());
    Payload->SetBoolField(TEXT("includeDataOnly"), false);

    FTestResponseCapture Capture;
    bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.graph.find_orphaned_nodes"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    if (Capture.Result.IsValid())
    {
        double OrphanedCount = Capture.Result->GetNumberField(TEXT("orphanedCount"));
        TestEqual(TEXT("orphanedCount == 0 (live knot not flagged)"), OrphanedCount, 0.0);
    }
    else
    {
        AddError(TEXT("No result JSON returned"));
    }
    return true;
}

// ============================================================================
// Test 8: EntryNodeNotFlagged
// CustomEvent with no downstream connections should not be flagged as orphan
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrphanDetectionEntryNodeNotFlaggedTest,
    "PinWright.blueprint.graph.find_orphaned_nodes.EntryNodeNotFlagged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrphanDetectionEntryNodeNotFlaggedTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph")); return false; }

    // Add a CustomEvent with no downstream connections
    UK2Node_CustomEvent* CustomEventNode = BpirGraphTestHelpers::AddNodeToGraph<UK2Node_CustomEvent>(EventGraph);
    CustomEventNode->CustomFunctionName = TEXT("TestOrphanEvent");
    CustomEventNode->ReconstructNode();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());

    FTestResponseCapture Capture;
    bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.graph.find_orphaned_nodes"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    if (Capture.Result.IsValid())
    {
        double OrphanedCount = Capture.Result->GetNumberField(TEXT("orphanedCount"));
        TestEqual(TEXT("orphanedCount == 0 (entry nodes never orphaned)"), OrphanedCount, 0.0);

        // Extra check: verify CustomEvent is not in orphanedNodes
        const TArray<TSharedPtr<FJsonValue>>* OrphanedNodes = nullptr;
        if (Capture.Result->TryGetArrayField(TEXT("orphanedNodes"), OrphanedNodes) && OrphanedNodes)
        {
            for (const TSharedPtr<FJsonValue>& Val : *OrphanedNodes)
            {
                TSharedPtr<FJsonObject> Obj = Val->AsObject();
                if (Obj.IsValid())
                {
                    FString NodeType = Obj->GetStringField(TEXT("nodeType"));
                    TestFalse(TEXT("CustomEvent should not appear in orphanedNodes"),
                        NodeType.Contains(TEXT("CustomEvent")));
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
// Test: delete_orphaned_nodes deletes orphans and leaves none behind on re-find.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeleteOrphanedNodesHandlerTest,
    "PinWright.blueprint.graph.delete_orphaned_nodes.DeletesAndLeavesNoOrphans",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDeleteOrphanedNodesHandlerTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph")); return false; }

    UK2Node_Event* BeginPlayNode = BpirGraphTestHelpers::EnsureBeginPlayNode(EventGraph);
    AddWiredPrintString(EventGraph, BeginPlayNode);

    UK2Node_IfThenElse* OrphanedBranch = BpirGraphTestHelpers::AddNodeToGraph<UK2Node_IfThenElse>(EventGraph);
    (void)OrphanedBranch;

    // Step 1: delete the orphan via the new mutating RPC.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());
        Payload->SetBoolField(TEXT("includeDataOnly"), true);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.graph.delete_orphaned_nodes"), Payload, Capture);
        TestTrue(TEXT("delete_orphaned_nodes handler found"), bFound);
        TestTrue(TEXT("delete_orphaned_nodes handler succeeded"), Capture.bSuccess);

        if (Capture.Result.IsValid())
        {
            const double DeletedCount = Capture.Result->GetNumberField(TEXT("deletedCount"));
            TestEqual(TEXT("deletedCount == 1"), DeletedCount, 1.0);
        }
        else
        {
            AddError(TEXT("No result JSON returned from delete_orphaned_nodes"));
        }
    }

    // Step 2: confirm find_orphaned_nodes now reports zero orphans.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());
        Payload->SetBoolField(TEXT("includeDataOnly"), true);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.graph.find_orphaned_nodes"), Payload, Capture);
        TestTrue(TEXT("find_orphaned_nodes handler found"), bFound);
        TestTrue(TEXT("find_orphaned_nodes handler succeeded"), Capture.bSuccess);

        if (Capture.Result.IsValid())
        {
            const double OrphanedCount = Capture.Result->GetNumberField(TEXT("orphanedCount"));
            TestEqual(TEXT("orphanedCount == 0 after delete"), OrphanedCount, 0.0);
        }
        else
        {
            AddError(TEXT("No result JSON returned from find_orphaned_nodes"));
        }
    }
    return true;
}

// ============================================================================
// Test: the sweep never tears a K2Node_MathExpression's bound graph apart.
//
// A MathExpression's BoundGraph is the exact shape that used to be swept: an
// entry/exit UK2Node_Tunnel pair plus purely generated math nodes, with no exec
// edge reaching the exit. Deleting the exit tunnel makes UK2Node_Tunnel::DestroyNode()
// null the owning composite's OutputSourceNode, which compiles and saves clean and
// then asserts fatally the next time the package is loaded (regenerate-on-load calls
// UK2Node_MathExpression::ReconstructNode() -> ClearExpression() -> GetExitNode()).
// This asserts the surviving invariant directly instead of reloading the package,
// because reproducing the load is a hard editor crash, not a catchable failure.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeleteOrphanedNodesMathExpressionBoundaryTest,
    "PinWright.blueprint.graph.delete_orphaned_nodes.MathExpressionBoundaryStaysIntact",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDeleteOrphanedNodesMathExpressionBoundaryTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph")); return false; }

    UK2Node_Event* BeginPlayNode = BpirGraphTestHelpers::EnsureBeginPlayNode(EventGraph);
    AddWiredPrintString(EventGraph, BeginPlayNode);

    UK2Node_MathExpression* MathNode =
        BpirGraphTestHelpers::AddNodeToGraph<UK2Node_MathExpression>(EventGraph);
    if (!MathNode) { AddError(TEXT("Failed to add MathExpression node")); return false; }

    MathNode->Expression = TEXT("a + b");
    MathNode->ReconstructNode();

    if (!MathNode->BoundGraph || !MathNode->InputSinkNode || !MathNode->OutputSourceNode)
    {
        AddError(TEXT("MathExpression fixture did not produce a bound graph with both tunnels"));
        return false;
    }

    const FGuid MathNodeGuid = MathNode->NodeGuid;
    const int32 BoundNodesBefore = MathNode->BoundGraph->Nodes.Num();

    // A plain orphan the sweep is still expected to remove, so this cannot pass by the
    // sweep having quietly become a no-op.
    UK2Node_IfThenElse* OrphanedBranch = BpirGraphTestHelpers::AddNodeToGraph<UK2Node_IfThenElse>(EventGraph);
    const FGuid OrphanedBranchGuid = OrphanedBranch->NodeGuid;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());
    Payload->SetBoolField(TEXT("includeDataOnly"), true);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("blueprint.graph.delete_orphaned_nodes"), Payload, Capture);
    TestTrue(TEXT("delete_orphaned_nodes handler found"), bFound);
    // Should the sweep ever start tearing boundary tunnels out again, the pre-save
    // integrity gate turns this into a COMPOSITE_BOUNDARY_BROKEN error, so a red here
    // is the guard reporting rather than a package having been corrupted on disk.
    TestTrue(TEXT("delete_orphaned_nodes handler succeeded"), Capture.bSuccess);

    TestTrue(TEXT("plain orphan was still deleted"),
        FindNodeByGuid(EventGraph, OrphanedBranchGuid) == nullptr);

    // Acceptance from the ticket: the MathExpression is either fully gone or fully intact.
    // Read the back-pointers directly -- UK2Node_Composite::GetExitNode() is check()-guarded,
    // so calling it on the broken state would assert instead of failing the test.
    if (UEdGraphNode* SurvivingNode = FindNodeByGuid(EventGraph, MathNodeGuid))
    {
        UK2Node_MathExpression* Surviving = Cast<UK2Node_MathExpression>(SurvivingNode);
        if (!Surviving)
        {
            AddError(TEXT("Node at the MathExpression guid is no longer a MathExpression"));
            return false;
        }

        TestTrue(TEXT("MathExpression kept its entry tunnel"),
            Surviving->InputSinkNode != nullptr);
        TestTrue(TEXT("MathExpression kept its exit tunnel"),
            Surviving->OutputSourceNode != nullptr);

        if (Surviving->BoundGraph)
        {
            TestEqual(TEXT("MathExpression bound graph kept every node"),
                Surviving->BoundGraph->Nodes.Num(), BoundNodesBefore);
        }
        else
        {
            AddError(TEXT("MathExpression survived without its bound graph"));
        }
    }

    return true;
}
