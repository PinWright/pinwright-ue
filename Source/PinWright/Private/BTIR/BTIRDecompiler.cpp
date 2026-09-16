// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "BTIR/BTIRDecompiler.h"


#include "IrCore/IrTextUtils.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "Handlers/AI/BehaviorTreeGraphNodeCompat.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "EdGraph/EdGraphPin.h"
#include "UObject/UnrealType.h"

namespace
{
    FString BTIRIndent(int32 Depth)
    {
        return FString::ChrN(Depth * 2, TEXT(' '));
    }

    FString ObjectPathToken(const UObject* Object)
    {
        return Object ? FIrTextUtils::FormatNameToken(Object->GetPathName()) : FString();
    }

    FString NodePositionSuffix(const UEdGraphNode* Node)
    {
        return Node
            ? FIrTextUtils::FormatPositionSuffix(Node->NodePosX, Node->NodePosY)
            : FString();
    }

    FString NodeKindToken(const UBTNode* Node)
    {
        if (Node && Node->IsA<UBTCompositeNode>())
        {
            return TEXT("composite");
        }
        if (Node && Node->IsA<UBTTaskNode>())
        {
            return TEXT("task");
        }
        if (Node && Node->IsA<UBTDecorator>())
        {
            return TEXT("decorator");
        }
        if (Node && Node->IsA<UBTService>())
        {
            return TEXT("service");
        }
        return TEXT("node");
    }

    FString NodeTypeToken(const UBTNode* Node)
    {
        if (!Node)
        {
            return TEXT("null");
        }

        UClass* NodeClass = Node->GetClass();
        if (NodeClass && NodeClass->HasAnyClassFlags(CLASS_CompiledFromBlueprint))
        {
            return FIrTextUtils::FormatNameToken(NodeClass->GetPathName());
        }

        return FIrTextUtils::FormatNameToken(UBehaviorTreeTypes::GetShortTypeName(Node));
    }

    FString NodeNameToken(const UBTNode* Node, const UEdGraphNode* GraphNode)
    {
        if (Node)
        {
            const FString NodeName = Node->GetNodeName();
            if (!NodeName.IsEmpty())
            {
                return FIrTextUtils::FormatNameToken(NodeName);
            }
        }

        return FIrTextUtils::FormatNameToken(GraphNode ? GraphNode->GetName() : TEXT("None"));
    }

    bool IsBlackboardKeySelectorStruct(const FStructProperty* StructProperty)
    {
        return StructProperty && StructProperty->Struct == FBlackboardKeySelector::StaticStruct();
    }

    // Per-class cache of the FBlackboardKeySelector members of a node class. Values are
    // raw FStructProperty*, so a collect that frees a Blueprint-generated node class
    // leaves them dangling; BTIRDecompiler::ClearPropertyCaches drops the whole map and
    // must be called before any such collect.
    TMap<UClass*, TArray<FStructProperty*>>& GetBlackboardSelectorCache()
    {
        static TMap<UClass*, TArray<FStructProperty*>> CachedByClass;
        return CachedByClass;
    }

