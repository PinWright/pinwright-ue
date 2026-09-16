// Copyright (c) 2026 Alexander Penkin. MIT License.

// AnimGraphConstructionUtils.cpp
//
// Implementation of shared anim graph construction helpers. State machine /
// state / transition creation routines are extracted from
// AnimationAuthoringHandler.cpp; behaviour preserved exactly.

#include "AnimGraphConstructionUtils.h"


#include "Animation/AnimBlueprint.h"
#include "AnimationGraph.h"
#include "AnimationGraphSchema.h"
#include "AnimGraphNode_Base.h"
#include "Handlers/Animation/AnimGraphNodeAccessor.h"
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimNodes/AnimNode_LayeredBoneBlend.h"
#include "Animation/AnimData/BoneMaskFilter.h"
#include "AnimGraphNode_LinkedAnimLayer.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimStateNode.h"
#include "AnimStateNodeBase.h"
#include "AnimStateEntryNode.h"
#include "AnimStateConduitNode.h"
#include "AnimStateAliasNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimationStateMachineGraph.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "IrCore/IrTextUtils.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "UObject/UnrealType.h"
#include "Utils/PropertyUtils.h"

namespace AnimGraphConstructionUtils
{

namespace
{
    struct FResolvedAnimNodeField
    {
        FStructProperty* NodeProperty = nullptr;
        FAnimNode_Base* NodeData = nullptr;
        FProperty* FieldProperty = nullptr;
        void* FieldData = nullptr;
    };

    FString ResolveAnimNodeFieldByName(UAnimGraphNode_Base* Node, FName FieldName, FResolvedAnimNodeField& OutResolved)
    {
        if (!Node)
        {
            return TEXT("Node is null");
        }

        FStructProperty* NodeProperty = PinWright::Anim::GetFNodeProperty(Node);
        if (!NodeProperty || !NodeProperty->Struct)
        {
            return TEXT("Node has no FAnimNode_* runtime struct property");
        }

        FAnimNode_Base* NodeData = PinWright::Anim::GetFNode(Node);
        if (!NodeData)
        {
            return TEXT("Could not resolve runtime struct pointer");
        }

        FProperty* FieldProperty = NodeProperty->Struct->FindPropertyByName(FieldName);
        if (!FieldProperty)
        {
            return FString::Printf(TEXT("Field '%s' not found on %s"), *FieldName.ToString(), *NodeProperty->Struct->GetName());
        }

        OutResolved.NodeProperty = NodeProperty;
        OutResolved.NodeData = NodeData;
        OutResolved.FieldProperty = FieldProperty;
        OutResolved.FieldData = FieldProperty->ContainerPtrToValuePtr<void>(NodeData);
        return FString();
    }

