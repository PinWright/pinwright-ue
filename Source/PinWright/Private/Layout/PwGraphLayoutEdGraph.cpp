// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwGraphLayoutEdGraph.cpp - UEdGraph adapter for PwGraphLayout.

#include "Layout/PwGraphLayoutEdGraph.h"

#include "AnimGraphNode_StateMachineBase.h"
#include "Animation/AnimBlueprint.h"
#include "AnimationStateMachineGraph.h"
#include "BpirLayoutSettings.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Handlers/Animation/AnimGraphConstructionUtils.h"
#include "Layout/BlueprintNodeSizeAdapter.h"

namespace PwGraphLayoutEdGraphImpl
{
    FString NodeKey(const UEdGraphNode* Node)
    {
        return Node->NodeGuid.ToString(EGuidFormats::Digits) + TEXT(":") + Node->GetName();
    }

    bool IsAtOrigin(const UEdGraphNode* Node)
    {
        return Node->NodePosX == 0 && Node->NodePosY == 0;
    }

    // Anim graphs PinWright authors into: top-level anim graphs, anim-layer interface graphs,
    // then every state-machine graph (and the graphs nested in its states) reached from those.
    TArray<UEdGraph*> CollectAnimGraphs(UAnimBlueprint* AnimBP)
    {
        TArray<UEdGraph*> Graphs;
        for (UEdGraph* Graph : AnimBP->FunctionGraphs)
        {
            if (AnimGraphConstructionUtils::IsTopLevelAnimGraph(Graph))
            {
                Graphs.AddUnique(Graph);
            }
        }
        TSet<const UEdGraph*> InterfaceGraphs;
        AnimGraphConstructionUtils::CollectInterfaceLayerGraphs(AnimBP, InterfaceGraphs);
        for (const UEdGraph* Graph : InterfaceGraphs)
        {
            if (AnimGraphConstructionUtils::IsTopLevelAnimGraph(Graph))
            {
                Graphs.AddUnique(const_cast<UEdGraph*>(Graph));
            }
        }
        // Graphs grows while it is walked, so nested machines are reached too.
        for (int32 Cursor = 0; Cursor < Graphs.Num(); ++Cursor)
        {
            for (UEdGraphNode* Node : Graphs[Cursor]->Nodes)
            {
                const UAnimGraphNode_StateMachineBase* Machine = Cast<UAnimGraphNode_StateMachineBase>(Node);
                UEdGraph* MachineGraph = Machine ? Machine->EditorStateMachineGraph.Get() : nullptr;
                if (!MachineGraph)
                {
                    continue;
                }
                Graphs.AddUnique(MachineGraph);
                TArray<UEdGraph*> Nested;
                MachineGraph->GetAllChildrenGraphs(Nested);
                for (UEdGraph* Child : Nested)
                {
                    if (Child)
                    {
                        Graphs.AddUnique(Child);
                    }
                }
            }
        }
        return Graphs;
    }
}

namespace PwGraphLayout
{
    FSpacing SpacingFrom(const UBpirLayoutSettings& Settings)
    {
        FSpacing Spacing;
        Spacing.ColumnGap = Settings.ColumnGapPx;
        Spacing.RowGap = Settings.RowGapPx;
        Spacing.DataColumnGap = Settings.DataColumnGapPx;
        Spacing.Grid = Settings.GridSnapPx;
        return Spacing;
    }

    FEdGraphModel BuildEdGraphModel(
        UEdGraph* Graph, const TSet<UEdGraphNode*>& Movable, const UBpirLayoutSettings& Settings)
    {
        using namespace PwGraphLayoutEdGraphImpl;

        FEdGraphModel Model;
        if (!Graph)
        {
            return Model;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && !Node->IsA<UEdGraphNode_Comment>())
            {
                Model.Nodes.Add(Node);
            }
        }
        Model.Nodes.Sort([](const UEdGraphNode& A, const UEdGraphNode& B) { return NodeKey(&A) < NodeKey(&B); });

