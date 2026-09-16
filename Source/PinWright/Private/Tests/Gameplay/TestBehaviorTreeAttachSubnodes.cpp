// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
// Compat header, not the raw engine one: UE 5.3 does not define
// UE_VERSION_NEWER_THAN_OR_EQUAL, which this file's version gate uses.
#include "Compat/EngineVersionCompat.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/Decorators/BTDecorator_Blackboard.h"
#include "BehaviorTree/Services/BTService_DefaultFocus.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "Handlers/AI/BehaviorTreeGraphNodeCompat.h"
#include "BehaviorTreeGraphNode_Root.h"

namespace
{
UBehaviorTreeGraphNode* FindBTTestGraphNode(UBehaviorTreeGraph* Graph, const FString& NodeId)
{
    if (!Graph)
    {
        return nullptr;
    }

    FGuid ParsedGuid;
    const bool bHasGuid = FGuid::Parse(NodeId, ParsedGuid);
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        UBehaviorTreeGraphNode* BTNode = PinWright::BehaviorTree::CastGraphNode(Node);
        if (BTNode && ((bHasGuid && BTNode->NodeGuid == ParsedGuid) || BTNode->NodeGuid.ToString() == NodeId))
        {
            return BTNode;
        }
    }

    return nullptr;
}

// Locate the hidden Root entry node the editor schema seeds. Callers read either
// NodeGuid.ToString() (the connect_nodes id) or GetName() (the addressable name)
// off the returned node.
UBehaviorTreeGraphNode_Root* FindBTTestRootNode(UBehaviorTreeGraph* Graph)
{
    if (!Graph)
    {
        return nullptr;
    }

    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (UBehaviorTreeGraphNode_Root* RootNode = Cast<UBehaviorTreeGraphNode_Root>(Node))
        {
            return RootNode;
        }
    }

    return nullptr;
}

FString FindBTTestRootNodeId(UBehaviorTreeGraph* Graph)
{
    UBehaviorTreeGraphNode_Root* RootNode = FindBTTestRootNode(Graph);
    return RootNode ? RootNode->NodeGuid.ToString() : FString();
}

bool InvokeBTTestHandler(const FString& MethodName, const TSharedPtr<FJsonObject>& Payload, FTestResponseCapture& Capture)
{
    return InvokeHandlerWithCapture(MethodName, Payload, Capture) && Capture.bWasCalled && Capture.bSuccess && Capture.Result.IsValid();
}

// Builds the fixture the blackboard-key tests need: a Blackboard carrying one Bool key and
// a Behavior Tree bound to it, both created through the RPCs a caller would use. The tree's
// BlackboardAsset is then assigned directly because no verb assigns a blackboard to a tree,
// and the hidden Root node is pointed at the SAME asset so nothing re-syncs it back - a
// freshly placed Root adopts whatever blackboard happens to be loaded in the editor
// (UBehaviorTreeGraphNode_Root::PostPlacedNewNode), which is the ambiguity that let the
// reported decorator silently resolve to SelfActor.
bool CreateBTBoundToBlackboardKey(const FString& BTName, const FString& BBName,
    const FString& SavePath, const FString& KeyName, FString& OutBTAssetPath,
    UBlackboardData*& OutBlackboard)
{
    FTestResponseCapture BlackboardCapture;
    TSharedPtr<FJsonObject> BlackboardPayload = MakeShared<FJsonObject>();
    BlackboardPayload->SetStringField(TEXT("name"), BBName);
    BlackboardPayload->SetStringField(TEXT("path"), SavePath);
    if (!InvokeBTTestHandler(TEXT("ai.create_blackboard_asset"), BlackboardPayload, BlackboardCapture))
    {
        return false;
    }

    FString BlackboardPath;
    if (!BlackboardCapture.Result->TryGetStringField(TEXT("blackboardPath"), BlackboardPath))
    {
        return false;
    }

    FTestResponseCapture KeyCapture;
    TSharedPtr<FJsonObject> KeyPayload = MakeShared<FJsonObject>();
    KeyPayload->SetStringField(TEXT("blackboardPath"), BlackboardPath);
    KeyPayload->SetStringField(TEXT("keyName"), KeyName);
    KeyPayload->SetStringField(TEXT("keyType"), TEXT("Bool"));
    if (!InvokeBTTestHandler(TEXT("ai.add_blackboard_key"), KeyPayload, KeyCapture))
    {
        return false;
    }

    FTestResponseCapture CreateCapture;
    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("name"), BTName);
    CreatePayload->SetStringField(TEXT("savePath"), SavePath);
    if (!InvokeBTTestHandler(TEXT("behavior_tree.create"), CreatePayload, CreateCapture))
    {
        return false;
    }
    if (!CreateCapture.Result->TryGetStringField(TEXT("assetPath"), OutBTAssetPath))
    {
        return false;
    }

    OutBlackboard = LoadObject<UBlackboardData>(nullptr, *BlackboardPath);
    UBehaviorTree* BehaviorTree = LoadObject<UBehaviorTree>(nullptr, *OutBTAssetPath);
    if (!OutBlackboard || !BehaviorTree)
    {
        return false;
    }

    BehaviorTree->BlackboardAsset = OutBlackboard;
    if (UBehaviorTreeGraphNode_Root* RootNode = FindBTTestRootNode(Cast<UBehaviorTreeGraph>(BehaviorTree->BTGraph)))
    {
        RootNode->BlackboardAsset = OutBlackboard;
    }
    return true;
}