    FString ImportAnimNodeFieldText(
        const FResolvedAnimNodeField& Resolved,
        UAnimGraphNode_Base* Node,
        void* Destination,
        FName FieldName,
        const FString& ValueAsText)
    {
        // The AGIR emitter quotes every reflected export, so unwrap the string/name
        // wrapper before ImportText (an empty struct arrives as `"()"`, which
        // FProperty::ImportText rejects for the leading `"`).
        const FString Unquoted = FIrTextUtils::UnwrapStringOrNameToken(ValueAsText);
        const TCHAR* Result = Resolved.FieldProperty->ImportText_Direct(
            *Unquoted, Destination, /*OwnerObject=*/Node, PPF_None);
        if (!Result)
        {
            return FString::Printf(
                TEXT("ImportText failed for '%s' on %s"),
                *FieldName.ToString(), *Resolved.NodeProperty->Struct->GetName());
        }

        return FString();
    }
}

bool IsTopLevelAnimGraph(const UEdGraph* Graph)
{
    if (!Graph || Graph->GetClass() != UAnimationGraph::StaticClass() || !Graph->Schema)
    {
        return false;
    }
    return Graph->Schema->IsChildOf(UAnimationGraphSchema::StaticClass());
}

void CollectInterfaceLayerGraphs(UBlueprint* Blueprint, TSet<const UEdGraph*>& OutGraphs)
{
    if (!Blueprint)
    {
        return;
    }

    for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
    {
        for (UEdGraph* InterfaceGraph : InterfaceDesc.Graphs)
        {
            if (!InterfaceGraph)
            {
                continue;
            }
            OutGraphs.Add(InterfaceGraph);

            TArray<UEdGraph*> Children;
            InterfaceGraph->GetAllChildrenGraphs(Children);
            for (UEdGraph* Child : Children)
            {
                if (Child)
                {
                    OutGraphs.Add(Child);
                }
            }
        }
    }
}

UEdGraph* GetAnimGraphFromBlueprint(UAnimBlueprint* AnimBP)
{
    if (!AnimBP)
    {
        return nullptr;
    }

    for (UEdGraph* Graph : AnimBP->FunctionGraphs)
    {
        if (Graph && Graph->GetName() == TEXT("AnimGraph"))
        {
            return Graph;
        }
    }

    return nullptr;
}

UEdGraph* ResolveAnimBlueprintGraph(UAnimBlueprint* AnimBP, const FString& GraphName)
{
    if (!AnimBP)
    {
        return nullptr;
    }
    if (GraphName.IsEmpty() || GraphName.Equals(TEXT("AnimGraph"), ESearchCase::IgnoreCase))
    {
        return GetAnimGraphFromBlueprint(AnimBP);
    }
    for (UEdGraph* Graph : AnimBP->FunctionGraphs)
    {
        if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
        {
            return Graph;
        }
    }
    return nullptr;
}

UAnimGraphNode_StateMachine* FindStateMachineNode(UEdGraph* Graph, const FString& Name)
{
    if (!Graph)
    {
        return nullptr;
    }

    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (UAnimGraphNode_StateMachine* SMNode = Cast<UAnimGraphNode_StateMachine>(Node))
        {
            const FString NodeName = SMNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
            if (NodeName.Contains(Name) || SMNode->GetStateMachineName() == Name)
            {
                return SMNode;
            }
        }
    }

    return nullptr;
}

UAnimStateNode* FindStateNode(UAnimationStateMachineGraph* SMGraph, const FString& Name)
{
    if (!SMGraph)
    {
        return nullptr;
    }

    for (UEdGraphNode* Node : SMGraph->Nodes)
    {
        if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node))
        {
            if (StateNode->GetStateName() == Name)
            {
                return StateNode;
            }
        }
    }

    return nullptr;
}

UAnimGraphNode_StateMachine* CreateStateMachine(UAnimBlueprint* AnimBP, UEdGraph* ParentGraph, FName Name, const FVector2D& Position)
{
    if (!AnimBP || !ParentGraph)
    {
        return nullptr;
    }

    FGraphNodeCreator<UAnimGraphNode_StateMachine> NodeCreator(*ParentGraph);
    UAnimGraphNode_StateMachine* SMNode = NodeCreator.CreateNode();
    SMNode->NodePosX = static_cast<int32>(Position.X);
    SMNode->NodePosY = static_cast<int32>(Position.Y);
    NodeCreator.Finalize();

    // Finalize() runs UAnimGraphNode_StateMachineBase::PostPlacedNewNode, which has ALREADY built
    // the inner UAnimationStateMachineGraph, pointed it back at this node, run the schema's
    // default nodes on it and registered it in ParentGraph->SubGraphs under the name
    // "New State Machine". Rename that graph rather than building a second one: a replacement
    // graph is outered to the Blueprint and never enters SubGraphs, so UBlueprint::GetAllGraphs
    // cannot see it, and the engine's graph is orphaned on the parent for the life of the asset -
    // which is also what made a rolled-back create_state_machine leave a stray graph behind.
    UAnimationStateMachineGraph* InnerGraph =
        Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
    if (!InnerGraph)
    {
        return nullptr;
    }

    if (!Name.IsNone())
    {
        // The engine's own naming call from PostPlacedNewNode: it uniquifies against the node's
        // sibling graphs the way the previous CreateNewGraph(AnimBP, Name, ...) did.
        const TSharedPtr<INameValidatorInterface> NameValidator =
            FNameValidatorFactory::MakeValidator(SMNode);
        FBlueprintEditorUtils::RenameGraphWithSuggestion(InnerGraph, NameValidator, Name.ToString());
    }

    return SMNode;
}

