// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwGraphLayoutRigVM.h - PwGraphLayout adapter for RigVM (Control Rig) graphs.
//
// Execute-context pins are flow; every other pin is data. Rows follow the editor's order (execute
// pins, outputs, other IO pins, inputs). An IO pin occupies one row on both
// sides, and an expanded pin's sub-pins take rows of their own (a link to a collapsed sub-pin lands
// on its nearest shown parent). Sizes are estimated from the title and the row count, with the
// width of the value editors RigVM nodes draw. Comment nodes are left out.

#pragma once

#include "CoreMinimal.h"
#include "Layout/PwGraphLayout.h"

class URigVMController;
class URigVMGraph;
class URigVMNode;

namespace PwGraphLayout
{
    struct FRigVMModel
    {
        FLayoutGraph Layout;
        // Parallel to Layout.Nodes; ordered by node path.
        TArray<URigVMNode*> Nodes;
    };

    PINWRIGHT_API FRigVMModel BuildRigVMModel(URigVMGraph* Graph, const TSet<URigVMNode*>& Movable);

    // Arranges Movable; every other node of Graph is a fixed obstacle. Positions go through
    // Controller->SetNodePosition without its undo bracket, like the rest of a CRIR compile.
    PINWRIGHT_API FArrangeReport ArrangeRigVMGraph(
        URigVMGraph* Graph, const TSet<URigVMNode*>& Movable, URigVMController* Controller);
}
