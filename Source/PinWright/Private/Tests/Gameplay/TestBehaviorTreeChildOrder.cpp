// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Misc/Char.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTNode.h"
// UBTTaskNode is only forward-declared by BTCompositeNode.h; the child comparisons below need the
// complete type to convert FBTCompositeChild::ChildTask to UObject*.
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/Composites/BTComposite_SimpleParallel.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Handlers/AI/BehaviorTreeGraphNodeCompat.h"

namespace
{
bool InvokeBTOrderHandler(const FString& MethodName, const TSharedPtr<FJsonObject>& Payload, FTestResponseCapture& Capture)
{
    return InvokeHandlerWithCapture(MethodName, Payload, Capture) && Capture.bWasCalled && Capture.bSuccess && Capture.Result.IsValid();
}

UBehaviorTreeGraphNode* FindBTOrderGraphNode(UBehaviorTreeGraph* Graph, const FString& NodeId)
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
        if (BTNode && bHasGuid && BTNode->NodeGuid == ParsedGuid)
        {
            return BTNode;
        }
    }
    return nullptr;
}

// Scrapes the `nodeId: <guid>` field the BTIR decompiler emits at the head of every graph-node
// field list. Deliberately reads the TEXT rather than the graph, so what it returns is exactly
// what an agent holding only a decompile response would have to work with.
TArray<FString> ExtractBtirNodeIds(const FString& Ir)
{
    TArray<FString> Ids;
    const FString Token = TEXT("nodeId: ");
    int32 SearchStart = 0;
    while (SearchStart < Ir.Len())
    {
        const int32 Found = Ir.Find(Token, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchStart);
        if (Found == INDEX_NONE)
        {
            break;
        }

        const int32 ValueStart = Found + Token.Len();
        int32 ValueEnd = ValueStart;
        while (ValueEnd < Ir.Len() && FChar::IsHexDigit(Ir[ValueEnd]))
        {
            ++ValueEnd;
        }
        Ids.Add(Ir.Mid(ValueStart, ValueEnd - ValueStart));
        SearchStart = ValueEnd > ValueStart ? ValueEnd : ValueStart + 1;
    }
    return Ids;
}

// create -> add Selector -> connect Root -> Selector. Returns the tree's asset path and the
// Selector's node id; both come out of the RPC results the way a caller would get them.
bool CreateBTWithRootComposite(const FString& AssetName, const FString& SavePath,
    FString& OutAssetPath, FString& OutSelectorNodeId)
{
    FTestResponseCapture CreateCapture;
    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("name"), AssetName);
    CreatePayload->SetStringField(TEXT("savePath"), SavePath);
    if (!InvokeBTOrderHandler(TEXT("behavior_tree.create"), CreatePayload, CreateCapture))
    {
        return false;
    }

    FString RootNodeId;
    if (!CreateCapture.Result->TryGetStringField(TEXT("assetPath"), OutAssetPath)
        || !CreateCapture.Result->TryGetStringField(TEXT("rootNodeId"), RootNodeId))
    {
        return false;
    }

    FTestResponseCapture SelectorCapture;
    TSharedPtr<FJsonObject> SelectorPayload = MakeShared<FJsonObject>();
    SelectorPayload->SetStringField(TEXT("assetPath"), OutAssetPath);
    SelectorPayload->SetStringField(TEXT("nodeType"), TEXT("Selector"));
    SelectorPayload->SetNumberField(TEXT("x"), 0.0);
    SelectorPayload->SetNumberField(TEXT("y"), 200.0);
    if (!InvokeBTOrderHandler(TEXT("behavior_tree.add_node"), SelectorPayload, SelectorCapture)
        || !SelectorCapture.Result->TryGetStringField(TEXT("nodeId"), OutSelectorNodeId))
    {
        return false;
    }

    FTestResponseCapture ConnectCapture;
    TSharedPtr<FJsonObject> ConnectPayload = MakeShared<FJsonObject>();
    ConnectPayload->SetStringField(TEXT("assetPath"), OutAssetPath);
    ConnectPayload->SetStringField(TEXT("parentNodeId"), RootNodeId);
    ConnectPayload->SetStringField(TEXT("childNodeId"), OutSelectorNodeId);
    return InvokeBTOrderHandler(TEXT("behavior_tree.connect_nodes"), ConnectPayload, ConnectCapture);
}