UAnimGraphNode_Base* CreateAnimNode(UEdGraph* ParentGraph, UClass* NodeClass, const FVector2D& Position)
{
    if (!ParentGraph || !NodeClass)
    {
        return nullptr;
    }

    if (!NodeClass->IsChildOf(UAnimGraphNode_Base::StaticClass()))
    {
        return nullptr;
    }

    FGraphNodeCreator<UAnimGraphNode_Base> NodeCreator(*ParentGraph);
    UAnimGraphNode_Base* Node = NodeCreator.CreateNode(/*bSelectNewNode=*/false, NodeClass);
    if (!Node)
    {
        return nullptr;
    }
    Node->NodePosX = static_cast<int32>(Position.X);
    Node->NodePosY = static_cast<int32>(Position.Y);
    NodeCreator.Finalize();
    return Node;
}

FAnimNodeResolveResult ResolveAnimGraphNodeByTitle(UEdGraph* Graph, const FString& NodeName)
{
    FAnimNodeResolveResult Out;
    if (!Graph)
    {
        return Out; // NotFound
    }

    // Pass 1: collect exact list-view-title matches. An exact title is the
    // unambiguous intent even when other nodes' titles merely contain it
    // (e.g. "Sequence Player" exactly vs. "Sequence Player 'Dino_Idle'").
    UEdGraphNode* ExactNode = nullptr;
    int32 ExactCount = 0;
    // Pass 2 (built in the same scan): every node whose title contains NodeName,
    // with its already-computed title captured in parallel so the result branches
    // below reuse it instead of re-running GetNodeTitle (a non-trivial virtual on
    // anim nodes that interpolates the bound asset name).
    TArray<UEdGraphNode*> SubstringNodes;
    TArray<FString> SubstringTitles;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node)
        {
            continue;
        }
        const FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
        if (Title.Equals(NodeName, ESearchCase::CaseSensitive))
        {
            if (ExactCount == 0)
            {
                ExactNode = Node;
            }
            ++ExactCount;
        }
        if (Title.Contains(NodeName))
        {
            SubstringNodes.Add(Node);
            SubstringTitles.Add(Title);
        }
    }

    // Exactly one exact-title match wins outright (exact beats substring). The
    // match was Title.Equals(NodeName), so the canonical title IS NodeName.
    if (ExactCount == 1)
    {
        Out.Status = EAnimNodeResolveStatus::Found;
        Out.Node = Cast<UAnimGraphNode_Base>(ExactNode);
        Out.Candidates.Add(NodeName);
        return Out;
    }

    if (SubstringNodes.Num() == 0)
    {
        return Out; // NotFound
    }
    if (SubstringNodes.Num() == 1)
    {
        Out.Status = EAnimNodeResolveStatus::Found;
        Out.Node = Cast<UAnimGraphNode_Base>(SubstringNodes[0]);
        Out.Candidates.Add(SubstringTitles[0]);
        return Out;
    }

    // >1 substring match and no single exact-title match: ambiguous. List every
    // matching node's title so the caller can disambiguate.
    Out.Status = EAnimNodeResolveStatus::Ambiguous;
    Out.Candidates = MoveTemp(SubstringTitles);
    return Out;
}

UAnimStateNode* CreateState(UAnimationStateMachineGraph* MachineGraph, FName StateName, const FVector2D& Position)
{
    if (!MachineGraph)
    {
        return nullptr;
    }

    FGraphNodeCreator<UAnimStateNode> StateCreator(*MachineGraph);
    UAnimStateNode* StateNode = StateCreator.CreateNode();
    StateNode->NodePosX = static_cast<int32>(Position.X);
    StateNode->NodePosY = static_cast<int32>(Position.Y);
    StateCreator.Finalize();

    if (StateNode->BoundGraph)
    {
        FBlueprintEditorUtils::RenameGraph(StateNode->BoundGraph, *StateName.ToString());
    }

    return StateNode;
}

