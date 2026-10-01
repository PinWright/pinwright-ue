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

    constexpr double HeaderHeight = 32.0;
    constexpr double RowHeight = 24.0;
    constexpr double MinWidth = 160.0;
    constexpr double MinHeight = 64.0;
    constexpr double TitleCharWidth = 8.0;
    constexpr double SidePadding = 48.0;

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

            int32 Row = 0;
            for (const URigVMPin* Pin : Node->GetPins())
            {
                const ERigVMPinDirection Direction = Pin->GetDirection();
                const bool bInput = Direction == ERigVMPinDirection::Input || Direction == ERigVMPinDirection::IO
                    || Direction == ERigVMPinDirection::Visible;
                const bool bOutput = Direction == ERigVMPinDirection::Output || Direction == ERigVMPinDirection::IO;
                if (!bInput && !bOutput)
                {
                    continue;
                }
                const EWireKind Kind = Pin->IsExecuteContext() ? EWireKind::Flow : EWireKind::Data;
                const double Centre = HeaderHeight + (Row + 0.5) * RowHeight;
                FPinSlots Slots;
                if (bInput) { Slots.Input = Out.Pins.Add({ EPinSide::Input, Kind, Centre }); }
                if (bOutput) { Slots.Output = Out.Pins.Add({ EPinSide::Output, Kind, Centre }); }
                SlotsOfPin.Add(Pin, TPair<int32, FPinSlots>(Index, Slots));
                ++Row;
            }
            Out.Size = FVector2D(
                FMath::Max(MinWidth, Node->GetNodeTitle().Len() * TitleCharWidth + SidePadding),
                FMath::Max(MinHeight, HeaderHeight + (FMath::Max(Row, 1) + 0.5) * RowHeight));
        }

        for (const URigVMLink* Link : Graph->GetLinks())
        {
            const URigVMPin* Source = Link ? Link->GetSourcePin() : nullptr;
            const URigVMPin* Target = Link ? Link->GetTargetPin() : nullptr;
            const TPair<int32, FPinSlots>* From = Source ? SlotsOfPin.Find(Source->GetRootPin()) : nullptr;
            const TPair<int32, FPinSlots>* To = Target ? SlotsOfPin.Find(Target->GetRootPin()) : nullptr;
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