// Adds a Sequence composite and wires the hidden Root to it. Returns the composite's node id.
FString AddBTTestSequenceUnderRoot(const FString& AssetPath, UBehaviorTreeGraph* Graph)
{
    FTestResponseCapture SequenceCapture;
    TSharedPtr<FJsonObject> SequencePayload = MakeShared<FJsonObject>();
    SequencePayload->SetStringField(TEXT("assetPath"), AssetPath);
    SequencePayload->SetStringField(TEXT("nodeType"), TEXT("Sequence"));
    SequencePayload->SetNumberField(TEXT("x"), 200.0);
    SequencePayload->SetNumberField(TEXT("y"), 100.0);
    if (!InvokeBTTestHandler(TEXT("behavior_tree.add_node"), SequencePayload, SequenceCapture))
    {
        return FString();
    }

    FString SequenceNodeId;
    if (!SequenceCapture.Result->TryGetStringField(TEXT("nodeId"), SequenceNodeId) || SequenceNodeId.IsEmpty())
    {
        return FString();
    }

    FTestResponseCapture ConnectCapture;
    TSharedPtr<FJsonObject> ConnectPayload = MakeShared<FJsonObject>();
    ConnectPayload->SetStringField(TEXT("assetPath"), AssetPath);
    ConnectPayload->SetStringField(TEXT("parentNodeId"), FindBTTestRootNodeId(Graph));
    ConnectPayload->SetStringField(TEXT("childNodeId"), SequenceNodeId);
    if (!InvokeBTTestHandler(TEXT("behavior_tree.connect_nodes"), ConnectPayload, ConnectCapture))
    {
        return FString();
    }

    return SequenceNodeId;
}

// Reads a node instance's FBlackboardKeySelector by reflection - the engine declares both
// BlackboardKey and the fields below it protected, so a test cannot reach them by member.
const FBlackboardKeySelector* ReadBTBlackboardKey(UObject* NodeInstance)
{
    FStructProperty* SelectorProperty = NodeInstance
        ? CastField<FStructProperty>(NodeInstance->GetClass()->FindPropertyByName(TEXT("BlackboardKey")))
        : nullptr;
    if (!SelectorProperty || SelectorProperty->Struct != FBlackboardKeySelector::StaticStruct())
    {
        return nullptr;
    }
    return SelectorProperty->ContainerPtrToValuePtr<FBlackboardKeySelector>(NodeInstance);
}

bool BTTestJsonArrayContainsString(const TArray<TSharedPtr<FJsonValue>>* Array, const TCHAR* Wanted)
{
    if (!Array)
    {
        return false;
    }
    for (const TSharedPtr<FJsonValue>& Value : *Array)
    {
        FString AsString;
        if (Value.IsValid() && Value->TryGetString(AsString) && AsString == Wanted)
        {
            return true;
        }
    }
    return false;
}

bool BTTestDroppedFieldsNameKey(const TArray<TSharedPtr<FJsonValue>>* Dropped, const TCHAR* Wanted)
{
    if (!Dropped)
    {
        return false;
    }
    for (const TSharedPtr<FJsonValue>& Value : *Dropped)
    {
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        FString Key;
        if (Value.IsValid() && Value->TryGetObject(Entry) && Entry && (*Entry).IsValid() &&
            (*Entry)->TryGetStringField(TEXT("name"), Key) && Key == Wanted)
        {
            return true;
        }
    }
    return false;
}
}