UAnimStateTransitionNode* CreateTransition(UAnimStateNodeBase* FromState, UAnimStateNodeBase* ToState, const FVector2D& Position)
{
    if (!FromState || !ToState)
    {
        return nullptr;
    }

    UEdGraph* OwningGraph = FromState->GetGraph();
    if (!OwningGraph)
    {
        return nullptr;
    }

    FGraphNodeCreator<UAnimStateTransitionNode> TransCreator(*OwningGraph);
    UAnimStateTransitionNode* TransNode = TransCreator.CreateNode();
    TransNode->NodePosX = static_cast<int32>(Position.X);
    TransNode->NodePosY = static_cast<int32>(Position.Y);
    TransCreator.Finalize();

    TransNode->CreateConnections(FromState, ToState);

    return TransNode;
}

bool SetStateMachineEntry(UAnimationStateMachineGraph* SMGraph, UAnimStateNode* TargetState,
    FString& OutErrorCode, FString& OutErrorMessage)
{
    OutErrorCode.Reset();
    OutErrorMessage.Reset();

    UAnimStateEntryNode* EntryNode = SMGraph ? SMGraph->EntryNode.Get() : nullptr;
    if (!EntryNode || EntryNode->Pins.Num() == 0)
    {
        OutErrorCode = TEXT("ENTRY_NODE_NOT_FOUND");
        OutErrorMessage = TEXT("State machine has no entry node or entry pin");
        return false;
    }

    UEdGraphPin* EntryPin = EntryNode->Pins[0];
    UEdGraphPin* TargetInputPin = TargetState ? TargetState->GetInputPin() : nullptr;
    if (!TargetInputPin)
    {
        OutErrorCode = TEXT("STATE_INPUT_PIN_MISSING");
        OutErrorMessage = TEXT("Target state has no input pin");
        return false;
    }

    const UEdGraphSchema* Schema = EntryPin->GetSchema();
    if (!Schema || !Schema->TryCreateConnection(EntryPin, TargetInputPin))
    {
        OutErrorCode = TEXT("CONNECTION_FAILED");
        OutErrorMessage = TEXT("Could not connect entry node to target state");
        return false;
    }

    return true;
}

UAnimStateConduitNode* CreateConduit(UAnimationStateMachineGraph* MachineGraph, FName ConduitName, const FVector2D& Position)
{
    if (!MachineGraph)
    {
        return nullptr;
    }

    FGraphNodeCreator<UAnimStateConduitNode> ConduitCreator(*MachineGraph);
    UAnimStateConduitNode* ConduitNode = ConduitCreator.CreateNode();
    ConduitNode->NodePosX = static_cast<int32>(Position.X);
    ConduitNode->NodePosY = static_cast<int32>(Position.Y);
    ConduitCreator.Finalize();

    if (ConduitNode->BoundGraph)
    {
        FBlueprintEditorUtils::RenameGraph(ConduitNode->BoundGraph, *ConduitName.ToString());
    }

    return ConduitNode;
}

UAnimStateAliasNode* CreateStateAlias(UAnimationStateMachineGraph* MachineGraph, FName AliasName, const FVector2D& Position)
{
    if (!MachineGraph)
    {
        return nullptr;
    }

    FGraphNodeCreator<UAnimStateAliasNode> AliasCreator(*MachineGraph);
    UAnimStateAliasNode* AliasNode = AliasCreator.CreateNode();
    AliasNode->NodePosX = static_cast<int32>(Position.X);
    AliasNode->NodePosY = static_cast<int32>(Position.Y);
    AliasCreator.Finalize();

    // GetStateName() returns StateAliasName; this drives transition by-name
    // resolution. OnRenameNode wraps the same write but also invokes name
    // validation; the deterministic compile path doesn't need that, the
    // caller has already chosen a unique name from the AGIR text.
    AliasNode->StateAliasName = AliasName.ToString();

    return AliasNode;
}

