// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "BTIR/BTIRDecompiler.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Tests/Assets/AssetDumpTestHelpers.h"
#include "Tests/TestUtils.h"


#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Decorators/BTDecorator_ForceSuccess.h"
#include "BehaviorTree/Tasks/BTTask_MoveTo.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "Handlers/AI/BehaviorTreeGraphNodeCompat.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "BehaviorTreeDecoratorGraph.h"
#include "BehaviorTreeDecoratorGraphNode_Decorator.h"
#include "BehaviorTreeDecoratorGraphNode_Logic.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "EdGraphSchema_BehaviorTreeDecorator.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace
{
    using AssetDumpTestHelpers::FindDumpFile;
    using AssetDumpTestHelpers::HasDumpFile;

    FString MakeUniqueBtirTestAssetName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    UBlackboardData* NewTransientBlackboard(FString& OutObjectPath)
    {
        const FString AssetName = MakeUniqueBtirTestAssetName(TEXT("BB_BTIR"));
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        UBlackboardData* Blackboard = NewObject<UBlackboardData>(
            Package,
            UBlackboardData::StaticClass(),
            *AssetName,
            RF_Public | RF_Standalone | RF_Transient);
        if (!Blackboard)
        {
            return nullptr;
        }

        FBlackboardEntry TargetEntry;
        TargetEntry.EntryName = TEXT("Target");
        UBlackboardKeyType_Object* ObjectKey = NewObject<UBlackboardKeyType_Object>(Blackboard);
        ObjectKey->BaseClass = AActor::StaticClass();
        TargetEntry.KeyType = ObjectKey;
        TargetEntry.bInstanceSynced = true;
        Blackboard->Keys.Add(TargetEntry);

        FBlackboardEntry PointEntry;
        PointEntry.EntryName = TEXT("PatrolPoint");
        PointEntry.KeyType = NewObject<UBlackboardKeyType_Vector>(Blackboard);
        Blackboard->Keys.Add(PointEntry);

        Blackboard->AddToRoot();
        OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        return Blackboard;
    }

    UBehaviorTreeGraphNode_Root* FindRootNode(UBehaviorTreeGraph* Graph)
    {
        if (!Graph)
        {
            return nullptr;
        }

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UBehaviorTreeGraphNode_Root* Root = Cast<UBehaviorTreeGraphNode_Root>(Node))
            {
                return Root;
            }
        }
        return nullptr;
    }

    template<typename GraphNodeT, typename NodeInstanceT>
    GraphNodeT* AddBtirTestNode(UBehaviorTree* BehaviorTree, UBehaviorTreeGraph* Graph, const TCHAR* Name, int32 X, int32 Y)
    {
        GraphNodeT* GraphNode = PinWright::BehaviorTree::NewNode<GraphNodeT>(Graph);
        if (!GraphNode)
        {
            return nullptr;
        }

        GraphNode->CreateNewGuid();
        GraphNode->NodePosX = X;
        GraphNode->NodePosY = Y;
        GraphNode->NodeInstance = NewObject<NodeInstanceT>(BehaviorTree, FName(Name), RF_Transactional);
        Graph->AddNode(GraphNode, true, false);
        GraphNode->AllocateDefaultPins();
        return GraphNode;
    }

    UBehaviorTreeGraphNode* AddBtirTestDecorator(
        UBehaviorTree* BehaviorTree,
        UBehaviorTreeGraph* Graph,
        UBehaviorTreeGraphNode* ParentNode,
        const TCHAR* Name)
    {
        UBehaviorTreeGraphNode* GraphNode =
            PinWright::BehaviorTree::NewDecorator(Graph);
        if (!GraphNode)
        {
            return nullptr;
        }

        GraphNode->CreateNewGuid();
        GraphNode->NodeInstance = NewObject<UBTDecorator_ForceSuccess>(BehaviorTree, FName(Name), RF_Transactional);
        GraphNode->ParentNode = ParentNode;
        Graph->AddNode(GraphNode, true, false);
        GraphNode->AllocateDefaultPins();
        if (ParentNode)
        {
            ParentNode->Decorators.Add(GraphNode);
        }
        return GraphNode;
    }

    UBehaviorTreeGraphNode_CompositeDecorator* AddBtirTestCompositeDecorator(
        UBehaviorTree* BehaviorTree,
        UBehaviorTreeGraph* Graph,
        UBehaviorTreeGraphNode* ParentNode,
        const TCHAR* Name)
    {
        UBehaviorTreeGraphNode_CompositeDecorator* CompositeNode =
            NewObject<UBehaviorTreeGraphNode_CompositeDecorator>(Graph, NAME_None, RF_Transactional);
        if (!CompositeNode)
        {
            return nullptr;
        }

        CompositeNode->CreateNewGuid();
        CompositeNode->CompositeName = Name;
        CompositeNode->ParentNode = ParentNode;
        Graph->AddNode(CompositeNode, true, false);
        if (ParentNode)
        {
            ParentNode->Decorators.Add(CompositeNode);
        }

        UBehaviorTreeDecoratorGraph* DecoratorGraph =
            NewObject<UBehaviorTreeDecoratorGraph>(CompositeNode, TEXT("CompositeDecorator"), RF_Transient);
        if (!DecoratorGraph)
        {
            return CompositeNode;
        }

        DecoratorGraph->Schema = UEdGraphSchema_BehaviorTreeDecorator::StaticClass();
        CompositeNode->BoundGraph = DecoratorGraph;
        DecoratorGraph->GetSchema()->CreateDefaultNodesForGraph(*DecoratorGraph);

        // UBehaviorTreeDecoratorGraphNode_Logic and _Decorator have no export-API macro in BehaviorTreeEditor,
        // so their GetPrivateStaticClass symbols are not exported. Resolve via reflection to avoid link errors.
        UClass* LogicNodeClass = FindObject<UClass>(nullptr, TEXT("/Script/BehaviorTreeEditor.BehaviorTreeDecoratorGraphNode_Logic"));
        UClass* DecoratorNodeClass = FindObject<UClass>(nullptr, TEXT("/Script/BehaviorTreeEditor.BehaviorTreeDecoratorGraphNode_Decorator"));

        UBehaviorTreeDecoratorGraphNode_Logic* SinkNode = nullptr;
        for (UEdGraphNode* Node : DecoratorGraph->Nodes)
        {
            // Use the runtime UClass* for the IsA check; static_cast is safe for member access once confirmed.
            UBehaviorTreeDecoratorGraphNode_Logic* LogicNode =
                (LogicNodeClass && Node && Node->IsA(LogicNodeClass))
                    ? static_cast<UBehaviorTreeDecoratorGraphNode_Logic*>(Node)
                    : nullptr;
            if (LogicNode && LogicNode->LogicMode == EDecoratorLogicMode::Sink)
            {
                SinkNode = LogicNode;
                break;
            }
        }

        // DecoratorNodeClass was already resolved above; use it here to avoid the unexported static-class symbol.
        UBehaviorTreeDecoratorGraphNode_Decorator* InnerDecoratorNode =
            DecoratorNodeClass
                ? static_cast<UBehaviorTreeDecoratorGraphNode_Decorator*>(
                    NewObject<UObject>(DecoratorGraph, DecoratorNodeClass, NAME_None, RF_Transactional))
                : nullptr;
        if (!SinkNode || !InnerDecoratorNode)
        {
            return CompositeNode;
        }

        InnerDecoratorNode->CreateNewGuid();
        InnerDecoratorNode->NodeInstance =
            NewObject<UBTDecorator_ForceSuccess>(BehaviorTree, TEXT("InnerForceSuccess"), RF_Transactional);
        DecoratorGraph->AddNode(InnerDecoratorNode, true, false);
        InnerDecoratorNode->AllocateDefaultPins();
        InnerDecoratorNode->GetOutputPin()->MakeLinkTo(SinkNode->GetInputPin());
        return CompositeNode;
    }

    UBehaviorTree* NewTransientBehaviorTree(FString& OutObjectPath, UBlackboardData*& OutBlackboard)
    {
        FString BlackboardPath;
        OutBlackboard = NewTransientBlackboard(BlackboardPath);
        if (!OutBlackboard)
        {
            return nullptr;
        }

        const FString AssetName = MakeUniqueBtirTestAssetName(TEXT("BT_BTIR"));
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        UBehaviorTree* BehaviorTree = NewObject<UBehaviorTree>(
            Package,
            UBehaviorTree::StaticClass(),
            *AssetName,
            RF_Public | RF_Standalone | RF_Transient);
        if (!BehaviorTree)
        {
            return nullptr;
        }

        BehaviorTree->BlackboardAsset = OutBlackboard;

        UBehaviorTreeGraph* Graph = NewObject<UBehaviorTreeGraph>(BehaviorTree, TEXT("BehaviorTree"), RF_Transient);
        Graph->Schema = UEdGraphSchema_BehaviorTree::StaticClass();
        BehaviorTree->BTGraph = Graph;
        Graph->GetSchema()->CreateDefaultNodesForGraph(*Graph);

        UBehaviorTreeGraphNode_Root* RootNode = FindRootNode(Graph);
        if (RootNode)
        {
            RootNode->BlackboardAsset = OutBlackboard;
        }

        UBehaviorTreeGraphNode_Composite* SequenceNode =
            AddBtirTestNode<UBehaviorTreeGraphNode_Composite, UBTComposite_Sequence>(
                BehaviorTree, Graph, TEXT("Sequence"), -200, 120);
        UBehaviorTreeGraphNode_Task* MoveToNode =
            AddBtirTestNode<UBehaviorTreeGraphNode_Task, UBTTask_MoveTo>(
                BehaviorTree, Graph, TEXT("MoveToTarget"), -200, 300);

        if (UBTTask_MoveTo* MoveTo = MoveToNode ? Cast<UBTTask_MoveTo>(MoveToNode->NodeInstance.Get()) : nullptr)
        {
            FStructProperty* BlackboardKeyProperty =
                CastField<FStructProperty>(MoveTo->GetClass()->FindPropertyByName(TEXT("BlackboardKey")));
            if (BlackboardKeyProperty && BlackboardKeyProperty->Struct == FBlackboardKeySelector::StaticStruct())
            {
                FBlackboardKeySelector* BlackboardKey =
                    BlackboardKeyProperty->ContainerPtrToValuePtr<FBlackboardKeySelector>(MoveTo);
                if (BlackboardKey)
                {
                    BlackboardKey->SelectedKeyName = TEXT("Target");
                    BlackboardKey->ResolveSelectedKey(*OutBlackboard);
                }
            }
        }
        if (MoveToNode)
        {
            MoveToNode->NodeComment = TEXT("Move to current target");
        }

        if (RootNode && SequenceNode)
        {
            RootNode->GetOutputPin()->MakeLinkTo(SequenceNode->GetInputPin());
        }
        if (SequenceNode && MoveToNode)
        {
            SequenceNode->GetOutputPin()->MakeLinkTo(MoveToNode->GetInputPin());
        }

        if (UBehaviorTreeGraphNode* DecoratorNode =
            AddBtirTestDecorator(BehaviorTree, Graph, SequenceNode, TEXT("ForceSuccess")))
        {
            BehaviorTree->RootDecorators.Add(Cast<UBTDecorator>(DecoratorNode->NodeInstance.Get()));
            BehaviorTree->RootDecoratorOps.Add(FBTDecoratorLogic(static_cast<uint8>(EBTDecoratorLogic::Not), 1));
            BehaviorTree->RootDecoratorOps.Add(FBTDecoratorLogic(static_cast<uint8>(EBTDecoratorLogic::Test), 0));
        }

        BehaviorTree->AddToRoot();
        OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        return BehaviorTree;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBehaviorTreeBtirRpcAndDumpParityTest,
    "PinWright.behavior_tree.decompile.BtirRpcAndDumpParity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBehaviorTreeBtirRpcAndDumpParityTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UBlackboardData* Blackboard = nullptr;
    UBehaviorTree* BehaviorTree = NewTransientBehaviorTree(ObjectPath, Blackboard);
    TestNotNull(TEXT("Transient Behavior Tree created"), BehaviorTree);
    if (!BehaviorTree)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    FTestResponseCapture Capture;
    TestTrue(TEXT("behavior_tree.decompile handler found"),
        InvokeHandlerWithCapture(TEXT("behavior_tree.decompile"), Payload, Capture));
    TestTrue(TEXT("behavior_tree.decompile succeeds"), Capture.bSuccess);
    TestTrue(TEXT("behavior_tree.decompile result exists"), Capture.Result.IsValid());

    FString RpcIr;
    if (Capture.Result.IsValid())
    {
        RpcIr = Capture.Result->GetStringField(TEXT("ir"));
        const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
        TestTrue(TEXT("response contains warnings array"), Capture.Result->TryGetArrayField(TEXT("warnings"), Warnings));
    }

    const FString ScratchRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir())
        / TEXT("BTIRDumpTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const AssetDumpHandler::FDumpSingleResult DumpResult =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot, /*bDiff=*/false);

    TestTrue(TEXT("asset.dump succeeds for transient Behavior Tree"), DumpResult.ErrorCode.IsEmpty());
    TestTrue(TEXT("properties.json remains present"), HasDumpFile(DumpResult.WrittenPaths, DumpFileNames::Properties));
    TestTrue(TEXT("btir.txt is written"), HasDumpFile(DumpResult.WrittenPaths, DumpFileNames::Btir));

    FString DumpIr;
    const FString BtirPath = FindDumpFile(DumpResult.WrittenPaths, DumpFileNames::Btir);
    TestTrue(TEXT("btir.txt loads"), FFileHelper::LoadFileToString(DumpIr, *BtirPath));
    TestEqual(TEXT("RPC and asset dump share BTIR text"), RpcIr, DumpIr);
    TestTrue(TEXT("BTIR contains behavior_tree"), DumpIr.Contains(TEXT("behavior_tree")));
    TestTrue(TEXT("BTIR contains blackboard"), DumpIr.Contains(TEXT("blackboard")));
    TestTrue(TEXT("BTIR contains composite line"), DumpIr.Contains(TEXT("composite")));
    TestTrue(TEXT("BTIR contains task line"), DumpIr.Contains(TEXT("task")));
    TestTrue(TEXT("BTIR contains graph position"), DumpIr.Contains(TEXT("@(")));
    TestTrue(TEXT("BTIR contains explicit decorator logic"), DumpIr.Contains(TEXT("decorator_logic [Not:1, Test:0]")));
    TestTrue(TEXT("BTIR contains graph node comment"), DumpIr.Contains(TEXT("comment: \"Move to current target\"")));

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    BehaviorTree->RemoveFromRoot();
    if (Blackboard)
    {
        Blackboard->RemoveFromRoot();
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlackboardBtirStandaloneDumpTest,
    "PinWright.behavior_tree.decompile.BlackboardStandaloneDump",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlackboardBtirStandaloneDumpTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UBlackboardData* Blackboard = NewTransientBlackboard(ObjectPath);
    TestNotNull(TEXT("Transient Blackboard created"), Blackboard);
    if (!Blackboard)
    {
        return false;
    }

    const FString ScratchRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir())
        / TEXT("BTIRBlackboardDumpTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const AssetDumpHandler::FDumpSingleResult DumpResult =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot, /*bDiff=*/false);

    TestTrue(TEXT("asset.dump succeeds for transient Blackboard"), DumpResult.ErrorCode.IsEmpty());
    TestTrue(TEXT("properties.json remains present"), HasDumpFile(DumpResult.WrittenPaths, DumpFileNames::Properties));
    TestTrue(TEXT("btir.txt is written"), HasDumpFile(DumpResult.WrittenPaths, DumpFileNames::Btir));

    FString DumpIr;
    const FString BtirPath = FindDumpFile(DumpResult.WrittenPaths, DumpFileNames::Btir);
    TestTrue(TEXT("btir.txt loads"), FFileHelper::LoadFileToString(DumpIr, *BtirPath));
    TestTrue(TEXT("BTIR contains blackboard"), DumpIr.Contains(TEXT("blackboard")));
    TestTrue(TEXT("BTIR contains Object key"), DumpIr.Contains(TEXT("key Object")));
    TestTrue(TEXT("BTIR contains Vector key"), DumpIr.Contains(TEXT("key Vector")));
    TestTrue(TEXT("BTIR contains instanceSynced"), DumpIr.Contains(TEXT("instanceSynced")));

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    Blackboard->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBehaviorTreeBtirEmitsUnreachableGraphNodesTest,
    "PinWright.behavior_tree.decompile.EmitsUnreachableGraphNodes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBehaviorTreeBtirEmitsUnreachableGraphNodesTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UBlackboardData* Blackboard = nullptr;
    UBehaviorTree* BehaviorTree = NewTransientBehaviorTree(ObjectPath, Blackboard);
    TestNotNull(TEXT("Transient Behavior Tree created"), BehaviorTree);
    if (!BehaviorTree)
    {
        return false;
    }

    UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(BehaviorTree->BTGraph);
    TestNotNull(TEXT("Transient Behavior Tree graph created"), Graph);
    if (!Graph)
    {
        BehaviorTree->RemoveFromRoot();
        if (Blackboard)
        {
            Blackboard->RemoveFromRoot();
        }
        return false;
    }

    AddBtirTestNode<UBehaviorTreeGraphNode_Task, UBTTask_MoveTo>(
        BehaviorTree, Graph, TEXT("UnreachableMoveTo"), 300, 300);

    const FBTIRResult Result = BTIRDecompiler::BuildBehaviorTreeIrText(BehaviorTree);
    TestTrue(TEXT("BTIR build succeeds"), Result.bSuccess);
    TestTrue(TEXT("BTIR emits unreachable graph node as orphan"), Result.Text.Contains(TEXT("orphan task")));

    bool bFoundUnreachableWarning = false;
    for (const FString& Warning : Result.Warnings)
    {
        if (Warning.Contains(TEXT("unreachable from root")))
        {
            bFoundUnreachableWarning = true;
            break;
        }
    }
    TestTrue(TEXT("BTIR warns about unreachable graph node"), bFoundUnreachableWarning);

    BehaviorTree->RemoveFromRoot();
    if (Blackboard)
    {
        Blackboard->RemoveFromRoot();
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBehaviorTreeBtirCompositeDecoratorEmitsInnerConditionsTest,
    "PinWright.behavior_tree.decompile.CompositeDecoratorEmitsInnerConditions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBehaviorTreeBtirCompositeDecoratorEmitsInnerConditionsTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UBlackboardData* Blackboard = nullptr;
    UBehaviorTree* BehaviorTree = NewTransientBehaviorTree(ObjectPath, Blackboard);
    TestNotNull(TEXT("Transient Behavior Tree created"), BehaviorTree);
    if (!BehaviorTree)
    {
        return false;
    }

    UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(BehaviorTree->BTGraph);
    UBehaviorTreeGraphNode_Root* RootNode = FindRootNode(Graph);
    TestNotNull(TEXT("Transient Behavior Tree graph created"), Graph);
    TestNotNull(TEXT("Transient Behavior Tree root node created"), RootNode);
    if (!Graph || !RootNode)
    {
        BehaviorTree->RemoveFromRoot();
        if (Blackboard)
        {
            Blackboard->RemoveFromRoot();
        }
        return false;
    }

    UBehaviorTreeGraphNode_CompositeDecorator* CompositeDecorator =
        AddBtirTestCompositeDecorator(BehaviorTree, Graph, RootNode, TEXT("CompositeCheck"));
    TestNotNull(TEXT("Composite decorator created"), CompositeDecorator);

    // Fixture guard. AddBtirTestCompositeDecorator returns CompositeNode *unbuilt* at two
    // bail-outs — no bound decorator graph, and no sink node / no inner decorator node — so a
    // silently no-op fixture is indistinguishable from success downstream: the decompiler just
    // takes the empty-body early-out and still emits the header. CollectDecoratorData is the
    // exact call EmitCompositeDecoratorNode makes, so asserting on it here pins the fixture
    // against the same API the production path consumes.
    TArray<UBTDecorator*> FixtureInnerDecorators;
    TArray<FBTDecoratorLogic> FixtureInnerOperations;
    if (CompositeDecorator)
    {
        CompositeDecorator->CollectDecoratorData(FixtureInnerDecorators, FixtureInnerOperations);
    }
    const bool bFixtureWiredInnerDecorator = FixtureInnerDecorators.Num() == 1;
    TestTrue(TEXT("Fixture wired exactly one inner decorator into the composite decorator graph"),
        bFixtureWiredInnerDecorator);

    const FBTIRResult Result = BTIRDecompiler::BuildBehaviorTreeIrText(BehaviorTree);
    TestTrue(TEXT("BTIR build succeeds"), Result.bSuccess);
    TestTrue(TEXT("BTIR emits composite decorator wrapper"),
        Result.Text.Contains(TEXT("decorator composite CompositeCheck")));

    // Structural assertion, not a substring probe.
    //
    // A bare Contains("ForceSuccess") cannot fail here: the shared NewTransientBehaviorTree
    // fixture attaches a SEPARATE UBTDecorator_ForceSuccess to the Sequence node, and
    // "ForceSuccess" is the decompiler's TYPE token for any such decorator (GetShortTypeName
    // strips through the first underscore of BTDecorator_ForceSuccess). Nor can the object name
    // "InnerForceSuccess" be probed: NodeNameToken emits UBTNode::GetNodeName(), which returns
    // the class's own NodeName — "Force Success" — and the UObject name never reaches the text.
    //
    // So assert the emitted BLOCK instead: the composite header must open a brace, the very next
    // line must be a strictly deeper-indented decorator instance, and the block must close at the
    // header's indent. Deleting the inner-decorator loop in EmitCompositeDecoratorNode takes the
    // empty-body early-out, which emits a bare header with no brace and no child line — and that
    // is what these three assertions catch.
    TArray<FString> IrLines;
    Result.Text.ParseIntoArrayLines(IrLines, /*InCullEmpty=*/false);

    const auto LeadingSpaces = [](const FString& Line)
    {
        int32 Count = 0;
        while (Count < Line.Len() && Line[Count] == TEXT(' '))
        {
            ++Count;
        }
        return Count;
    };

    int32 HeaderLineIndex = INDEX_NONE;
    for (int32 LineIndex = 0; LineIndex < IrLines.Num(); ++LineIndex)
    {
        if (IrLines[LineIndex].Contains(TEXT("decorator composite CompositeCheck")))
        {
            HeaderLineIndex = LineIndex;
            break;
        }
    }
    TestTrue(TEXT("BTIR composite decorator header line located"), HeaderLineIndex != INDEX_NONE);

    if (bFixtureWiredInnerDecorator && HeaderLineIndex != INDEX_NONE)
    {
        const FString& HeaderLine = IrLines[HeaderLineIndex];
        TestTrue(FString::Printf(TEXT("BTIR composite header opens a block (line: %s)"), *HeaderLine),
            HeaderLine.TrimEnd().EndsWith(TEXT("{")));

        const int32 HeaderIndent = LeadingSpaces(HeaderLine);
        const int32 InnerLineIndex = HeaderLineIndex + 1;
        TestTrue(TEXT("BTIR composite block has a line after the header"), IrLines.IsValidIndex(InnerLineIndex));
        if (IrLines.IsValidIndex(InnerLineIndex))
        {
            const FString& InnerLine = IrLines[InnerLineIndex];
            TestTrue(FString::Printf(TEXT("BTIR inner condition is nested under the composite (line: %s)"), *InnerLine),
                LeadingSpaces(InnerLine) > HeaderIndent);
            TestTrue(FString::Printf(TEXT("BTIR emits inner decorator condition (line: %s)"), *InnerLine),
                InnerLine.TrimStart().StartsWith(TEXT("decorator ForceSuccess ")));
        }

        const FString ExpectedClose = FString::ChrN(HeaderIndent, TEXT(' ')) + TEXT("}");
        bool bFoundBlockClose = false;
        for (int32 LineIndex = HeaderLineIndex + 1; LineIndex < IrLines.Num(); ++LineIndex)
        {
            if (IrLines[LineIndex] == ExpectedClose)
            {
                bFoundBlockClose = true;
                break;
            }
            if (LeadingSpaces(IrLines[LineIndex]) <= HeaderIndent)
            {
                break;
            }
        }
        TestTrue(TEXT("BTIR composite block closes at the header indent"), bFoundBlockClose);
    }

    BehaviorTree->RemoveFromRoot();
    if (Blackboard)
    {
        Blackboard->RemoveFromRoot();
    }
    return true;
}
