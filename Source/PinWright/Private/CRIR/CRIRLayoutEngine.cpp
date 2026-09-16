// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "CRIR/CRIRLayoutEngine.h"


#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMLink.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMPin.h"

namespace
{
struct FNodeRecord
{
    URigVMNode* Node = nullptr;
    int32 Depth = 0;
    FString StableKey;
};

int32 ComputeDepth(
    URigVMNode* Node,
    const TSet<URigVMNode*>& NodeSet,
    TMap<URigVMNode*, int32>& Depths,
    TSet<URigVMNode*>& Visiting)
{
    if (!Node)
    {
        return 0;
    }
    if (const int32* Cached = Depths.Find(Node))
    {
        return *Cached;
    }
    if (Visiting.Contains(Node))
    {
        // Cycle guard — RigVM event graphs are usually DAGs but cast nodes /
        // execution-context loops can introduce back edges.
        return 0;
    }

    Visiting.Add(Node);
    int32 Depth = 0;
    for (URigVMLink* Link : Node->GetLinks())
    {
        if (!Link)
        {
            continue;
        }
        URigVMPin* TargetPin = Link->GetTargetPin();
        if (!TargetPin || TargetPin->GetNode() != Node)
        {
            continue;
        }
        URigVMPin* SourcePin = Link->GetSourcePin();
        URigVMNode* Source = SourcePin ? SourcePin->GetNode() : nullptr;
        if (!Source || !NodeSet.Contains(Source))
        {
            continue;
        }
        Depth = FMath::Max(Depth, ComputeDepth(Source, NodeSet, Depths, Visiting) + 1);
    }
    Visiting.Remove(Node);
    Depths.Add(Node, Depth);
    return Depth;
}
} // namespace

void FCRIRLayoutEngine::RunLayout(
    URigVMGraph* Graph,
    const TSet<URigVMNode*>& NodesToLayout,
    URigVMController* Controller)
{
    RunLayout(Graph, NodesToLayout, Controller, FCRIRLayoutOptions());
}

void FCRIRLayoutEngine::RunLayout(
    URigVMGraph* Graph,
    const TSet<URigVMNode*>& NodesToLayout,
    URigVMController* Controller,
    const FCRIRLayoutOptions& Options)
{
    if (!Graph || !Controller || NodesToLayout.Num() == 0)
    {
        return;
    }

    // Depth computation walks the whole graph (NodesToLayout plus pre-positioned
    // obstacles) so columns line up with already-laid-out neighbours.
    TSet<URigVMNode*> NodeSet;
    for (URigVMNode* Node : Graph->GetNodes())
    {
        if (Node)
        {
            NodeSet.Add(Node);
        }
    }

    TMap<URigVMNode*, int32> Depths;
    TSet<URigVMNode*> Visiting;
    for (URigVMNode* Node : NodeSet)
    {
        ComputeDepth(Node, NodeSet, Depths, Visiting);
    }

    // Track lanes occupied by *all* nodes (subjects + obstacles) at each depth
    // so subjects don't stack on top of pre-positioned siblings.
    TMap<int32, int32> LanesByDepth;
    for (URigVMNode* Node : NodeSet)
    {
        if (!Node || NodesToLayout.Contains(Node))
        {
            continue;
        }
        const int32 Depth = Depths.FindRef(Node);
        int32& Lane = LanesByDepth.FindOrAdd(Depth);
        ++Lane;
    }

    TArray<FNodeRecord> Subjects;
    Subjects.Reserve(NodesToLayout.Num());
    for (URigVMNode* Node : NodesToLayout)
    {
        if (!Node)
        {
            continue;
        }
        FNodeRecord Record;
        Record.Node = Node;
        Record.Depth = Depths.FindRef(Node);
        Record.StableKey = Node->GetNodePath();
        Subjects.Add(MoveTemp(Record));
    }

    Subjects.Sort([](const FNodeRecord& A, const FNodeRecord& B)
    {
        if (A.Depth != B.Depth)
        {
            return A.Depth < B.Depth;
        }
        return A.StableKey < B.StableKey;
    });

    for (const FNodeRecord& Record : Subjects)
    {
        int32& Lane = LanesByDepth.FindOrAdd(Record.Depth);
        const FVector2D Position(
            Options.OriginX + Record.Depth * Options.HorizontalSpacing,
            Options.OriginY + Lane * Options.VerticalSpacing);
        ++Lane;
        Controller->SetNodePosition(
            Record.Node,
            Position,
            /*bSetupUndoRedo*/ false,
            /*bMergeUndoAction*/ false,
            /*bPrintPythonCommand*/ false);
    }
}