bool WirePoseLink(UEdGraphNode* UpstreamNode, FName UpstreamPinName, UEdGraphNode* DownstreamNode, FName DownstreamPinName)
{
    if (!UpstreamNode || !DownstreamNode)
    {
        return false;
    }

    UEdGraphPin* OutPin = UpstreamNode->FindPin(UpstreamPinName);
    UEdGraphPin* InPin = DownstreamNode->FindPin(DownstreamPinName);
    if (!OutPin || !InPin)
    {
        return false;
    }

    const UEdGraphSchema* Schema = UpstreamNode->GetSchema();
    if (!Schema)
    {
        return false;
    }

    return Schema->TryCreateConnection(OutPin, InPin);
}

FString WriteAnimNodeFieldByName(UAnimGraphNode_Base* Node, FName FieldName, const FString& ValueAsText)
{
    FResolvedAnimNodeField Resolved;
    const FString ResolveError = ResolveAnimNodeFieldByName(Node, FieldName, Resolved);
    if (!ResolveError.IsEmpty())
    {
        return ResolveError;
    }

    return ImportAnimNodeFieldText(Resolved, Node, Resolved.FieldData, FieldName, ValueAsText);
}

FString ValidateAnimNodeFieldByName(UAnimGraphNode_Base* Node, FName FieldName, const FString& ValueAsText)
{
    FResolvedAnimNodeField Resolved;
    const FString ResolveError = ResolveAnimNodeFieldByName(Node, FieldName, Resolved);
    if (!ResolveError.IsEmpty())
    {
        return ResolveError;
    }

    void* Scratch = Resolved.FieldProperty->AllocateAndInitializeValue();
    if (!Scratch)
    {
        return FString::Printf(TEXT("Could not allocate scratch value for '%s'"), *FieldName.ToString());
    }

    Resolved.FieldProperty->CopyCompleteValue(Scratch, Resolved.FieldData);
    const FString ImportError = ImportAnimNodeFieldText(
        Resolved, Node, Scratch, FieldName, ValueAsText);
    Resolved.FieldProperty->DestroyAndFreeValue(Scratch);
    return ImportError;
}

bool ApplyJsonValueToAnimNodeFieldByName(
    UAnimGraphNode_Base* Node,
    FName FieldName,
    const TSharedPtr<FJsonValue>& Value,
    FString& OutError)
{
    OutError.Empty();
    FResolvedAnimNodeField Resolved;
    OutError = ResolveAnimNodeFieldByName(Node, FieldName, Resolved);
    if (!OutError.IsEmpty())
    {
        return false;
    }

    if (!ApplyJsonValueToProperty(Resolved.NodeData, Resolved.FieldProperty, Value, OutError))
    {
        return false;
    }

    return true;
}

bool ToggleOptionalPinExposed(UAnimGraphNode_Base* Node, FName PropertyName, bool bExposed, FString& OutError)
{
    if (!Node)
    {
        OutError = TEXT("Node is null");
        return false;
    }

    int32 FoundIndex = INDEX_NONE;
    for (int32 Index = 0; Index < Node->ShowPinForProperties.Num(); ++Index)
    {
        if (Node->ShowPinForProperties[Index].PropertyName == PropertyName)
        {
            FoundIndex = Index;
            break;
        }
    }

    if (FoundIndex == INDEX_NONE)
    {
        OutError = TEXT("OPTIONAL_PIN_NOT_FOUND");
        return false;
    }

    // Some optional pins are flagged non-toggleable by the property-list rebuild;
    // promote when exposing so SetPinVisibility doesn't silently noop.
    if (bExposed && !Node->ShowPinForProperties[FoundIndex].bCanToggleVisibility)
    {
        Node->ShowPinForProperties[FoundIndex].bCanToggleVisibility = true;
    }

    // Engine-public API: handles CacheShownPins -> EvaluateOldShownPins -> ReconstructNode
    // -> property-changed broadcast, preserving variable bindings across the toggle.
    Node->SetPinVisibility(bExposed, FoundIndex);
    return true;
}