    const TArray<FStructProperty*>& GetBlackboardSelectorProperties(UClass* Class)
    {
        TMap<UClass*, TArray<FStructProperty*>>& CachedByClass = GetBlackboardSelectorCache();
        static const TArray<FStructProperty*> EmptyProperties;

        if (!Class)
        {
            return EmptyProperties;
        }

        if (const TArray<FStructProperty*>* CachedProperties = CachedByClass.Find(Class))
        {
            return *CachedProperties;
        }

        TArray<FStructProperty*> Properties;
        for (TFieldIterator<FStructProperty> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
        {
            FStructProperty* StructProperty = *It;
            if (IsBlackboardKeySelectorStruct(StructProperty))
            {
                Properties.Add(StructProperty);
            }
        }

        return CachedByClass.Add(Class, MoveTemp(Properties));
    }

    void AppendBlackboardSelectorFields(UObject* Instance, TSet<FName>& ExplicitProperties, TArray<FString>& OutFields)
    {
        if (!Instance)
        {
            return;
        }

        for (FStructProperty* StructProperty : GetBlackboardSelectorProperties(Instance->GetClass()))
        {
            const FBlackboardKeySelector* Selector =
                StructProperty->ContainerPtrToValuePtr<FBlackboardKeySelector>(Instance);
            if (Selector && !Selector->SelectedKeyName.IsNone())
            {
                OutFields.Add(FString::Printf(
                    TEXT("%s: %s"),
                    *StructProperty->GetName(),
                    *FIrTextUtils::FormatNameToken(Selector->SelectedKeyName.ToString())));
                ExplicitProperties.Add(StructProperty->GetFName());
            }
        }
    }

    TArray<FString> BuildNodeFields(UObject* Instance)
    {
        TArray<FString> Fields;
        TSet<FName> ExplicitProperties;
        AppendBlackboardSelectorFields(Instance, ExplicitProperties, Fields);

        UObject* DefaultObject = Instance ? Instance->GetClass()->GetDefaultObject() : nullptr;
        FReflectedFieldEmitOptions Options;
        Options.FieldSeparator = TEXT(": ");
        Options.bEmitArraysAsBracketList = true;

        FIrTextUtils::AppendReflectedFields(
            Instance ? Instance->GetClass() : nullptr,
            Instance,
            DefaultObject,
            Instance,
            ExplicitProperties,
            [](const FStructProperty* StructProperty)
            {
                return IsBlackboardKeySelectorStruct(StructProperty);
            },
            [](FName PropertyName)
            {
                return PropertyName == FName(TEXT("NodeName"));
            },
            Options,
            Fields);

        return Fields;
    }

    TArray<FString> BuildGraphNodeFields(UObject* Instance, const UEdGraphNode* GraphNode)
    {
        TArray<FString> Fields;
        // Leads the field list because it is the only token on the line that any other verb
        // consumes: every nodeId-keyed behavior_tree verb resolves this exact string through
        // FindBTGraphNode, so without it a decompiled tree is readable but not editable.
        // FGuid::ToString()'s default Digits form is byte-identical to what add_node returns.
        if (GraphNode && GraphNode->NodeGuid.IsValid())
        {
            Fields.Add(FString::Printf(TEXT("nodeId: %s"), *GraphNode->NodeGuid.ToString()));
        }
        Fields.Append(BuildNodeFields(Instance));
        if (GraphNode && !GraphNode->NodeComment.IsEmpty())
        {
            Fields.Add(FString::Printf(TEXT("comment: %s"), *FIrTextUtils::Quote(GraphNode->NodeComment)));
        }
        return Fields;
    }

    FString DecoratorLogicOperationToken(EBTDecoratorLogic::Type Operation)
    {
        switch (Operation)
        {
        case EBTDecoratorLogic::Test:
            return TEXT("Test");
        case EBTDecoratorLogic::And:
            return TEXT("And");
        case EBTDecoratorLogic::Or:
            return TEXT("Or");
        case EBTDecoratorLogic::Not:
            return TEXT("Not");
        default:
            return TEXT("Invalid");
        }
    }

    bool IsImplicitAndDecoratorLogic(const TArray<FBTDecoratorLogic>& DecoratorOps, int32 DecoratorCount)
    {
        if (DecoratorOps.Num() != DecoratorCount)
        {
            return false;
        }

        for (int32 Index = 0; Index < DecoratorOps.Num(); ++Index)
        {
            if (DecoratorOps[Index].Operation != EBTDecoratorLogic::Test
                || DecoratorOps[Index].Number != Index)
            {
                return false;
            }
        }

        return true;
    }

    void AppendDecoratorLogicLine(
        const TArray<FBTDecoratorLogic>& DecoratorOps,
        int32 DecoratorCount,
        int32 Depth,
        TArray<FString>& Lines)
    {
        if (DecoratorOps.IsEmpty() || IsImplicitAndDecoratorLogic(DecoratorOps, DecoratorCount))
        {
            return;
        }

        TArray<FString> Tokens;
        Tokens.Reserve(DecoratorOps.Num());
        for (const FBTDecoratorLogic& Operation : DecoratorOps)
        {
            Tokens.Add(FString::Printf(
                TEXT("%s:%d"),
                *DecoratorLogicOperationToken(Operation.Operation),
                Operation.Number));
        }

        Lines.Add(FString::Printf(
            TEXT("%sdecorator_logic [%s]"),
            *BTIRIndent(Depth),
            *FString::Join(Tokens, TEXT(", "))));
    }

    int32 FindCompositeChildIndex(UBehaviorTreeGraphNode* ParentGraphNode, UBehaviorTreeGraphNode* ChildGraphNode)
    {
        UBTCompositeNode* ParentComposite = ParentGraphNode
            ? Cast<UBTCompositeNode>(ParentGraphNode->NodeInstance.Get())
            : nullptr;
        UBTNode* ChildNode = ChildGraphNode
            ? Cast<UBTNode>(ChildGraphNode->NodeInstance.Get())
            : nullptr;
        if (!ParentComposite || !ChildNode)
        {
            return INDEX_NONE;
        }

        for (int32 Index = 0; Index < ParentComposite->Children.Num(); ++Index)
        {
            const FBTCompositeChild& Child = ParentComposite->Children[Index];
            if (Child.ChildComposite.Get() == ChildNode || Child.ChildTask.Get() == ChildNode)
            {
                return Index;
            }
        }

        return INDEX_NONE;
    }

    TArray<FBTDecoratorLogic> FindDecoratorLogicForAttachment(
        UBehaviorTree* BehaviorTree,
        UBehaviorTreeGraphNode* ParentGraphNode,
        UBehaviorTreeGraphNode* ChildGraphNode)
    {
        if (!ParentGraphNode)
        {
            return {};
        }

        if (ParentGraphNode->IsA<UBehaviorTreeGraphNode_Root>())
        {
            return BehaviorTree ? BehaviorTree->RootDecoratorOps : TArray<FBTDecoratorLogic>();
        }

        UBTCompositeNode* ParentComposite = Cast<UBTCompositeNode>(ParentGraphNode->NodeInstance.Get());
        const int32 ChildIndex = FindCompositeChildIndex(ParentGraphNode, ChildGraphNode);
        if (ParentComposite && ParentComposite->Children.IsValidIndex(ChildIndex))
        {
            return ParentComposite->Children[ChildIndex].DecoratorOps;
        }

        return {};
    }

    TArray<UBehaviorTreeGraphNode*> LinkedChildNodes(UBehaviorTreeGraphNode* Node)
    {
        TArray<UBehaviorTreeGraphNode*> Children;
        if (!Node)
        {
            return Children;
        }

        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Output)
            {
                continue;
            }

            for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
            {
                UBehaviorTreeGraphNode* Child = LinkedPin
                    ? PinWright::BehaviorTree::CastGraphNode(LinkedPin->GetOwningNode())
                    : nullptr;
                if (Child && !Child->IsSubNode())
                {
                    Children.Add(Child);
                }
            }
        }