// Regression: behavior_tree.create must surface the hidden Root entry node through
// the RPC result (rootNodeId/rootNodeName) so callers can connect their top composite
// to it WITHOUT reading C++ source or reflecting on the graph. If the fix is reverted
// the create result lacks rootNodeId and this test fails at the first assertion; the
// connect-using-only-RPC-data step proves the returned id is the functional Root.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBTCreateReturnsRootNodeTest,
    "PinWright.behavior_tree.create.ReturnsConnectableRootNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBTCreateReturnsRootNodeTest::RunTest(const FString& Parameters)
{
    const FString AssetName = FString::Printf(TEXT("BT_RootDiscover_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString SavePath = TEXT("/Game/McpTests/BehaviorTree");
    const FString PackagePath = SavePath / AssetName;
    CleanupTestAsset(PackagePath);
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackagePath);
    };

    FTestResponseCapture CreateCapture;
    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("name"), AssetName);
    CreatePayload->SetStringField(TEXT("savePath"), SavePath);
    if (!TestTrue(TEXT("behavior_tree.create succeeded"), InvokeBTTestHandler(TEXT("behavior_tree.create"), CreatePayload, CreateCapture)))
    {
        return false;
    }

    // The core fix: the Root entry node id/name must be in the RPC result.
    FString RootNodeId;
    const bool bHasRootId = CreateCapture.Result->TryGetStringField(TEXT("rootNodeId"), RootNodeId);
    TestTrue(TEXT("create result carries rootNodeId"), bHasRootId);
    TestFalse(TEXT("rootNodeId is non-empty"), RootNodeId.IsEmpty());

    FString RootNodeName;
    const bool bHasRootName = CreateCapture.Result->TryGetStringField(TEXT("rootNodeName"), RootNodeName);
    TestTrue(TEXT("create result carries rootNodeName"), bHasRootName);
    TestFalse(TEXT("rootNodeName is non-empty"), RootNodeName.IsEmpty());

    if (!bHasRootId || RootNodeId.IsEmpty())
    {
        return false;
    }

    FString AssetPath;
    TestTrue(TEXT("create returned assetPath"), CreateCapture.Result->TryGetStringField(TEXT("assetPath"), AssetPath));

    // Add a top composite, then connect Root -> composite using ONLY the RPC-returned
    // rootNodeId — no graph reflection. This is the flow the ticket says was impossible.
    FTestResponseCapture SelectorCapture;
    TSharedPtr<FJsonObject> SelectorPayload = MakeShared<FJsonObject>();
    SelectorPayload->SetStringField(TEXT("assetPath"), AssetPath);
    SelectorPayload->SetStringField(TEXT("nodeType"), TEXT("Selector"));
    SelectorPayload->SetNumberField(TEXT("x"), 0.0);
    SelectorPayload->SetNumberField(TEXT("y"), 200.0);
    if (!TestTrue(TEXT("behavior_tree.add_node Selector succeeded"), InvokeBTTestHandler(TEXT("behavior_tree.add_node"), SelectorPayload, SelectorCapture)))
    {
        return false;
    }

    FString SelectorNodeId;
    TestTrue(TEXT("Selector node id returned"), SelectorCapture.Result->TryGetStringField(TEXT("nodeId"), SelectorNodeId));

    FTestResponseCapture ConnectCapture;
    TSharedPtr<FJsonObject> ConnectPayload = MakeShared<FJsonObject>();
    ConnectPayload->SetStringField(TEXT("assetPath"), AssetPath);
    ConnectPayload->SetStringField(TEXT("parentNodeId"), RootNodeId); // RPC-returned id only
    ConnectPayload->SetStringField(TEXT("childNodeId"), SelectorNodeId);
    if (!TestTrue(TEXT("Root (from rootNodeId) connects to Selector"), InvokeBTTestHandler(TEXT("behavior_tree.connect_nodes"), ConnectPayload, ConnectCapture)))
    {
        return false;
    }

    // rootNodeName must also resolve the same Root via connect_nodes' name-matching path.
    UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *AssetPath);
    UBehaviorTreeGraph* BTGraph = BT ? Cast<UBehaviorTreeGraph>(BT->BTGraph) : nullptr;
    TestNotNull(TEXT("Behavior Tree graph loaded"), BTGraph);
    if (!BTGraph)
    {
        return false;
    }
    UBehaviorTreeGraphNode_Root* RootGraphNode = FindBTTestRootNode(BTGraph);
    TestNotNull(TEXT("Root graph node located"), RootGraphNode);
    TestEqual(TEXT("rootNodeName equals the Root graph node's GetName()"), RootNodeName,
        RootGraphNode ? RootGraphNode->GetName() : FString());

    // The tree now runs: connecting the Root entry node to the Selector and recompiling
    // (UpdateAsset) makes the Selector the runtime root composite. In UE's BT model the
    // graph node wired to the Root entry's output pin becomes BTAsset->RootNode directly
    // (CreateBTFromGraph: BTAsset->RootNode = RootEdNode->NodeInstance) — it is NOT held as
    // a child of a separate wrapper. So the proof the connection took effect at runtime is
    // that BT->RootNode is non-null and is exactly the connected Selector's node instance.
    UBTCompositeNode* RuntimeRoot = BT->RootNode;
    TestNotNull(TEXT("Runtime root composite exists after connecting via rootNodeId"), RuntimeRoot);

    UBehaviorTreeGraphNode* SelectorGraphNode = FindBTTestGraphNode(BTGraph, SelectorNodeId);
    TestNotNull(TEXT("Selector graph node located by RPC-returned id"), SelectorGraphNode);
    if (RuntimeRoot && SelectorGraphNode)
    {
        const UObject* SelectorInstance = SelectorGraphNode->NodeInstance;
        TestTrue(TEXT("Runtime root is the connected Selector's node instance"),
            static_cast<const UObject*>(RuntimeRoot) == SelectorInstance);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBTAttachDecoratorServiceToParentTest,
    "PinWright.behavior_tree.attach_subnodes.AttachesDecoratorServiceToParent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBTAttachDecoratorServiceToParentTest::RunTest(const FString& Parameters)
{
    const FString AssetName = FString::Printf(TEXT("BT_AttachSubnodes_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString SavePath = TEXT("/Game/McpTests/BehaviorTree");
    const FString PackagePath = SavePath / AssetName;
    CleanupTestAsset(PackagePath);
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackagePath);
    };

    FTestResponseCapture CreateCapture;
    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("name"), AssetName);
    CreatePayload->SetStringField(TEXT("savePath"), SavePath);
    if (!TestTrue(TEXT("behavior_tree.create succeeded"), InvokeBTTestHandler(TEXT("behavior_tree.create"), CreatePayload, CreateCapture)))
    {
        return false;
    }

    FString AssetPath;
    TestTrue(TEXT("create returned assetPath"), CreateCapture.Result->TryGetStringField(TEXT("assetPath"), AssetPath));
    TestFalse(TEXT("assetPath is non-empty"), AssetPath.IsEmpty());

    FTestResponseCapture SequenceCapture;
    TSharedPtr<FJsonObject> SequencePayload = MakeShared<FJsonObject>();
    SequencePayload->SetStringField(TEXT("assetPath"), AssetPath);
    SequencePayload->SetStringField(TEXT("nodeType"), TEXT("Sequence"));
    SequencePayload->SetNumberField(TEXT("x"), 200.0);
    SequencePayload->SetNumberField(TEXT("y"), 100.0);
    if (!TestTrue(TEXT("behavior_tree.add_node Sequence succeeded"), InvokeBTTestHandler(TEXT("behavior_tree.add_node"), SequencePayload, SequenceCapture)))
    {
        return false;
    }

    FString SequenceNodeId;
    TestTrue(TEXT("Sequence node id returned"), SequenceCapture.Result->TryGetStringField(TEXT("nodeId"), SequenceNodeId));
    TestFalse(TEXT("Sequence node id is non-empty"), SequenceNodeId.IsEmpty());

    FTestResponseCapture WaitCapture;
    TSharedPtr<FJsonObject> WaitPayload = MakeShared<FJsonObject>();
    WaitPayload->SetStringField(TEXT("assetPath"), AssetPath);
    WaitPayload->SetStringField(TEXT("nodeType"), TEXT("Wait"));
    WaitPayload->SetNumberField(TEXT("x"), 500.0);
    WaitPayload->SetNumberField(TEXT("y"), 220.0);
    if (!TestTrue(TEXT("behavior_tree.add_node Wait succeeded"), InvokeBTTestHandler(TEXT("behavior_tree.add_node"), WaitPayload, WaitCapture)))
    {
        return false;
    }

    FString WaitNodeId;
    TestTrue(TEXT("Wait node id returned"), WaitCapture.Result->TryGetStringField(TEXT("nodeId"), WaitNodeId));
    TestFalse(TEXT("Wait node id is non-empty"), WaitNodeId.IsEmpty());

    UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *AssetPath);
    UBehaviorTreeGraph* BTGraph = BT ? Cast<UBehaviorTreeGraph>(BT->BTGraph) : nullptr;
    TestNotNull(TEXT("Behavior Tree graph loaded"), BTGraph);
    if (!BTGraph)
    {
        return false;
    }

    const FString RootNodeId = FindBTTestRootNodeId(BTGraph);
    TestFalse(TEXT("Root node id is non-empty"), RootNodeId.IsEmpty());

    FTestResponseCapture ConnectRootCapture;
    TSharedPtr<FJsonObject> ConnectRootPayload = MakeShared<FJsonObject>();
    ConnectRootPayload->SetStringField(TEXT("assetPath"), AssetPath);
    ConnectRootPayload->SetStringField(TEXT("parentNodeId"), RootNodeId);
    ConnectRootPayload->SetStringField(TEXT("childNodeId"), SequenceNodeId);
    if (!TestTrue(TEXT("Root connects to Sequence"), InvokeBTTestHandler(TEXT("behavior_tree.connect_nodes"), ConnectRootPayload, ConnectRootCapture)))
    {
        return false;
    }

    FTestResponseCapture ConnectTaskCapture;
    TSharedPtr<FJsonObject> ConnectTaskPayload = MakeShared<FJsonObject>();
    ConnectTaskPayload->SetStringField(TEXT("assetPath"), AssetPath);
    ConnectTaskPayload->SetStringField(TEXT("parentNodeId"), SequenceNodeId);
    ConnectTaskPayload->SetStringField(TEXT("childNodeId"), WaitNodeId);
    if (!TestTrue(TEXT("Sequence connects to Wait"), InvokeBTTestHandler(TEXT("behavior_tree.connect_nodes"), ConnectTaskPayload, ConnectTaskCapture)))
    {
        return false;
    }

    FTestResponseCapture DecoratorCapture;
    TSharedPtr<FJsonObject> DecoratorPayload = MakeShared<FJsonObject>();
    DecoratorPayload->SetStringField(TEXT("assetPath"), AssetPath);
    DecoratorPayload->SetStringField(TEXT("parentNodeId"), WaitNodeId);
    DecoratorPayload->SetStringField(TEXT("decoratorClass"), TEXT("Blackboard"));
    if (!TestTrue(TEXT("behavior_tree.attach_decorator succeeded"), InvokeBTTestHandler(TEXT("behavior_tree.attach_decorator"), DecoratorPayload, DecoratorCapture)))
    {
        return false;
    }

    FString DecoratorNodeId;
    TestTrue(TEXT("Decorator node id returned"), DecoratorCapture.Result->TryGetStringField(TEXT("nodeId"), DecoratorNodeId));
    TestFalse(TEXT("Decorator node id is non-empty"), DecoratorNodeId.IsEmpty());

    FTestResponseCapture ServiceCapture;
    TSharedPtr<FJsonObject> ServicePayload = MakeShared<FJsonObject>();
    ServicePayload->SetStringField(TEXT("assetPath"), AssetPath);
    ServicePayload->SetStringField(TEXT("parentNodeId"), WaitNodeId);
    ServicePayload->SetStringField(TEXT("serviceClass"), TEXT("DefaultFocus"));
    if (!TestTrue(TEXT("behavior_tree.attach_service succeeded"), InvokeBTTestHandler(TEXT("behavior_tree.attach_service"), ServicePayload, ServiceCapture)))
    {
        return false;
    }

    FString ServiceNodeId;
    TestTrue(TEXT("Service node id returned"), ServiceCapture.Result->TryGetStringField(TEXT("nodeId"), ServiceNodeId));
    TestFalse(TEXT("Service node id is non-empty"), ServiceNodeId.IsEmpty());

    UBehaviorTreeGraphNode* WaitGraphNode = FindBTTestGraphNode(BTGraph, WaitNodeId);
    TestNotNull(TEXT("Wait graph node found"), WaitGraphNode);
    if (!WaitGraphNode)
    {
        return false;
    }

    TestEqual(TEXT("Wait graph node has one decorator subnode"), WaitGraphNode->Decorators.Num(), 1);
    TestEqual(TEXT("Wait graph node has one service subnode"), WaitGraphNode->Services.Num(), 1);
    if (WaitGraphNode->Decorators.Num() != 1 || WaitGraphNode->Services.Num() != 1)
    {
        return false;
    }

    UBehaviorTreeGraphNode* DecoratorGraphNode = WaitGraphNode->Decorators[0];
    UBehaviorTreeGraphNode* ServiceGraphNode = WaitGraphNode->Services[0];
    TestTrue(TEXT("Decorator subnode ParentNode points at Wait"), DecoratorGraphNode && DecoratorGraphNode->ParentNode == WaitGraphNode);
    TestTrue(TEXT("Service subnode ParentNode points at Wait"), ServiceGraphNode && ServiceGraphNode->ParentNode == WaitGraphNode);

    UBTDecorator_Blackboard* DecoratorInstance = DecoratorGraphNode ? Cast<UBTDecorator_Blackboard>(DecoratorGraphNode->NodeInstance) : nullptr;
    UBTService_DefaultFocus* ServiceInstance = ServiceGraphNode ? Cast<UBTService_DefaultFocus>(ServiceGraphNode->NodeInstance) : nullptr;
    TestNotNull(TEXT("Decorator NodeInstance is UBTDecorator_Blackboard"), DecoratorInstance);
    TestNotNull(TEXT("Service NodeInstance is UBTService_DefaultFocus"), ServiceInstance);

    UBTCompositeNode* RuntimeRoot = BT->RootNode;
    TestNotNull(TEXT("Runtime root composite exists after handler attachment"), RuntimeRoot);
    if (!RuntimeRoot)
    {
        return false;
    }

    TestEqual(TEXT("Runtime root has one child"), RuntimeRoot->Children.Num(), 1);
    if (RuntimeRoot->Children.Num() != 1)
    {
        return false;
    }

    FBTCompositeChild& RuntimeChild = RuntimeRoot->Children[0];
    TestEqual(TEXT("Runtime child has one decorator"), RuntimeChild.Decorators.Num(), 1);
    TestTrue(TEXT("Runtime child decorator is attached instance"), RuntimeChild.Decorators.Num() == 1 && RuntimeChild.Decorators[0] == DecoratorInstance);

    UBTTaskNode* RuntimeTask = RuntimeChild.ChildTask;
    TestNotNull(TEXT("Runtime child task exists"), RuntimeTask);
    if (RuntimeTask)
    {
        TestEqual(TEXT("Runtime task has one service"), RuntimeTask->Services.Num(), 1);
        TestTrue(TEXT("Runtime task service is attached instance"), RuntimeTask->Services.Num() == 1 && RuntimeTask->Services[0] == ServiceInstance);
    }

    return true;
}