FString ValidateLayeredBlendLayers(
    const TArray<TSharedPtr<FJsonValue>>& LayersJson)
{
    // Validate the entire LayersJson before any node mutation. Each layer must be an
    // object; `branchFilters` is optional (BlendMask mode legitimately has no filters).
    // If present, every entry must carry a non-empty boneName.
    for (int32 Index = 0; Index < LayersJson.Num(); ++Index)
    {
        const TSharedPtr<FJsonValue>& LayerVal = LayersJson[Index];
        const TSharedPtr<FJsonObject>* LayerObj = nullptr;
        if (!LayerVal.IsValid() || !LayerVal->TryGetObject(LayerObj) || !LayerObj || !LayerObj->IsValid())
        {
            return FString::Printf(TEXT("INVALID_LAYERS: layer[%d] is not an object"), Index);
        }
        const TArray<TSharedPtr<FJsonValue>>* FiltersArray = nullptr;
        if (!(*LayerObj)->TryGetArrayField(TEXT("branchFilters"), FiltersArray) || FiltersArray == nullptr)
        {
            // No branchFilters — legal (BlendMask mode). Leave LayerSetup[i] empty.
            continue;
        }
        for (int32 FIdx = 0; FIdx < FiltersArray->Num(); ++FIdx)
        {
            const TSharedPtr<FJsonObject>* FilterObj = nullptr;
            if (!(*FiltersArray)[FIdx].IsValid() || !(*FiltersArray)[FIdx]->TryGetObject(FilterObj) || !FilterObj || !FilterObj->IsValid())
            {
                return FString::Printf(TEXT("INVALID_LAYERS: layer[%d].branchFilters[%d] is not an object"), Index, FIdx);
            }
            FString BoneName;
            if (!(*FilterObj)->TryGetStringField(TEXT("boneName"), BoneName) || BoneName.IsEmpty())
            {
                return FString::Printf(TEXT("INVALID_LAYERS: layer[%d].branchFilters[%d] missing/empty boneName"), Index, FIdx);
            }
        }
    }

    return FString();
}

FString WriteLayeredBlendLayers(
    UAnimGraphNode_LayeredBoneBlend* Node,
    const TArray<TSharedPtr<FJsonValue>>& LayersJson)
{
    if (!Node)
    {
        return TEXT("Node is null");
    }

    const FString ValidationError = ValidateLayeredBlendLayers(LayersJson);
    if (!ValidationError.IsEmpty())
    {
        return ValidationError;
    }

    ApplyValidatedLayeredBlendLayers(Node, LayersJson);
    return FString();
}

void ApplyValidatedLayeredBlendLayers(
    UAnimGraphNode_LayeredBoneBlend* Node,
    const TArray<TSharedPtr<FJsonValue>>& LayersJson)
{
    // The caller must run ValidateLayeredBlendLayers before entering this mutation-only path.
    // Keeping the validation wrapper above separate lets a setter preflight all inputs before
    // changing another live field without retaining a post-mutation error branch.
    if (!Node)
    {
        return;
    }

    const int32 NumLayers = LayersJson.Num();

    // Pass 2: lock-step rebuild of the four parallel arrays on the runtime node.
    // Pattern mirrors AGIRCompiler_LayeredBlend.cpp:117-129. AddPose() and
    // SyncBlendMasksAndLayers() are not DLL-exported in UE 5.6 — inline using
    // public state only.
    Node->Node.LayerSetup.Reset();
    Node->Node.BlendPoses.Reset();
    Node->Node.BlendWeights.Reset();
    Node->Node.BlendMasks.Reset();

    for (int32 Index = 0; Index < NumLayers; ++Index)
    {
        Node->Node.BlendWeights.Add(1.f);
        Node->Node.BlendPoses.AddDefaulted();
        if (Node->Node.BlendMode == ELayeredBoneBlendMode::BlendMask)
        {
            Node->Node.BlendMasks.SetNum(Node->Node.BlendPoses.Num());
            Node->Node.LayerSetup.Reset();
        }
        else
        {
            Node->Node.BlendMasks.Reset();
            Node->Node.LayerSetup.SetNum(Node->Node.BlendPoses.Num());
        }
    }

    // Pass 3: write branchFilters into each LayerSetup entry (BranchFilter mode
    // only — BlendMask mode leaves LayerSetup empty per the lock-step rule above).
    for (int32 Index = 0; Index < NumLayers; ++Index)
    {
        const TSharedPtr<FJsonObject>* LayerObj = nullptr;
        LayersJson[Index]->TryGetObject(LayerObj);
        const TArray<TSharedPtr<FJsonValue>>* FiltersArray = nullptr;
        (*LayerObj)->TryGetArrayField(TEXT("branchFilters"), FiltersArray);

        if (Node->Node.BlendMode == ELayeredBoneBlendMode::BranchFilter && Node->Node.LayerSetup.IsValidIndex(Index))
        {
            FInputBlendPose& Layer = Node->Node.LayerSetup[Index];
            Layer.BranchFilters.Reset();
            if (FiltersArray == nullptr)
            {
                continue;
            }
            for (const TSharedPtr<FJsonValue>& FilterVal : *FiltersArray)
            {
                const TSharedPtr<FJsonObject>* FilterObj = nullptr;
                FilterVal->TryGetObject(FilterObj);
                FString BoneName;
                (*FilterObj)->TryGetStringField(TEXT("boneName"), BoneName);
                double BlendDepthD = 0.0;
                (*FilterObj)->TryGetNumberField(TEXT("blendDepth"), BlendDepthD);

                FBranchFilter Filter;
                Filter.BoneName = FName(*BoneName);
                Filter.BlendDepth = static_cast<int32>(BlendDepthD);
                Layer.BranchFilters.Emplace(MoveTemp(Filter));
            }
        }
    }

}