        Children.Sort([](const UBehaviorTreeGraphNode& A, const UBehaviorTreeGraphNode& B)
        {
            if (A.NodePosX != B.NodePosX)
            {
                return A.NodePosX < B.NodePosX;
            }
            return A.NodePosY < B.NodePosY;
        });
        return Children;
    }

    TArray<UBehaviorTreeGraphNode*> BehaviorGraphNodes(UBehaviorTree* BehaviorTree)
    {
        TArray<UBehaviorTreeGraphNode*> Nodes;
        UBehaviorTreeGraph* Graph = BehaviorTree ? Cast<UBehaviorTreeGraph>(BehaviorTree->BTGraph) : nullptr;
        if (!Graph)
        {
            return Nodes;
        }

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UBehaviorTreeGraphNode* BehaviorNode = PinWright::BehaviorTree::CastGraphNode(Node);
            if (BehaviorNode && !BehaviorNode->IsA<UBehaviorTreeGraphNode_Root>() && !BehaviorNode->IsSubNode())
            {
                Nodes.Add(BehaviorNode);
            }
        }

        Nodes.Sort([](const UBehaviorTreeGraphNode& A, const UBehaviorTreeGraphNode& B)
        {
            if (A.NodePosX != B.NodePosX)
            {
                return A.NodePosX < B.NodePosX;
            }
            return A.NodePosY < B.NodePosY;
        });
        return Nodes;
    }

    void EmitAuxInstance(UBTNode* Node, const UEdGraphNode* GraphNode, int32 Depth, TArray<FString>& Lines)
    {
        const FString Header = FString::Printf(
            TEXT("%s%s %s %s%s %s"),
            *BTIRIndent(Depth),
            *NodeKindToken(Node),
            *NodeTypeToken(Node),
            *NodeNameToken(Node, GraphNode),
            *NodePositionSuffix(GraphNode),
            *FIrTextUtils::FormatFieldList(BuildGraphNodeFields(Node, GraphNode)));
        Lines.Add(Header);
    }

    void EmitCompositeDecoratorNode(
        UBehaviorTreeGraphNode_CompositeDecorator* CompositeDecorator,
        int32 Depth,
        TArray<FString>& Lines)
    {
        TArray<UBTDecorator*> InnerDecorators;
        TArray<FBTDecoratorLogic> InnerOperations;
        if (CompositeDecorator)
        {
            CompositeDecorator->CollectDecoratorData(InnerDecorators, InnerOperations);
        }

        const FString Name = CompositeDecorator && !CompositeDecorator->CompositeName.IsEmpty()
            ? CompositeDecorator->CompositeName
            : (CompositeDecorator ? CompositeDecorator->GetName() : TEXT("CompositeDecorator"));
        const FString Header = FString::Printf(
            TEXT("%sdecorator composite %s%s %s"),
            *BTIRIndent(Depth),
            *FIrTextUtils::FormatNameToken(Name),
            *NodePositionSuffix(CompositeDecorator),
            *FIrTextUtils::FormatFieldList(BuildGraphNodeFields(nullptr, CompositeDecorator)));

        if (InnerDecorators.IsEmpty() && InnerOperations.IsEmpty())
        {
            Lines.Add(Header);
            return;
        }

        Lines.Add(Header + TEXT(" {"));
        for (UBTDecorator* Decorator : InnerDecorators)
        {
            EmitAuxInstance(Decorator, nullptr, Depth + 1, Lines);
        }
        AppendDecoratorLogicLine(InnerOperations, InnerDecorators.Num(), Depth + 1, Lines);
        Lines.Add(BTIRIndent(Depth) + TEXT("}"));
    }

    void EmitAuxNode(UBehaviorTreeGraphNode* GraphNode, int32 Depth, TArray<FString>& Lines)
    {
        if (UBehaviorTreeGraphNode_CompositeDecorator* CompositeDecorator =
            Cast<UBehaviorTreeGraphNode_CompositeDecorator>(GraphNode))
        {
            EmitCompositeDecoratorNode(CompositeDecorator, Depth, Lines);
            return;
        }

        EmitAuxInstance(GraphNode ? Cast<UBTNode>(GraphNode->NodeInstance.Get()) : nullptr, GraphNode, Depth, Lines);
    }

    int32 CountDecoratorInstances(const TArray<TObjectPtr<UBehaviorTreeGraphNode>>& DecoratorNodes)
    {
        int32 Count = 0;
        for (const TObjectPtr<UBehaviorTreeGraphNode>& DecoratorEntry : DecoratorNodes)
        {
            UBehaviorTreeGraphNode* DecoratorNode = DecoratorEntry.Get();
            if (UBehaviorTreeGraphNode_CompositeDecorator* CompositeDecorator =
                Cast<UBehaviorTreeGraphNode_CompositeDecorator>(DecoratorNode))
            {
                TArray<UBTDecorator*> InnerDecorators;
                TArray<FBTDecoratorLogic> InnerOperations;
                CompositeDecorator->CollectDecoratorData(InnerDecorators, InnerOperations);
                Count += InnerDecorators.Num();
                continue;
            }

            if (DecoratorNode && DecoratorNode->NodeInstance.Get())
            {
                ++Count;
            }
        }
        return Count;
    }

    void EmitBehaviorNode(
        UBehaviorTree* BehaviorTree,
        UBehaviorTreeGraphNode* GraphNode,
        UBehaviorTreeGraphNode* ParentGraphNode,
        const FString& Role,
        int32 Depth,
        TSet<UBehaviorTreeGraphNode*>& Visited,
        TArray<FString>& Lines,
        TArray<FString>& Warnings)
    {
        if (!GraphNode)
        {
            Lines.Add(FString::Printf(TEXT("%s%s null ()"), *BTIRIndent(Depth), *Role));
            Warnings.Add(FString::Printf(TEXT("Encountered null %s node."), *Role));
            return;
        }

        UBTNode* Node = Cast<UBTNode>(GraphNode->NodeInstance.Get());
        const FString Header = FString::Printf(
            TEXT("%s%s %s %s %s%s %s"),
            *BTIRIndent(Depth),
            *Role,
            *NodeKindToken(Node),
            *NodeTypeToken(Node),
            *NodeNameToken(Node, GraphNode),
            *NodePositionSuffix(GraphNode),
            *FIrTextUtils::FormatFieldList(BuildGraphNodeFields(Node, GraphNode)));

        if (Visited.Contains(GraphNode))
        {
            Lines.Add(Header + TEXT(" # already emitted"));
            Warnings.Add(FString::Printf(TEXT("Behavior Tree graph references node '%s' more than once."), *GraphNode->GetName()));
            return;
        }

        Visited.Add(GraphNode);

        TArray<UBehaviorTreeGraphNode*> Children = LinkedChildNodes(GraphNode);
        const bool bHasBody = GraphNode->Decorators.Num() > 0
            || GraphNode->Services.Num() > 0
            || Children.Num() > 0;

        if (!bHasBody)
        {
            Lines.Add(Header);
            return;
        }

        Lines.Add(Header + TEXT(" {"));
        for (UBehaviorTreeGraphNode* Decorator : GraphNode->Decorators)
        {
            EmitAuxNode(Decorator, Depth + 1, Lines);
        }
        AppendDecoratorLogicLine(
            FindDecoratorLogicForAttachment(BehaviorTree, ParentGraphNode, GraphNode),
            CountDecoratorInstances(GraphNode->Decorators),
            Depth + 1,
            Lines);
        for (UBehaviorTreeGraphNode* Service : GraphNode->Services)
        {
            EmitAuxNode(Service, Depth + 1, Lines);
        }
        for (UBehaviorTreeGraphNode* Child : Children)
        {
            EmitBehaviorNode(BehaviorTree, Child, GraphNode, TEXT("child"), Depth + 1, Visited, Lines, Warnings);
        }
        Lines.Add(BTIRIndent(Depth) + TEXT("}"));
    }

    void EmitRootAuxNodes(
        UBehaviorTree* BehaviorTree,
        UBehaviorTreeGraphNode_Root* RootNode,
        int32 Depth,
        TArray<FString>& Lines)
    {
        if (!RootNode || (RootNode->Decorators.IsEmpty() && RootNode->Services.IsEmpty()))
        {
            return;
        }

        Lines.Add(FString::Printf(
            TEXT("%sroot_aux root %s%s %s {"),
            *BTIRIndent(Depth),
            *FIrTextUtils::FormatNameToken(RootNode->GetName()),
            *NodePositionSuffix(RootNode),
            *FIrTextUtils::FormatFieldList(BuildGraphNodeFields(nullptr, RootNode))));
        for (UBehaviorTreeGraphNode* Decorator : RootNode->Decorators)
        {
            EmitAuxNode(Decorator, Depth + 1, Lines);
        }
        AppendDecoratorLogicLine(
            BehaviorTree ? BehaviorTree->RootDecoratorOps : TArray<FBTDecoratorLogic>(),
            CountDecoratorInstances(RootNode->Decorators),
            Depth + 1,
            Lines);
        for (UBehaviorTreeGraphNode* Service : RootNode->Services)
        {
            EmitAuxNode(Service, Depth + 1, Lines);
        }
        Lines.Add(BTIRIndent(Depth) + TEXT("}"));
    }

    UBehaviorTreeGraphNode_Root* FindRootNode(UBehaviorTree* BehaviorTree)
    {
        UBehaviorTreeGraph* Graph = BehaviorTree ? Cast<UBehaviorTreeGraph>(BehaviorTree->BTGraph) : nullptr;
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

    FString BlackboardKeyTypeToken(const UBlackboardKeyType* KeyType)
    {
        if (!KeyType)
        {
            return TEXT("Unknown");
        }

        FString TypeName = KeyType->GetClass()->GetName();
        TypeName.RemoveFromStart(TEXT("BlackboardKeyType_"));
        return FIrTextUtils::FormatNameToken(TypeName);
    }

    TArray<FString> BuildBlackboardKeyFields(const FBlackboardEntry& Entry)
    {
        TArray<FString> Fields;
        TSet<FName> ExplicitProperties;

        UBlackboardKeyType* KeyType = Entry.KeyType.Get();

        if (const UBlackboardKeyType_Object* ObjectKey = Cast<UBlackboardKeyType_Object>(KeyType))
        {
            if (ObjectKey->BaseClass)
            {
                Fields.Add(FString::Printf(TEXT("baseClass: %s"), *ObjectPathToken(ObjectKey->BaseClass)));
                ExplicitProperties.Add(FName(TEXT("BaseClass")));
            }
        }
        else if (const UBlackboardKeyType_Class* ClassKey = Cast<UBlackboardKeyType_Class>(KeyType))
        {
            if (ClassKey->BaseClass)
            {
                Fields.Add(FString::Printf(TEXT("baseClass: %s"), *ObjectPathToken(ClassKey->BaseClass)));
                ExplicitProperties.Add(FName(TEXT("BaseClass")));
            }
        }

        if (Entry.bInstanceSynced)
        {
            Fields.Add(TEXT("instanceSynced: true"));
        }

        UObject* DefaultObject = KeyType ? KeyType->GetClass()->GetDefaultObject() : nullptr;
        FReflectedFieldEmitOptions Options;
        Options.FieldSeparator = TEXT(": ");
        Options.bEmitArraysAsBracketList = true;

        FIrTextUtils::AppendReflectedFields(
            KeyType ? KeyType->GetClass() : nullptr,
            KeyType,
            DefaultObject,
            KeyType,
            ExplicitProperties,
            [](const FStructProperty*)
            {
                return false;
            },
            [](FName)
            {
                return false;
            },
            Options,
            Fields);

        return Fields;
    }

    void AppendBlackboardBlock(UBlackboardData* Blackboard, int32 Depth, TArray<FString>& Lines)
    {
        Lines.Add(FString::Printf(TEXT("%sblackboard %s {"), *BTIRIndent(Depth), *ObjectPathToken(Blackboard)));

        if (Blackboard->Parent)
        {
            Lines.Add(FString::Printf(TEXT("%sparent %s"), *BTIRIndent(Depth + 1), *ObjectPathToken(Blackboard->Parent)));
        }

        for (const FBlackboardEntry& Entry : Blackboard->Keys)
        {
            Lines.Add(FString::Printf(
                TEXT("%skey %s %s %s"),
                *BTIRIndent(Depth + 1),
                *BlackboardKeyTypeToken(Entry.KeyType.Get()),
                *FIrTextUtils::FormatNameToken(Entry.EntryName.ToString()),
                *FIrTextUtils::FormatFieldList(BuildBlackboardKeyFields(Entry))));
        }

        Lines.Add(BTIRIndent(Depth) + TEXT("}"));
    }
}