        const GraphLayout::FBlueprintNodeSizeAdapter Sizer(Settings);
        TMap<const UEdGraphPin*, TPair<int32, int32>> SlotOfPin;
        for (int32 Index = 0; Index < Model.Nodes.Num(); ++Index)
        {
            const UEdGraphNode* Node = Model.Nodes[Index];
            FLayoutNode& Out = Model.Layout.Nodes.AddDefaulted_GetRef();
            Out.Key = NodeKey(Node);
            Out.Position = FVector2D(Node->NodePosX, Node->NodePosY);
            Out.Size = Sizer.EstimateNodeSize(Node);
            Out.bMovable = Movable.Contains(Node);

            int32 InputRow = 0;
            int32 OutputRow = 0;
            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (!GraphLayout::FBlueprintNodeSizeAdapter::IsPinShown(Pin))
                {
                    continue;
                }
                FPinSlot Slot;
                Slot.Side = Pin->Direction == EGPD_Input ? EPinSide::Input : EPinSide::Output;
                Slot.Kind = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec ? EWireKind::Flow : EWireKind::Data;
                Slot.OffsetY = GraphLayout::FBlueprintNodeSizeAdapter::IsPinInTitle(Pin)
                    ? Sizer.TitlePinOffsetY()
                    : Sizer.PinOffsetY(Slot.Side == EPinSide::Input ? InputRow++ : OutputRow++);
                SlotOfPin.Add(Pin, TPair<int32, int32>(Index, Out.Pins.Add(Slot)));
            }
        }

        for (int32 Index = 0; Index < Model.Nodes.Num(); ++Index)
        {
            for (const UEdGraphPin* Pin : Model.Nodes[Index]->Pins)
            {
                const TPair<int32, int32>* From = SlotOfPin.Find(Pin);
                if (!From || Pin->Direction != EGPD_Output)
                {
                    continue;
                }
                for (const UEdGraphPin* Linked : Pin->LinkedTo)
                {
                    if (const TPair<int32, int32>* To = SlotOfPin.Find(Linked))
                    {
                        Model.Layout.Wires.Add({ From->Key, From->Value, To->Key, To->Value });
                    }
                }
            }
        }
        return Model;
    }

    FArrangeReport ArrangeEdGraph(
        UEdGraph* Graph,
        const TArray<UEdGraphNode*>& Movable,
        const TArray<UEdGraphNode*>& Roots,
        const UBpirLayoutSettings& Settings)
    {
        FEdGraphModel Model = BuildEdGraphModel(Graph, TSet<UEdGraphNode*>(Movable), Settings);
        for (UEdGraphNode* Root : Roots)
        {
            const int32 Index = Model.Nodes.IndexOfByKey(Root);
            if (Index != INDEX_NONE)
            {
                Model.Layout.Roots.Add(Index);
            }
        }

        const FArrangeReport Report = Arrange(Model.Layout, SpacingFrom(Settings));
        for (int32 Index = 0; Index < Model.Nodes.Num(); ++Index)
        {
            UEdGraphNode* Node = Model.Nodes[Index];
            const FLayoutNode& Placed = Model.Layout.Nodes[Index];
            const int32 X = FMath::RoundToInt(Placed.Position.X);
            const int32 Y = FMath::RoundToInt(Placed.Position.Y);
            if (Placed.bMovable && (Node->NodePosX != X || Node->NodePosY != Y))
            {
                Node->Modify();
                Node->NodePosX = X;
                Node->NodePosY = Y;
            }
        }
        return Report;
    }

    FArrangeReport ArrangeAnimBlueprint(UAnimBlueprint* AnimBP)
    {
        FArrangeReport Total;
        const UBpirLayoutSettings* Settings = GetDefault<UBpirLayoutSettings>();
        if (!AnimBP || !Settings)
        {
            return Total;
        }
        for (UEdGraph* Graph : PwGraphLayoutEdGraphImpl::CollectAnimGraphs(AnimBP))
        {
            TArray<UEdGraphNode*> Movable;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node && !Node->IsA<UEdGraphNode_Comment>() && PwGraphLayoutEdGraphImpl::IsAtOrigin(Node))
                {
                    Movable.Add(Node);
                }
            }
            if (Movable.Num() == 0)
            {
                continue;
            }
            const FArrangeReport Report = ArrangeEdGraph(Graph, Movable, {}, *Settings);
            Total.Moved += Report.Moved;
            Total.Trees += Report.Trees;
            Total.MeasuredSizes += Report.MeasuredSizes;
            Total.EstimatedSizes += Report.EstimatedSizes;
        }
        return Total;
    }
}
