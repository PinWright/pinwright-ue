// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "AGIR/AGIRLayoutEngine.h"


#include "Handlers/Animation/AnimGraphConstructionUtils.h"

#include "Animation/AnimBlueprint.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimationStateMachineGraph.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"

namespace
{
using AnimGraphConstructionUtils::IsTopLevelAnimGraph;
struct FNodeRecord
{
    UEdGraphNode* Node = nullptr;
    int32 Depth = 0;
    FString StableKey;
};

bool IsUnpositioned(const UEdGraphNode* Node)
{
    return Node && Node->NodePosX == 0 && Node->NodePosY == 0;
}

FString StableNodeKey(const UEdGraphNode* Node)
{
    if (!Node)
    {
        return FString();
    }
    if (Node->NodeGuid.IsValid())
    {
        return Node->NodeGuid.ToString(EGuidFormats::Digits);
    }
    return Node->GetName();
}

// Filters the shared `CollectInterfaceLayerGraphs` walk to top-level anim
// graphs (the layout engine only positions nodes inside `UAnimationGraph`-
// class graphs; non-anim interface graphs are owned by other walkers).
void CollectInterfaceAnimLayerGraphs(UBlueprint* Blueprint, TArray<UEdGraph*>& OutGraphs, TSet<UEdGraph*>& Seen)
{
    TSet<const UEdGraph*> AllInterfaceGraphs;
    AnimGraphConstructionUtils::CollectInterfaceLayerGraphs(Blueprint, AllInterfaceGraphs);
    for (const UEdGraph* Graph : AllInterfaceGraphs)
    {
        if (IsTopLevelAnimGraph(Graph) && !Seen.Contains(Graph))
        {
            UEdGraph* MutableGraph = const_cast<UEdGraph*>(Graph);
            Seen.Add(MutableGraph);
            OutGraphs.Add(MutableGraph);
        }
    }
}

// Recursively reaches into every state-machine inner graph nested under
// `Graph` so the lane algorithm is applied to state nodes as well.
void CollectStateMachineInnerGraphs(UEdGraph* Graph, TArray<UEdGraph*>& OutGraphs, TSet<UEdGraph*>& Seen)
{
    if (!Graph)
    {
        return;
    }
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        UAnimGraphNode_StateMachineBase* MachineNode = Cast<UAnimGraphNode_StateMachineBase>(Node);
        if (!MachineNode || !MachineNode->EditorStateMachineGraph)
        {
            continue;
        }
        UEdGraph* InnerGraph = MachineNode->EditorStateMachineGraph.Get();
        if (!Seen.Contains(InnerGraph))
        {
            Seen.Add(InnerGraph);
            OutGraphs.Add(InnerGraph);
        }
        // State machine inner graphs may themselves embed nested state machines
        // via state nodes' `BoundGraph`; engine-level `GetAllChildrenGraphs`
        // walks through these via the standard UEdGraph child link.
        TArray<UEdGraph*> Children;
        InnerGraph->GetAllChildrenGraphs(Children);
        for (UEdGraph* Child : Children)
        {
            if (Child && !Seen.Contains(Child))
            {
                Seen.Add(Child);
                OutGraphs.Add(Child);
            }
        }
    }
}

int32 ComputeNodeDepth(
    UEdGraphNode* Node,
    const TSet<UEdGraphNode*>& NodeSet,
    TMap<UEdGraphNode*, int32>& Depths,
    TSet<UEdGraphNode*>& Visiting)
{
    if (!Node)
    {
        return 0;
    }
    if (const int32* Existing = Depths.Find(Node))
    {
        return *Existing;
    }
    if (Visiting.Contains(Node))
    {
        // Cycle guard — anim graphs should be DAGs but cached-pose / state
        // machine inner graphs can produce loops via transition pairs.
        return 0;
    }

    Visiting.Add(Node);
    int32 Depth = 0;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin || Pin->Direction != EGPD_Input)
        {
            continue;
        }
        for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
        {
            if (!LinkedPin || !LinkedPin->GetOwningNode())
            {
                continue;
            }
            UEdGraphNode* Source = LinkedPin->GetOwningNode();
            if (!NodeSet.Contains(Source))
            {
                continue;
            }
            Depth = FMath::Max(Depth, ComputeNodeDepth(Source, NodeSet, Depths, Visiting) + 1);
        }
    }
    Visiting.Remove(Node);
    Depths.Add(Node, Depth);
    return Depth;
}
} // namespace