FBTIRResult BTIRDecompiler::BuildBehaviorTreeIrText(UBehaviorTree* BehaviorTree)
{
    if (!BehaviorTree)
    {
        return FBTIRResult::MakeError(TEXT("BehaviorTree is null."));
    }

    FBTIRResult Result;
    Result.bSuccess = true;

    TArray<FString> Lines;
    Lines.Add(FString::Printf(TEXT("behavior_tree %s {"), *ObjectPathToken(BehaviorTree)));

    if (BehaviorTree->BlackboardAsset)
    {
        Lines.Add(FString::Printf(TEXT("  blackboard %s"), *ObjectPathToken(BehaviorTree->BlackboardAsset)));
    }

    UBehaviorTreeGraphNode_Root* RootNode = FindRootNode(BehaviorTree);
    TSet<UBehaviorTreeGraphNode*> Visited;
    if (!BehaviorTree->BTGraph)
    {
        Result.Warnings.Add(TEXT("Behavior Tree has no BTGraph; topology is unavailable."));
    }
    else if (!RootNode)
    {
        Result.Warnings.Add(TEXT("Behavior Tree graph has no root node."));
    }
    else
    {
        EmitRootAuxNodes(BehaviorTree, RootNode, 1, Lines);
        TArray<UBehaviorTreeGraphNode*> RootChildren = LinkedChildNodes(RootNode);
        if (RootChildren.IsEmpty())
        {
            Result.Warnings.Add(TEXT("Behavior Tree root has no child node."));
        }

        for (UBehaviorTreeGraphNode* Child : RootChildren)
        {
            EmitBehaviorNode(BehaviorTree, Child, RootNode, TEXT("root"), 1, Visited, Lines, Result.Warnings);
        }
    }

    if (BehaviorTree->BTGraph)
    {
        for (UBehaviorTreeGraphNode* GraphNode : BehaviorGraphNodes(BehaviorTree))
        {
            if (!Visited.Contains(GraphNode))
            {
                Result.Warnings.Add(FString::Printf(
                    TEXT("Behavior Tree graph node '%s' is unreachable from root and emitted as orphan."),
                    *GraphNode->GetName()));
                EmitBehaviorNode(BehaviorTree, GraphNode, nullptr, TEXT("orphan"), 1, Visited, Lines, Result.Warnings);
            }
        }
    }

    if (BehaviorTree->BlackboardAsset)
    {
        Lines.Add(TEXT(""));
        AppendBlackboardBlock(BehaviorTree->BlackboardAsset, 1, Lines);
    }

    Lines.Add(TEXT("}"));
    Result.Text = FString::Join(Lines, TEXT("\n")) + TEXT("\n");
    return Result;
}

FBTIRResult BTIRDecompiler::BuildBlackboardIrText(UBlackboardData* Blackboard)
{
    if (!Blackboard)
    {
        return FBTIRResult::MakeError(TEXT("Blackboard is null."));
    }

    FBTIRResult Result;
    Result.bSuccess = true;

    TArray<FString> Lines;
    AppendBlackboardBlock(Blackboard, 0, Lines);
    Result.Text = FString::Join(Lines, TEXT("\n")) + TEXT("\n");
    return Result;
}

void BTIRDecompiler::ClearPropertyCaches()
{
    GetBlackboardSelectorCache().Empty();
}