// Adds an unconnected Wait task at (X, Y). Returns the new node's id.
bool AddBTLooseTask(const FString& AssetPath, double X, double Y, FString& OutNodeId)
{
    FTestResponseCapture AddCapture;
    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("assetPath"), AssetPath);
    AddPayload->SetStringField(TEXT("nodeType"), TEXT("Wait"));
    AddPayload->SetNumberField(TEXT("x"), X);
    AddPayload->SetNumberField(TEXT("y"), Y);
    return InvokeBTOrderHandler(TEXT("behavior_tree.add_node"), AddPayload, AddCapture)
        && AddCapture.Result->TryGetStringField(TEXT("nodeId"), OutNodeId);
}

// Adds a Wait task at (X, Y) and wires it under ParentNodeId. Returns the new node's id.
bool AddBTChildTask(const FString& AssetPath, const FString& ParentNodeId, double X, double Y, FString& OutNodeId)
{
    if (!AddBTLooseTask(AssetPath, X, Y, OutNodeId))
    {
        return false;
    }

    FTestResponseCapture ConnectCapture;
    TSharedPtr<FJsonObject> ConnectPayload = MakeShared<FJsonObject>();
    ConnectPayload->SetStringField(TEXT("assetPath"), AssetPath);
    ConnectPayload->SetStringField(TEXT("parentNodeId"), ParentNodeId);
    ConnectPayload->SetStringField(TEXT("childNodeId"), OutNodeId);
    return InvokeBTOrderHandler(TEXT("behavior_tree.connect_nodes"), ConnectPayload, ConnectCapture);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBTDecompileEmitsResolvableNodeIdsTest,
    "PinWright.behavior_tree.decompile.EmitsResolvableNodeIds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBTDecompileEmitsResolvableNodeIdsTest::RunTest(const FString& Parameters)
{
    const FString AssetName = FString::Printf(TEXT("BT_NodeIds_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString SavePath = TEXT("/Game/McpTests/BehaviorTree");
    const FString PackagePath = SavePath / AssetName;
    CleanupTestAsset(PackagePath);
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackagePath);
    };

    FString AssetPath;
    FString SelectorNodeId;
    if (!TestTrue(TEXT("Behavior Tree fixture with a root composite created"),
        CreateBTWithRootComposite(AssetName, SavePath, AssetPath, SelectorNodeId)))
    {
        return false;
    }

    FString FirstTaskNodeId;
    FString SecondTaskNodeId;
    if (!TestTrue(TEXT("First child task added"), AddBTChildTask(AssetPath, SelectorNodeId, -200.0, 400.0, FirstTaskNodeId))
        || !TestTrue(TEXT("Second child task added"), AddBTChildTask(AssetPath, SelectorNodeId, 200.0, 400.0, SecondTaskNodeId)))
    {
        return false;
    }

    FTestResponseCapture DecompileCapture;
    TSharedPtr<FJsonObject> DecompilePayload = MakeShared<FJsonObject>();
    DecompilePayload->SetStringField(TEXT("assetPath"), AssetPath);
    if (!TestTrue(TEXT("behavior_tree.decompile succeeded"),
        InvokeBTOrderHandler(TEXT("behavior_tree.decompile"), DecompilePayload, DecompileCapture)))
    {
        return false;
    }

    FString Ir;
    TestTrue(TEXT("decompile result carries ir"), DecompileCapture.Result->TryGetStringField(TEXT("ir"), Ir));

    // The core fix: an id per emitted graph node, in the format add_node hands back.
    const TArray<FString> EmittedIds = ExtractBtirNodeIds(Ir);
    TestTrue(FString::Printf(TEXT("BTIR emits a nodeId per graph node (got %d, IR: %s)"), EmittedIds.Num(), *Ir),
        EmittedIds.Num() >= 3);
    TestTrue(TEXT("BTIR nodeId set contains the Selector id add_node returned"), EmittedIds.Contains(SelectorNodeId));
    TestTrue(TEXT("BTIR nodeId set contains the first task id add_node returned"), EmittedIds.Contains(FirstTaskNodeId));
    TestTrue(TEXT("BTIR nodeId set contains the second task id add_node returned"), EmittedIds.Contains(SecondTaskNodeId));

    if (EmittedIds.Num() == 0)
    {
        return false;
    }

    // The point of the emission: an id read out of the decompile TEXT is addressable. Uses the
    // scraped string, never the add_node return, so this is the "tree authored in another
    // session" path the ticket says is unreachable today.
    for (const FString& EmittedId : EmittedIds)
    {
        FTestResponseCapture SetCapture;
        TSharedPtr<FJsonObject> SetPayload = MakeShared<FJsonObject>();
        SetPayload->SetStringField(TEXT("assetPath"), AssetPath);
        SetPayload->SetStringField(TEXT("nodeId"), EmittedId);
        SetPayload->SetStringField(TEXT("comment"), TEXT("resolved from BTIR"));
        const bool bFound = InvokeHandlerWithCapture(TEXT("behavior_tree.set_node_properties"), SetPayload, SetCapture);
        TestTrue(TEXT("behavior_tree.set_node_properties handler found"), bFound);
        TestTrue(FString::Printf(TEXT("set_node_properties resolves BTIR nodeId '%s' (error: %s)"),
                *EmittedId, *SetCapture.ErrorCode),
            SetCapture.bSuccess);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBTSetChildOrderReordersCompositeChildrenTest,
    "PinWright.behavior_tree.set_child_order.ReordersCompositeChildren",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBTSetChildOrderReordersCompositeChildrenTest::RunTest(const FString& Parameters)
{
    const FString AssetName = FString::Printf(TEXT("BT_ChildOrder_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString SavePath = TEXT("/Game/McpTests/BehaviorTree");
    const FString PackagePath = SavePath / AssetName;
    CleanupTestAsset(PackagePath);
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackagePath);
    };

    FString AssetPath;
    FString SelectorNodeId;
    if (!TestTrue(TEXT("Behavior Tree fixture with a root composite created"),
        CreateBTWithRootComposite(AssetName, SavePath, AssetPath, SelectorNodeId)))
    {
        return false;
    }

    // Left task first, right task second: that is the execution order UE derives from X.
    FString LeftNodeId;
    FString RightNodeId;
    if (!TestTrue(TEXT("Left child task added"), AddBTChildTask(AssetPath, SelectorNodeId, -200.0, 400.0, LeftNodeId))
        || !TestTrue(TEXT("Right child task added"), AddBTChildTask(AssetPath, SelectorNodeId, 200.0, 400.0, RightNodeId)))
    {
        return false;
    }

    UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *AssetPath);
    UBehaviorTreeGraph* BTGraph = BT ? Cast<UBehaviorTreeGraph>(BT->BTGraph) : nullptr;
    TestNotNull(TEXT("Behavior Tree graph loaded"), BTGraph);
    UBehaviorTreeGraphNode* LeftGraphNode = FindBTOrderGraphNode(BTGraph, LeftNodeId);
    UBehaviorTreeGraphNode* RightGraphNode = FindBTOrderGraphNode(BTGraph, RightNodeId);
    TestNotNull(TEXT("Left child graph node located"), LeftGraphNode);
    TestNotNull(TEXT("Right child graph node located"), RightGraphNode);
    if (!BT || !BTGraph || !LeftGraphNode || !RightGraphNode)
    {
        return false;
    }

    // Baseline: connecting rebuilt the asset, so the runtime composite already runs left-then-right.
    UBTCompositeNode* RuntimeComposite = BT->RootNode;
    TestNotNull(TEXT("Runtime root composite exists"), RuntimeComposite);
    if (!RuntimeComposite || !TestEqual(TEXT("Runtime composite has both children"), RuntimeComposite->Children.Num(), 2))
    {
        return false;
    }
    TestTrue(TEXT("Baseline execution order runs the left task first"),
        static_cast<const UObject*>(RuntimeComposite->Children[0].ChildTask.Get()) == LeftGraphNode->NodeInstance.Get());

    // The ask: make the right task run first, on a tree that already exists.
    FTestResponseCapture OrderCapture;
    TSharedPtr<FJsonObject> OrderPayload = MakeShared<FJsonObject>();
    OrderPayload->SetStringField(TEXT("assetPath"), AssetPath);
    OrderPayload->SetStringField(TEXT("parentNodeId"), SelectorNodeId);
    TArray<TSharedPtr<FJsonValue>> RequestedOrder;
    RequestedOrder.Add(MakeShared<FJsonValueString>(RightNodeId));
    RequestedOrder.Add(MakeShared<FJsonValueString>(LeftNodeId));
    OrderPayload->SetArrayField(TEXT("childNodeIds"), RequestedOrder);
    if (!TestTrue(TEXT("behavior_tree.set_child_order succeeded"),
        InvokeBTOrderHandler(TEXT("behavior_tree.set_child_order"), OrderPayload, OrderCapture)))
    {
        return false;
    }

    // The echoed order is read off the runtime composite, so it is the readback the ticket asked for.
    const TArray<TSharedPtr<FJsonValue>>* ChildOrder = nullptr;
    TestTrue(TEXT("response carries childOrder"), OrderCapture.Result->TryGetArrayField(TEXT("childOrder"), ChildOrder));
    if (ChildOrder && TestEqual(TEXT("childOrder lists both children"), ChildOrder->Num(), 2))
    {
        const TSharedPtr<FJsonObject>* FirstEntry = nullptr;
        if ((*ChildOrder)[0].IsValid() && (*ChildOrder)[0]->TryGetObject(FirstEntry) && FirstEntry)
        {
            TestEqual(TEXT("childOrder reports the right task first"),
                (*FirstEntry)->GetStringField(TEXT("nodeId")), RightNodeId);
        }
    }

    // The proof: the runtime array the tree executes now starts with the task that was second.
    UBTCompositeNode* ReorderedComposite = BT->RootNode;
    TestNotNull(TEXT("Runtime root composite survives the reorder"), ReorderedComposite);
    if (!ReorderedComposite || !TestEqual(TEXT("Runtime composite still has both children"), ReorderedComposite->Children.Num(), 2))
    {
        return false;
    }
    TestTrue(TEXT("Right task now executes first"),
        static_cast<const UObject*>(ReorderedComposite->Children[0].ChildTask.Get()) == RightGraphNode->NodeInstance.Get());
    TestTrue(TEXT("Left task now executes second"),
        static_cast<const UObject*>(ReorderedComposite->Children[1].ChildTask.Get()) == LeftGraphNode->NodeInstance.Get());
    TestTrue(TEXT("Reordering moved the right task left of the left task in graph X"),
        RightGraphNode->NodePosX < LeftGraphNode->NodePosX);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBTSetChildOrderRefusesMultiOutputPinParentTest,
    "PinWright.behavior_tree.set_child_order.RefusesMultiOutputPinParent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBTSetChildOrderRefusesMultiOutputPinParentTest::RunTest(const FString& Parameters)
{
    const FString AssetName = FString::Printf(TEXT("BT_ParallelOrder_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString SavePath = TEXT("/Game/McpTests/BehaviorTree");
    const FString PackagePath = SavePath / AssetName;
    CleanupTestAsset(PackagePath);
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackagePath);
    };

    FString AssetPath;
    FString SelectorNodeId;
    if (!TestTrue(TEXT("Behavior Tree fixture with a root composite created"),
        CreateBTWithRootComposite(AssetName, SavePath, AssetPath, SelectorNodeId)))
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

    // behavior_tree.add_node builds every composite on UBehaviorTreeGraphNode_Composite, which has a
    // single output pin, so the two-pin shape cannot be produced through the RPC surface at all. Build
    // the real editor class: it is UCLASS() with no export macro, so resolve it by reflection and
    // construct through the exported UAIGraphNode base, the pattern the handlers already use.
    UClass* ParallelClass = FindObject<UClass>(nullptr,
        TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_SimpleParallel"));
    TestNotNull(TEXT("BehaviorTreeGraphNode_SimpleParallel class resolved"), ParallelClass);
    UBehaviorTreeGraphNode* ParallelNode = ParallelClass
        ? PinWright::BehaviorTree::CastGraphNode(
            NewObject<UAIGraphNode>(BTGraph, ParallelClass, NAME_None, RF_Transactional))
        : nullptr;
    TestNotNull(TEXT("Simple Parallel graph node created"), ParallelNode);
    if (!ParallelNode)
    {
        return false;
    }

    ParallelNode->CreateNewGuid();
    ParallelNode->NodePosX = 0;
    ParallelNode->NodePosY = 400;
    UAIGraphNode::UpdateNodeClassDataFrom(UBTComposite_SimpleParallel::StaticClass(), ParallelNode->ClassData);
    BTGraph->AddNode(ParallelNode, true, false);
    ParallelNode->PostPlacedNewNode();
    ParallelNode->AllocateDefaultPins();

    TArray<UEdGraphPin*> OutputPins;
    for (UEdGraphPin* Pin : ParallelNode->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Output)
        {
            OutputPins.Add(Pin);
        }
    }

    // Fixture guard: the defect only exists on a node with more than one output pin, so a fixture
    // that produced one pin would assert nothing while still going green on the refusal check.
    if (!TestEqual(TEXT("Simple Parallel node exposes two output pins ('Task' and 'Out')"), OutputPins.Num(), 2))
    {
        return false;
    }

    FString TaskPinChildId;
    FString OutPinChildId;
    if (!TestTrue(TEXT("Main-task child added"), AddBTLooseTask(AssetPath, -200.0, 600.0, TaskPinChildId))
        || !TestTrue(TEXT("Background child added"), AddBTLooseTask(AssetPath, 200.0, 600.0, OutPinChildId)))
    {
        return false;
    }

    UBehaviorTreeGraphNode* TaskPinChild = FindBTOrderGraphNode(BTGraph, TaskPinChildId);
    UBehaviorTreeGraphNode* OutPinChild = FindBTOrderGraphNode(BTGraph, OutPinChildId);
    TestNotNull(TEXT("Main-task child graph node located"), TaskPinChild);
    TestNotNull(TEXT("Background child graph node located"), OutPinChild);
    if (!TaskPinChild || !OutPinChild)
    {
        return false;
    }

    // One child per pin. connect_nodes only ever uses the FIRST output pin, so the cross-pin shape
    // this test is about has to be wired directly.
    OutputPins[0]->MakeLinkTo(TaskPinChild->GetInputPin());
    OutputPins[1]->MakeLinkTo(OutPinChild->GetInputPin());

    FTestResponseCapture OrderCapture;
    TSharedPtr<FJsonObject> OrderPayload = MakeShared<FJsonObject>();
    OrderPayload->SetStringField(TEXT("assetPath"), AssetPath);
    OrderPayload->SetStringField(TEXT("parentNodeId"), ParallelNode->NodeGuid.ToString());
    TArray<TSharedPtr<FJsonValue>> RequestedOrder;
    RequestedOrder.Add(MakeShared<FJsonValueString>(OutPinChildId));
    RequestedOrder.Add(MakeShared<FJsonValueString>(TaskPinChildId));
    OrderPayload->SetArrayField(TEXT("childNodeIds"), RequestedOrder);

    TestTrue(TEXT("behavior_tree.set_child_order handler found"),
        InvokeHandlerWithCapture(TEXT("behavior_tree.set_child_order"), OrderPayload, OrderCapture));

    // Without the guard this flattened cross-pin list validates as a permutation, the call reports
    // success, and the engine's per-pin sort leaves both children exactly where they were.
    TestFalse(TEXT("set_child_order refuses a two-output-pin parent"), OrderCapture.bSuccess);
    TestEqual(TEXT("refusal is INVALID_ARGUMENT"), OrderCapture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    if (OrderCapture.Result.IsValid())
    {
        double ReportedPinCount = 0.0;
        TestTrue(TEXT("refusal reports outputPinCount"),
            OrderCapture.Result->TryGetNumberField(TEXT("outputPinCount"), ReportedPinCount));
        TestEqual(TEXT("refusal names both output pins"), static_cast<int32>(ReportedPinCount), 2);
    }

    return true;
}
