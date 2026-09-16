// Copyright (c) 2026 Alexander Penkin. MIT License.

// CRIRLayoutEngine.h
//
// Auto-layout pass for nodes whose CRIR text omitted `@(x,y)`. Mirrors
// FAGIRLayoutEngine: depth comes from a topo walk over input links; same depth
// stacks vertically by stable name order.

#pragma once

#include "CoreMinimal.h"


class URigVMController;
class URigVMGraph;
class URigVMNode;

struct FCRIRLayoutOptions
{
    int32 OriginX = 0;
    int32 OriginY = 0;
    int32 HorizontalSpacing = 320;
    int32 VerticalSpacing = 180;
};

class FCRIRLayoutEngine
{
public:
    // Lays out only the nodes in `NodesToLayout`. Other nodes in `Graph` act as
    // obstacles for column/row counting (their existing positions are honoured)
    // but are not repositioned. `Controller` applies the moves through
    // SetNodePosition so the editor model fires its NodePositionChanged event.
    static void RunLayout(
        URigVMGraph* Graph,
        const TSet<URigVMNode*>& NodesToLayout,
        URigVMController* Controller);

    static void RunLayout(
        URigVMGraph* Graph,
        const TSet<URigVMNode*>& NodesToLayout,
        URigVMController* Controller,
        const FCRIRLayoutOptions& Options);
};
