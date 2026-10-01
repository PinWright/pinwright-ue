// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwGraphLayoutRigVM.cpp - RigVM adapter for PwGraphLayout.

#include "Layout/PwGraphLayoutRigVM.h"

#include "RigVMModel/Nodes/RigVMCommentNode.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMLink.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMPin.h"

namespace PwGraphLayoutRigVMImpl
{
    using namespace PwGraphLayout;

    // Measured on the Control Rig editor's drawn nodes: a ~16 px title bar, ~24 px rows, and nodes
    // ~250-280 px wide because most rows carry a value editor (enum combo, name picker, number box).
    constexpr double HeaderHeight = 16.0;
    constexpr double RowHeight = 24.0;
    constexpr double MinWidth = 260.0;
    constexpr double MinHeight = 64.0;
    constexpr double TitleCharWidth = 8.0;
    constexpr double SidePadding = 64.0;

    FSpacing RigSpacing()
    {
        FSpacing Spacing;
        Spacing.ColumnGap = 80.0;
        Spacing.RowGap = 32.0;
        Spacing.DataColumnGap = 48.0;
        Spacing.Grid = 16.0;
        return Spacing;
    }

    struct FPinSlots
    {
        int32 Input = INDEX_NONE;
        int32 Output = INDEX_NONE;
    };

    // One row per shown pin, depth first: an expanded pin's sub-pins follow it.
    void AddPinRows(const URigVMPin* Pin, int32 NodeIndex, FLayoutNode& Out, int32& Row,
        TMap<const URigVMPin*, TPair<int32, FPinSlots>>& SlotsOfPin)
    {
        const ERigVMPinDirection Direction = Pin->GetDirection();
        const bool bInput = Direction == ERigVMPinDirection::Input || Direction == ERigVMPinDirection::IO
            || Direction == ERigVMPinDirection::Visible;
        const bool bOutput = Direction == ERigVMPinDirection::Output || Direction == ERigVMPinDirection::IO;
        if (!bInput && !bOutput)
        {
            return;
        }
        const EWireKind Kind = Pin->IsExecuteContext() ? EWireKind::Flow : EWireKind::Data;
        const double Centre = HeaderHeight + (Row + 0.5) * RowHeight;
        FPinSlots Slots;
        if (bInput) { Slots.Input = Out.Pins.Add({ EPinSide::Input, Kind, Centre }); }
        if (bOutput) { Slots.Output = Out.Pins.Add({ EPinSide::Output, Kind, Centre }); }
        SlotsOfPin.Add(Pin, TPair<int32, FPinSlots>(NodeIndex, Slots));
        ++Row;
        if (Pin->IsExpanded())
        {
            for (const URigVMPin* Sub : Pin->GetSubPins())
            {
                AddPinRows(Sub, NodeIndex, Out, Row, SlotsOfPin);
            }
        }
    }

    // The pin itself when it has a row, else its nearest shown parent.
    const TPair<int32, FPinSlots>* FindShownSlots(
        const URigVMPin* Pin, const TMap<const URigVMPin*, TPair<int32, FPinSlots>>& SlotsOfPin)
    {
        for (; Pin; Pin = Pin->GetParentPin())
        {
            if (const TPair<int32, FPinSlots>* Found = SlotsOfPin.Find(Pin))
            {
                return Found;
            }
        }
        return nullptr;
    }
}

namespace PwGraphLayout
{
    FRigVMModel BuildRigVMModel(URigVMGraph* Graph, const TSet<URigVMNode*>& Movable)
    {
        using namespace PwGraphLayoutRigVMImpl;

        FRigVMModel Model;
        if (!Graph)
        {
            return Model;
        }
        for (URigVMNode* Node : Graph->GetNodes())
        {
            if (Node && !Node->IsA<URigVMCommentNode>())
            {
                Model.Nodes.Add(Node);
            }
        }
        Model.Nodes.Sort([](const URigVMNode& A, const URigVMNode& B) { return A.GetNodePath() < B.GetNodePath(); });

        TMap<const URigVMPin*, TPair<int32, FPinSlots>> SlotsOfPin;
        for (int32 Index = 0; Index < Model.Nodes.Num(); ++Index)
        {
            URigVMNode* Node = Model.Nodes[Index];
            FLayoutNode& Out = Model.Layout.Nodes.AddDefaulted_GetRef();
            Out.Key = Node->GetNodePath();
            Out.Position = Node->GetPosition();
            Out.bMovable = Movable.Contains(Node);

            // The Control Rig editor lists a node's pins in four groups: execute pins, outputs,
            // other IO pins, then inputs (URigVMEdGraphNode::AllocateDefaultPins).
            auto Group = [](const URigVMPin* Pin)
            {
                const ERigVMPinDirection Direction = Pin->GetDirection();
                if (Direction == ERigVMPinDirection::IO) { return Pin->IsExecuteContext() ? 0 : 2; }
                return Direction == ERigVMPinDirection::Output ? 1 : 3;
            };
            int32 Row = 0;
            for (int32 Pass = 0; Pass < 4; ++Pass)
            {
                for (const URigVMPin* Pin : Node->GetPins())
                {
                    if (Group(Pin) == Pass)
                    {
                        AddPinRows(Pin, Index, Out, Row, SlotsOfPin);
                    }
                }
            }
            Out.Size = FVector2D(
                FMath::Max(MinWidth, Node->GetNodeTitle().Len() * TitleCharWidth + SidePadding),
                FMath::Max(MinHeight, HeaderHeight + (FMath::Max(Row, 1) + 0.5) * RowHeight));
        }

        for (const URigVMLink* Link : Graph->GetLinks())
        {
            const URigVMPin* Source = Link ? Link->GetSourcePin() : nullptr;
            const URigVMPin* Target = Link ? Link->GetTargetPin() : nullptr;
            const TPair<int32, FPinSlots>* From = FindShownSlots(Source, SlotsOfPin);
            const TPair<int32, FPinSlots>* To = FindShownSlots(Target, SlotsOfPin);
            if (From && To && From->Value.Output != INDEX_NONE && To->Value.Input != INDEX_NONE)
            {
                Model.Layout.Wires.Add({ From->Key, From->Value.Output, To->Key, To->Value.Input });
            }
        }
        return Model;
    }

    FArrangeReport ArrangeRigVMGraph(URigVMGraph* Graph, const TSet<URigVMNode*>& Movable, URigVMController* Controller)
    {
        if (!Graph || !Controller || Movable.Num() == 0)
        {
            return FArrangeReport();
        }
        FRigVMModel Model = BuildRigVMModel(Graph, Movable);
        const FArrangeReport Report = Arrange(Model.Layout, PwGraphLayoutRigVMImpl::RigSpacing());
        for (int32 Index = 0; Index < Model.Nodes.Num(); ++Index)
        {
            const FLayoutNode& Placed = Model.Layout.Nodes[Index];
            if (Placed.bMovable && !Placed.Position.Equals(Model.Nodes[Index]->GetPosition(), 0.01))
            {
                Controller->SetNodePosition(
                    Model.Nodes[Index],
                    Placed.Position,
                    /*bSetupUndoRedo*/ false,
                    /*bMergeUndoAction*/ false,
                    /*bPrintPythonCommand*/ false);
            }
        }
        return Report;
    }
}