UEdGraph* CreateAnimLayerInterfaceImplementationGraph(UAnimBlueprint* AnimBP, UClass* InterfaceClass, FName FunctionName)
{
    if (!AnimBP || !InterfaceClass)
    {
        return nullptr;
    }

    UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
        AnimBP,
        FunctionName,
        UAnimationGraph::StaticClass(),
        UAnimationGraphSchema::StaticClass());

    if (!NewGraph)
    {
        return nullptr;
    }

    FBlueprintEditorUtils::AddFunctionGraph<UClass>(AnimBP, NewGraph, /*bIsUserCreated=*/false, InterfaceClass);

    return NewGraph;
}

void ClearAnimGraph(UAnimBlueprint* AnimBP)
{
    if (!AnimBP)
    {
        return;
    }

    // ClearOne: drop every authored node, leaving only the schema-default
    // UAnimGraphNode_Root so the graph remains compile-able as an empty stub.
    auto ClearOne = [](UEdGraph* Graph)
    {
        if (!Graph)
        {
            return;
        }
        Graph->Modify();
        Graph->Nodes.RemoveAll([](UEdGraphNode* N)
        {
            return N && !N->IsA<UAnimGraphNode_Root>();
        });
    };

    for (UEdGraph* Graph : AnimBP->FunctionGraphs)
    {
        if (IsTopLevelAnimGraph(Graph))
        {
            ClearOne(Graph);
        }
    }

    for (FBPInterfaceDescription& InterfaceDesc : AnimBP->ImplementedInterfaces)
    {
        for (UEdGraph* InterfaceGraph : InterfaceDesc.Graphs)
        {
            if (IsTopLevelAnimGraph(InterfaceGraph))
            {
                ClearOne(InterfaceGraph);
            }
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
}

void SetupLinkedAnimLayerFromLayerId(UAnimGraphNode_LinkedAnimLayer* LayerNode, FName LayerId)
{
    if (!LayerNode)
    {
        return;
    }

    // Body inlined from engine's UAnimGraphNode_LinkedAnimLayer::SetupFromLayerId
    // (Editor/AnimGraph/Private/AnimGraphNode_LinkedAnimLayer.cpp:972). The engine
    // method is `protected` and not exported across the AnimGraph DLL boundary
    // (no *_API tag), so a using-declaration access shim still leaves an
    // unresolved external at link time. The body only needs public state on
    // FAnimNode_LinkedAnimLayer (Layer, Interface), the public Node.InstanceClass
    // on FAnimNode_LinkedAnimGraph base, the public InterfaceGuid UPROPERTY on
    // UAnimGraphNode_LinkedAnimLayer, the public GetTargetClass() on
    // UAnimGraphNode_CustomProperty base, and the protected FunctionReference
    // on UAnimGraphNode_LinkedAnimGraphBase (accessed via a derived-class
    // member-access shim — fields don't generate link symbols).
    LayerNode->Node.Layer = LayerId;

    // FunctionReference is `protected` on UAnimGraphNode_LinkedAnimGraphBase.
    // Define a local derived class to expose the field via static_cast — this
    // is a compile-time access check, no link symbol is generated for member
    // field access (compiler emits an offset reference into the object).
    struct FFunctionRefAccess : public UAnimGraphNode_LinkedAnimLayer
    {
        // FunctionReference is declared `protected` on UAnimGraphNode_LinkedAnimGraphBase
        // (the immediate base of UAnimGraphNode_LinkedAnimLayer). Naming it via the
        // grandparent base in the using-declaration matches the actual declaring scope.
        using UAnimGraphNode_LinkedAnimGraphBase::FunctionReference;
    };
    FFunctionRefAccess* AccessNode = static_cast<FFunctionRefAccess*>(LayerNode);

    // Set to self member first so we have a valid name for inlined GetLayerName-equivalent
    // lookup of the interface graph. The engine comment notes this pre-step is required
    // because GetInterfaceForLayer() routes through GetLayerName() (which reads
    // FunctionReference.GetMemberName()).
    AccessNode->FunctionReference.SetSelfMember(LayerId);

    // Inlined GetInterfaceForLayer(): walk the owning AnimBlueprint's implemented
    // interfaces and return the first interface whose layer graph FName matches
    // our layer id. Uses only public state (UAnimBlueprint::ImplementedInterfaces,
    // FBPInterfaceDescription::Graphs, UEdGraph::GetFName, FBPInterfaceDescription::Interface).
    TSubclassOf<UAnimLayerInterface> ResolvedInterface = nullptr;
    if (UAnimBlueprint* CurrentBlueprint = Cast<UAnimBlueprint>(LayerNode->GetBlueprint()))
    {
        for (FBPInterfaceDescription& InterfaceDesc : CurrentBlueprint->ImplementedInterfaces)
        {
            for (UEdGraph* InterfaceGraph : InterfaceDesc.Graphs)
            {
                if (InterfaceGraph && InterfaceGraph->GetFName() == LayerId)
                {
                    ResolvedInterface = InterfaceDesc.Interface;
                    break;
                }
            }
            if (ResolvedInterface)
            {
                break;
            }
        }
    }
    LayerNode->Node.Interface = ResolvedInterface;

    // Inlined GetGuidForLayer(): same walk, but capture the matching interface
    // graph's UEdGraph::InterfaceGuid (public field).
    FGuid ResolvedGuid;
    if (UAnimBlueprint* CurrentBlueprint = Cast<UAnimBlueprint>(LayerNode->GetBlueprint()))
    {
        for (FBPInterfaceDescription& InterfaceDesc : CurrentBlueprint->ImplementedInterfaces)
        {
            for (UEdGraph* InterfaceGraph : InterfaceDesc.Graphs)
            {
                if (InterfaceGraph && InterfaceGraph->GetFName() == LayerId)
                {
                    ResolvedGuid = InterfaceGraph->InterfaceGuid;
                    break;
                }
            }
            if (ResolvedGuid.IsValid())
            {
                break;
            }
        }
    }
    LayerNode->InterfaceGuid = ResolvedGuid;

    if (LayerNode->Node.Interface.Get() == nullptr)
    {
        // Self layers cannot have override implementations
        LayerNode->Node.InstanceClass = nullptr;
    }

    // Set up function reference. GetTargetClass() is public on
    // UAnimGraphNode_CustomProperty base; FBlueprintEditorUtils helpers are public.
    UClass* TargetClass = LayerNode->GetTargetClass();
    if (TargetClass)
    {
        FGuid FunctionGuid;
        FBlueprintEditorUtils::GetFunctionGuidFromClassByFieldName(
            FBlueprintEditorUtils::GetMostUpToDateClass(TargetClass), LayerId, FunctionGuid);
        AccessNode->FunctionReference.SetExternalMember(LayerId, TargetClass, FunctionGuid);
    }
    else
    {
        AccessNode->FunctionReference.SetSelfMember(LayerId);
    }
}

} // namespace AnimGraphConstructionUtils