// Regression: behavior_tree.attach_service must reject the unresolvable short name
// "Blackboard" with an INVALID_CLASS error that ENUMERATES the concrete service names
// the caller can actually use (board E-bt-attach-service-blackboard-name-misleads).
// The asymmetry is real: UBTDecorator_Blackboard is concrete (so attach_decorator
// "Blackboard" works) but the AIModule service base UBTService_BlackboardBase is
// UCLASS(Abstract), so there is no concrete UBTService_Blackboard and attach_service
// "Blackboard" cannot resolve. Before the fix the error was a bare "Could not resolve
// service class 'Blackboard'." with no recovery path. After the fix it carries an
// availableClasses list naming the concrete services (DefaultFocus, RunEQS) and
// omitting "Blackboard" itself. If the enumeration is reverted the error result has no
// availableClasses field and the "names DefaultFocus" assertion fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBTAttachServiceBlackboardHintsConcreteNamesTest,
    "PinWright.behavior_tree.attach_service.RejectsBlackboardWithConcreteHint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBTAttachServiceBlackboardHintsConcreteNamesTest::RunTest(const FString& Parameters)
{
    const FString AssetName = FString::Printf(TEXT("BT_SvcBlackboardHint_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString SavePath = TEXT("/Game/McpTests/BehaviorTree");
    const FString PackagePath = SavePath / AssetName;
    CleanupTestAsset(PackagePath);
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackagePath);
    };

    FTestResponseCapture CreateCapture;
    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("name"), AssetName);
    CreatePayload->SetStringField(TEXT("savePath"), SavePath);
    if (!TestTrue(TEXT("behavior_tree.create succeeded"), InvokeBTTestHandler(TEXT("behavior_tree.create"), CreatePayload, CreateCapture)))
    {
        return false;
    }

    FString AssetPath;
    TestTrue(TEXT("create returned assetPath"), CreateCapture.Result->TryGetStringField(TEXT("assetPath"), AssetPath));
    if (AssetPath.IsEmpty())
    {
        return false;
    }

    // A composite is a valid subnode parent, so add a Selector to attach onto.
    FTestResponseCapture SelectorCapture;
    TSharedPtr<FJsonObject> SelectorPayload = MakeShared<FJsonObject>();
    SelectorPayload->SetStringField(TEXT("assetPath"), AssetPath);
    SelectorPayload->SetStringField(TEXT("nodeType"), TEXT("Selector"));
    SelectorPayload->SetNumberField(TEXT("x"), 0.0);
    SelectorPayload->SetNumberField(TEXT("y"), 200.0);
    if (!TestTrue(TEXT("behavior_tree.add_node Selector succeeded"), InvokeBTTestHandler(TEXT("behavior_tree.add_node"), SelectorPayload, SelectorCapture)))
    {
        return false;
    }
    FString SelectorNodeId;
    TestTrue(TEXT("Selector node id returned"), SelectorCapture.Result->TryGetStringField(TEXT("nodeId"), SelectorNodeId));
    if (SelectorNodeId.IsEmpty())
    {
        return false;
    }

    // attach_service serviceClass:"Blackboard" must FAIL — no concrete UBTService_Blackboard.
    TSharedPtr<FJsonObject> ServicePayload = MakeShared<FJsonObject>();
    ServicePayload->SetStringField(TEXT("assetPath"), AssetPath);
    ServicePayload->SetStringField(TEXT("parentNodeId"), SelectorNodeId);
    ServicePayload->SetStringField(TEXT("serviceClass"), TEXT("Blackboard"));

    FTestResponseCapture ServiceCapture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("behavior_tree.attach_service"), ServicePayload, ServiceCapture);
    TestTrue(TEXT("attach_service handler registered"), bFound);
    TestTrue(TEXT("attach_service response was produced"), ServiceCapture.bWasCalled);
    TestFalse(TEXT("attach_service serviceClass:'Blackboard' is rejected (no concrete UBTService_Blackboard)"),
        ServiceCapture.bSuccess);
    TestEqual(TEXT("rejection error code is INVALID_CLASS"),
        ServiceCapture.ErrorCode, FString(TEXT("INVALID_CLASS")));

    // Core regression: the error must enumerate concrete service names so the caller
    // can recover without reading engine C++.
    if (TestTrue(TEXT("INVALID_CLASS carries structured error data"), ServiceCapture.Result.IsValid()))
    {
        const TArray<TSharedPtr<FJsonValue>>* Available = nullptr;
        const bool bHasAvailable = ServiceCapture.Result->TryGetArrayField(TEXT("availableClasses"), Available);
        TestTrue(TEXT("error data carries availableClasses"), bHasAvailable && Available != nullptr);
        // DefaultFocus (UBTService_DefaultFocus) is the concrete blackboard-backed
        // service the docs now point callers to — it must appear in the hint.
        TestTrue(TEXT("availableClasses names the concrete DefaultFocus service"),
            JsonStringArrayContains(ServiceCapture.Result, TEXT("availableClasses"), TEXT("DefaultFocus")));
        // The abstract base name the caller tried must NOT be echoed back as available
        // — that would re-send them down the same dead end.
        TestFalse(TEXT("availableClasses does not list the unresolvable 'Blackboard'"),
            JsonStringArrayContains(ServiceCapture.Result, TEXT("availableClasses"), TEXT("Blackboard")));
    }

    // Sanity: the SAME short name resolves for attach_decorator (concrete
    // UBTDecorator_Blackboard) — this is the asymmetry the fix documents, not a
    // blanket rejection of "Blackboard".
    TSharedPtr<FJsonObject> DecoratorPayload = MakeShared<FJsonObject>();
    DecoratorPayload->SetStringField(TEXT("assetPath"), AssetPath);
    DecoratorPayload->SetStringField(TEXT("parentNodeId"), SelectorNodeId);
    DecoratorPayload->SetStringField(TEXT("decoratorClass"), TEXT("Blackboard"));
    FTestResponseCapture DecoratorCapture;
    TestTrue(TEXT("attach_decorator decoratorClass:'Blackboard' resolves (concrete UBTDecorator_Blackboard)"),
        InvokeBTTestHandler(TEXT("behavior_tree.attach_decorator"), DecoratorPayload, DecoratorCapture));

    return true;
}

