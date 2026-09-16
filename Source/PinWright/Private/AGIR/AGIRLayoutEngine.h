// Copyright (c) 2026 Alexander Penkin. MIT License.

// AGIRLayoutEngine.h
//
// Auto-layout for nodes emitted by `FAGIRCompiler`. Mirrors `FMGIRLayoutEngine`:
// only repositions nodes whose `NodePosX/NodePosY == 0` (i.e. compile did not
// set them from `@(x, y)` annotations). Lays out every anim graph reachable
// on the supplied `UAnimBlueprint` — top-level anim graphs, anim layer
// interface override graphs, and state-machine inner graphs.

#pragma once

#include "CoreMinimal.h"

class UAnimBlueprint;
class UEdGraph;


struct FAGIRLayoutOptions
{
    int32 OriginX = 0;
    int32 OriginY = 0;
    int32 HorizontalSpacing = 320;
    int32 VerticalSpacing = 180;
};

class FAGIRLayoutEngine
{
public:
    static void Layout(UAnimBlueprint* AnimBP);
    static void Layout(UAnimBlueprint* AnimBP, const FAGIRLayoutOptions& Options);

    // Lays out a single graph in isolation. Used by `Layout(UAnimBlueprint*)`
    // for each reachable anim graph and is exposed for callers that already
    // hold a graph pointer.
    static void LayoutGraph(UEdGraph* Graph, const FAGIRLayoutOptions& Options);
};