void FAGIRLayoutEngine::Layout(UAnimBlueprint* AnimBP)
{
    Layout(AnimBP, FAGIRLayoutOptions());
}

void FAGIRLayoutEngine::Layout(UAnimBlueprint* AnimBP, const FAGIRLayoutOptions& Options)
{
    if (!AnimBP)
    {
        return;
    }

    TSet<UEdGraph*> Seen;
    TArray<UEdGraph*> Graphs;

    // Top-level anim graphs from FunctionGraphs.
    for (UEdGraph* Graph : AnimBP->FunctionGraphs)
    {
        if (IsTopLevelAnimGraph(Graph) && !Seen.Contains(Graph))
        {
            Seen.Add(Graph);
            Graphs.Add(Graph);
        }
    }

    // Anim layer interface override pose graphs.
    CollectInterfaceAnimLayerGraphs(AnimBP, Graphs, Seen);

    // State-machine inner graphs reached through any anim graph collected so
    // far. Iterate by index because the array grows.
    const int32 InitialCount = Graphs.Num();
    for (int32 i = 0; i < InitialCount; ++i)
    {
        CollectStateMachineInnerGraphs(Graphs[i], Graphs, Seen);
    }
    // Newly-added state-machine inner graphs may host further machines.
    for (int32 i = InitialCount; i < Graphs.Num(); ++i)
    {
        CollectStateMachineInnerGraphs(Graphs[i], Graphs, Seen);
    }

    for (UEdGraph* Graph : Graphs)
    {
        LayoutGraph(Graph, Options);
    }
}

void FAGIRLayoutEngine::LayoutGraph(UEdGraph* Graph, const FAGIRLayoutOptions& Options)
{
    if (!Graph)
    {
        return;
    }

    // Skip the depth-map / sort scaffolding when no node needs positioning —
    // the dominant case for graphs the user has hand-laid out or that AGIR
    // text already supplied positions for.
    bool bAnyUnpositioned = false;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (IsUnpositioned(Node))
        {
            bAnyUnpositioned = true;
            break;
        }
    }
    if (!bAnyUnpositioned)
    {
        return;
    }

    TSet<UEdGraphNode*> NodeSet;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node)
        {
            NodeSet.Add(Node);
        }
    }

    TMap<UEdGraphNode*, int32> Depths;
    TSet<UEdGraphNode*> Visiting;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node)
        {
            ComputeNodeDepth(Node, NodeSet, Depths, Visiting);
        }
    }

    TArray<FNodeRecord> Records;
    Records.Reserve(NodeSet.Num());
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node)
        {
            continue;
        }
        FNodeRecord Record;
        Record.Node = Node;
        Record.Depth = Depths.FindRef(Node);
        Record.StableKey = StableNodeKey(Node);
        Records.Add(MoveTemp(Record));
    }

    Records.Sort([](const FNodeRecord& A, const FNodeRecord& B)
    {
        if (A.Depth != B.Depth)
        {
            return A.Depth < B.Depth;
        }
        return A.StableKey < B.StableKey;
    });

    TMap<int32, int32> LanesByDepth;
    for (const FNodeRecord& Record : Records)
    {
        UEdGraphNode* Node = Record.Node;
        if (!IsUnpositioned(Node))
        {
            continue;
        }
        int32& Lane = LanesByDepth.FindOrAdd(Record.Depth);
        Node->NodePosX = Options.OriginX + Record.Depth * Options.HorizontalSpacing;
        Node->NodePosY = Options.OriginY + Lane * Options.VerticalSpacing;
        ++Lane;
    }
}