// Regression: behavior_tree.set_node_properties must NOT report success when
// every supplied property key fails to land. Before the fix
// (B-bt-set-node-properties-silent-noop) ApplyBTNodeProperties discarded both
// failure paths — an unknown property name (FindPropertyCI null -> bare continue)
// and an unconvertible value (ApplyJsonValueToProperty false return ignored) —
// and the handler always called SendSuccess, so a write where nothing landed
// looked successful. Here we feed a Wait task two keys that both fail:
//   - "WaitTime": 3.5  -> in UE 5.7 UBTTask_Wait::WaitTime is an
//     FValueOrBBKey_Float struct; a bare number hits the struct branch and
//     ApplyJsonValueToProperty returns "Unsupported JSON type for struct property".
//   - "TotallyUnknownProp": 1 -> resolves to no property (FindPropertyCI null).
// The call must now be an error (INVALID_PROPERTY) carrying a droppedFields list
// naming both keys. If the fix is reverted this returns bSuccess == true and the
// first assertion fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBTSetNodePropertiesRejectsDroppedWritesTest,
    "PinWright.behavior_tree.set_node_properties.RejectsSilentlyDroppedWrites",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBTSetNodePropertiesRejectsDroppedWritesTest::RunTest(const FString& Parameters)
{
    const FString AssetName = FString::Printf(TEXT("BT_SetNodeProps_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString SavePath = TEXT("/Game/McpTests/BehaviorTree");
    const FString PackagePath = SavePath / AssetName;
    CleanupTestAsset(PackagePath);
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackagePath);
    };

    FTestResponseCapture CreateCapture;
    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("name"), AssetName);
    CreatePayload->SetStringField(TEXT("savePath"), SavePath);
    if (!TestTrue(TEXT("behavior_tree.create succeeded"), InvokeBTTestHandler(TEXT("behavior_tree.create"), CreatePayload, CreateCapture)))
    {
        return false;
    }

    FString AssetPath;
    TestTrue(TEXT("create returned assetPath"), CreateCapture.Result->TryGetStringField(TEXT("assetPath"), AssetPath));
    if (AssetPath.IsEmpty())
    {
        return false;
    }

    // Add a Wait task whose NodeInstance carries WaitTime (struct in 5.7, float earlier).
    FTestResponseCapture WaitCapture;
    TSharedPtr<FJsonObject> WaitPayload = MakeShared<FJsonObject>();
    WaitPayload->SetStringField(TEXT("assetPath"), AssetPath);
    WaitPayload->SetStringField(TEXT("nodeType"), TEXT("Wait"));
    WaitPayload->SetNumberField(TEXT("x"), 300.0);
    WaitPayload->SetNumberField(TEXT("y"), 200.0);
    if (!TestTrue(TEXT("behavior_tree.add_node Wait succeeded"), InvokeBTTestHandler(TEXT("behavior_tree.add_node"), WaitPayload, WaitCapture)))
    {
        return false;
    }

    FString WaitNodeId;
    TestTrue(TEXT("Wait node id returned"), WaitCapture.Result->TryGetStringField(TEXT("nodeId"), WaitNodeId));
    if (WaitNodeId.IsEmpty())
    {
        return false;
    }

    // Set two keys that both fail to land: an unknown property name and (on 5.7)
    // a bare-number value into the FValueOrBBKey_Float WaitTime struct.
    TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
    Props->SetNumberField(TEXT("WaitTime"), 3.5);
    Props->SetNumberField(TEXT("TotallyUnknownProp"), 1.0);

    TSharedPtr<FJsonObject> SetPayload = MakeShared<FJsonObject>();
    SetPayload->SetStringField(TEXT("assetPath"), AssetPath);
    SetPayload->SetStringField(TEXT("nodeId"), WaitNodeId);
    SetPayload->SetObjectField(TEXT("properties"), Props);

    FTestResponseCapture SetCapture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("behavior_tree.set_node_properties"), SetPayload, SetCapture);
    TestTrue(TEXT("set_node_properties handler registered"), bFound);
    TestTrue(TEXT("set_node_properties response was produced"), SetCapture.bWasCalled);

    // Core regression: an all-keys-failed write must NOT report success.
    TestFalse(TEXT("set_node_properties reports failure (not silent success) when all keys are dropped"),
        SetCapture.bSuccess);
    TestEqual(TEXT("set_node_properties error code is INVALID_PROPERTY"),
        SetCapture.ErrorCode, FString(TEXT("INVALID_PROPERTY")));

    // The dropped keys must be surfaced so the caller can self-correct.
    if (SetCapture.Result.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* Dropped = nullptr;
        const bool bHasDropped = SetCapture.Result->TryGetArrayField(TEXT("droppedFields"), Dropped);
        TestTrue(TEXT("error result carries droppedFields"), bHasDropped && Dropped != nullptr);
        if (bHasDropped && Dropped)
        {
            auto DroppedContainsKey = [Dropped](const TCHAR* Wanted) -> bool
            {
                for (const TSharedPtr<FJsonValue>& Val : *Dropped)
                {
                    const TSharedPtr<FJsonObject>* Entry = nullptr;
                    FString Key;
                    if (Val.IsValid() && Val->TryGetObject(Entry) && Entry && (*Entry).IsValid() &&
                        (*Entry)->TryGetStringField(TEXT("name"), Key) && Key == Wanted)
                    {
                        return true;
                    }
                }
                return false;
            };
            // The unknown-key drop is engine-version-independent: WaitTime only
            // fails on 5.7+ where it became an FValueOrBBKey_Float struct (a bare
            // number into the pre-5.7 float would have applied), so gate that one.
            TestTrue(TEXT("droppedFields names the unknown property"), DroppedContainsKey(TEXT("TotallyUnknownProp")));
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
            TestTrue(TEXT("droppedFields names the unconvertible WaitTime key"), DroppedContainsKey(TEXT("WaitTime")));
#endif
        }
    }
    else
    {
        AddError(TEXT("set_node_properties error carried no structured result"));
    }

    return true;
}

// Regression: behavior_tree.attach_decorator must APPLY the `properties` payload, not just
// accept it. The reported shape (board B-attach-decorator-properties-silently-ignored) was a
// success response carrying a nodeId while the Blackboard condition stayed bound to the
// blackboard's first key: a bare-string BlackboardKey reached ApplyJsonValueToProperty's
// struct branch, failed both the JSON and the ImportText parse, and the drop list was
// discarded by the call site. The branch then evaluated an always-set key and the tree still
// compiled, so nothing surfaced it.
//
// Three assertions carry the fix, and each fails on its own reversion:
//   * SelectedKeyName - the string was coerced onto the selector at all.
//   * SelectedKeyID == the blackboard's id for that key - the selector was RESOLVED. That is
//     the half a plain reflection store never does, and the half the runtime reads.
//   * OperationType == NotSet - the engine's own PostEditChangeProperty ran (it is the only
//     code that maps BasicOperation onto OperationType) AND key selectors were applied before
//     the other keys, since that mapping needs the resolved key's type.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBTAttachDecoratorAppliesBlackboardKeyTest,
    "PinWright.behavior_tree.attach_decorator.AppliesBlackboardKeyProperty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBTAttachDecoratorAppliesBlackboardKeyTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SavePath = TEXT("/Game/McpTests/BehaviorTree");
    const FString AssetName = FString::Printf(TEXT("BT_BBKeyApply_%s"), *Suffix);
    const FString BlackboardName = FString::Printf(TEXT("BB_BBKeyApply_%s"), *Suffix);
    const FString PackagePath = SavePath / AssetName;
    const FString BlackboardPackagePath = SavePath / BlackboardName;
    const FString KeyName = TEXT("bDwellStalled");
    CleanupTestAsset(PackagePath);
    CleanupTestAsset(BlackboardPackagePath);
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackagePath);
        CleanupTestAsset(BlackboardPackagePath);
    };

    FString AssetPath;
    UBlackboardData* Blackboard = nullptr;
    if (!TestTrue(TEXT("Behavior Tree + Blackboard fixture created"),
            CreateBTBoundToBlackboardKey(AssetName, BlackboardName, SavePath, KeyName, AssetPath, Blackboard)))
    {
        return false;
    }

    UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *AssetPath);
    UBehaviorTreeGraph* BTGraph = BT ? Cast<UBehaviorTreeGraph>(BT->BTGraph) : nullptr;
    TestNotNull(TEXT("Behavior Tree graph loaded"), BTGraph);
    if (!BTGraph)
    {
        return false;
    }

    const FString SequenceNodeId = AddBTTestSequenceUnderRoot(AssetPath, BTGraph);
    if (!TestFalse(TEXT("Sequence node id is non-empty"), SequenceNodeId.IsEmpty()))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
    Props->SetStringField(TEXT("BlackboardKey"), KeyName);
    Props->SetStringField(TEXT("BasicOperation"), TEXT("NotSet"));
    Props->SetStringField(TEXT("NotifyObserver"), TEXT("ValueChange"));

    TSharedPtr<FJsonObject> DecoratorPayload = MakeShared<FJsonObject>();
    DecoratorPayload->SetStringField(TEXT("assetPath"), AssetPath);
    DecoratorPayload->SetStringField(TEXT("parentNodeId"), SequenceNodeId);
    DecoratorPayload->SetStringField(TEXT("decoratorClass"), TEXT("Blackboard"));
    DecoratorPayload->SetObjectField(TEXT("properties"), Props);

    FTestResponseCapture DecoratorCapture;
    if (!TestTrue(TEXT("behavior_tree.attach_decorator succeeded"),
            InvokeBTTestHandler(TEXT("behavior_tree.attach_decorator"), DecoratorPayload, DecoratorCapture)))
    {
        AddError(FString::Printf(TEXT("attach_decorator failed: %s: %s"),
            *DecoratorCapture.ErrorCode, *DecoratorCapture.Message));
        return false;
    }

    // Per-key confirmation in the response, so a caller never has to decompile to find out.
    const TArray<TSharedPtr<FJsonValue>>* AppliedProperties = nullptr;
    TestTrue(TEXT("attach result carries appliedProperties"),
        DecoratorCapture.Result->TryGetArrayField(TEXT("appliedProperties"), AppliedProperties));
    TestTrue(TEXT("appliedProperties names BlackboardKey"),
        BTTestJsonArrayContainsString(AppliedProperties, TEXT("BlackboardKey")));
    TestTrue(TEXT("appliedProperties names BasicOperation"),
        BTTestJsonArrayContainsString(AppliedProperties, TEXT("BasicOperation")));

    UBehaviorTreeGraphNode* SequenceGraphNode = FindBTTestGraphNode(BTGraph, SequenceNodeId);
    TestNotNull(TEXT("Sequence graph node found"), SequenceGraphNode);
    if (!SequenceGraphNode || !TestEqual(TEXT("Sequence has one decorator subnode"), SequenceGraphNode->Decorators.Num(), 1))
    {
        return false;
    }

    UBTDecorator_Blackboard* DecoratorInstance =
        Cast<UBTDecorator_Blackboard>(SequenceGraphNode->Decorators[0]->NodeInstance);
    TestNotNull(TEXT("Decorator NodeInstance is UBTDecorator_Blackboard"), DecoratorInstance);
    if (!DecoratorInstance)
    {
        return false;
    }

    const FBlackboardKeySelector* Selector = ReadBTBlackboardKey(DecoratorInstance);
    TestNotNull(TEXT("Decorator exposes a BlackboardKey selector"), Selector);
    if (!Selector)
    {
        return false;
    }

    // The core regression: the key the caller asked for, not the blackboard's first key.
    TestEqual(TEXT("BlackboardKey.SelectedKeyName is the requested key"),
        Selector->SelectedKeyName.ToString(), KeyName);
    TestTrue(TEXT("BlackboardKey resolved to a real blackboard entry"), Selector->IsSet());
    TestEqual(TEXT("BlackboardKey.SelectedKeyID matches the blackboard's id for that key"),
        static_cast<int32>(Selector->GetSelectedKeyID()),
        static_cast<int32>(Blackboard->GetKeyID(FName(*KeyName))));

    // OperationType is only written by UBTDecorator_Blackboard::PostEditChangeProperty, and
    // only when the selected key's type is already resolved - so this one assertion covers
    // both the change notification and the selector-first apply order.
    FByteProperty* OperationTypeProperty =
        CastField<FByteProperty>(DecoratorInstance->GetClass()->FindPropertyByName(TEXT("OperationType")));
    TestNotNull(TEXT("Decorator exposes OperationType"), OperationTypeProperty);
    if (OperationTypeProperty)
    {
        TestEqual(TEXT("BasicOperation reached the runtime OperationType byte"),
            static_cast<int32>(OperationTypeProperty->GetPropertyValue_InContainer(DecoratorInstance)),
            static_cast<int32>(EBasicKeyOperation::NotSet));
    }

    return true;
}

// Regression: a `properties` key that cannot land must FAIL the attach, and the subnode must
// not survive the rejection. "KeyQuery" is the reported spelling - it is the details-panel
// DisplayName of BasicOperation, not a UPROPERTY name, so it resolves to nothing. Before the
// fix the call returned a nodeId and no error, leaving a decorator whose condition was not
// the one the caller wrote. After it, the response is INVALID_PROPERTY with the key named in
// droppedFields, and the parent carries no decorator at all.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBTAttachDecoratorRejectsUnknownPropertyTest,
    "PinWright.behavior_tree.attach_decorator.RejectsUnknownPropertyName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBTAttachDecoratorRejectsUnknownPropertyTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SavePath = TEXT("/Game/McpTests/BehaviorTree");
    const FString AssetName = FString::Printf(TEXT("BT_BBKeyReject_%s"), *Suffix);
    const FString BlackboardName = FString::Printf(TEXT("BB_BBKeyReject_%s"), *Suffix);
    const FString PackagePath = SavePath / AssetName;
    const FString BlackboardPackagePath = SavePath / BlackboardName;
    CleanupTestAsset(PackagePath);
    CleanupTestAsset(BlackboardPackagePath);
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackagePath);
        CleanupTestAsset(BlackboardPackagePath);
    };

    FString AssetPath;
    UBlackboardData* Blackboard = nullptr;
    if (!TestTrue(TEXT("Behavior Tree + Blackboard fixture created"),
            CreateBTBoundToBlackboardKey(AssetName, BlackboardName, SavePath, TEXT("bDwellStalled"),
                AssetPath, Blackboard)))
    {
        return false;
    }

    UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *AssetPath);
    UBehaviorTreeGraph* BTGraph = BT ? Cast<UBehaviorTreeGraph>(BT->BTGraph) : nullptr;
    TestNotNull(TEXT("Behavior Tree graph loaded"), BTGraph);
    if (!BTGraph)
    {
        return false;
    }

    const FString SequenceNodeId = AddBTTestSequenceUnderRoot(AssetPath, BTGraph);
    if (!TestFalse(TEXT("Sequence node id is non-empty"), SequenceNodeId.IsEmpty()))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
    Props->SetStringField(TEXT("KeyQuery"), TEXT("IsUnset"));

    TSharedPtr<FJsonObject> DecoratorPayload = MakeShared<FJsonObject>();
    DecoratorPayload->SetStringField(TEXT("assetPath"), AssetPath);
    DecoratorPayload->SetStringField(TEXT("parentNodeId"), SequenceNodeId);
    DecoratorPayload->SetStringField(TEXT("decoratorClass"), TEXT("Blackboard"));
    DecoratorPayload->SetObjectField(TEXT("properties"), Props);

    FTestResponseCapture DecoratorCapture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("behavior_tree.attach_decorator"), DecoratorPayload, DecoratorCapture);
    TestTrue(TEXT("attach_decorator handler registered"), bFound);
    TestTrue(TEXT("attach_decorator response was produced"), DecoratorCapture.bWasCalled);

    // Core regression: an unappliable key must NOT report success.
    TestFalse(TEXT("attach_decorator reports failure when a property key is dropped"),
        DecoratorCapture.bSuccess);
    TestEqual(TEXT("attach_decorator error code is INVALID_PROPERTY"),
        DecoratorCapture.ErrorCode, FString(TEXT("INVALID_PROPERTY")));

    if (DecoratorCapture.Result.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* Dropped = nullptr;
        TestTrue(TEXT("error result carries droppedFields"),
            DecoratorCapture.Result->TryGetArrayField(TEXT("droppedFields"), Dropped));
        TestTrue(TEXT("droppedFields names the unknown property"),
            BTTestDroppedFieldsNameKey(Dropped, TEXT("KeyQuery")));

        bool bRolledBack = false;
        TestTrue(TEXT("error result reports the attach was rolled back"),
            DecoratorCapture.Result->TryGetBoolField(TEXT("rolledBack"), bRolledBack) && bRolledBack);
    }
    else
    {
        AddError(TEXT("attach_decorator error carried no structured result"));
    }

    // Nothing half-configured may survive the rejection.
    UBehaviorTreeGraphNode* SequenceGraphNode = FindBTTestGraphNode(BTGraph, SequenceNodeId);
    TestNotNull(TEXT("Sequence graph node found"), SequenceGraphNode);
    if (SequenceGraphNode)
    {
        TestEqual(TEXT("Rejected attach left no decorator on the parent"),
            SequenceGraphNode->Decorators.Num(), 0);
    }

    return true;
}
