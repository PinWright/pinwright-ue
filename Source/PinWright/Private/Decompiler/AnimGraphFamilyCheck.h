// Copyright (c) 2026 Alexander Penkin. MIT License.

// AnimGraphFamilyCheck.h
//
// Shared schema-based predicate identifying graphs that AGIR (not BPIR) is
// responsible for. Lifted out of `BpirDecompiler.cpp` and `BpirCompiler.cpp`
// where the same check appeared duplicated; both call sites now include this
// header. The schema check matches `UAnimationGraphSchema` and its anim-side
// subclasses (`UAnimationStateGraphSchema`, `UAnimationCustomTransitionSchema`).
// Conduit and transition-rule graphs deliberately use K2-derived schemas
// (`UAnimationConduitGraphSchema`, `UAnimationTransitionSchema`) — those carry
// boolean rule logic and BPIR walks them as normal K2 graphs. The explicit
// class check covers `UAnimationStateMachineGraph`, whose schema
// (`UAnimationStateMachineSchema`) is `UEdGraphSchema`-derived and so does
// not hit the schema chain.

#pragma once

#include "CoreMinimal.h"


#include "AnimationGraphSchema.h"
#include "AnimationStateMachineGraph.h"
#include "EdGraph/EdGraph.h"

namespace BpirAnimGraphFamily
{
    inline bool IsAnimGraphFamily(const UEdGraph* Graph)
    {
        if (!Graph)
        {
            return false;
        }
        if (Graph->Schema && Graph->Schema->IsChildOf(UAnimationGraphSchema::StaticClass()))
        {
            return true;
        }
        if (Graph->IsA<UAnimationStateMachineGraph>())
        {
            return true;
        }
        return false;
    }
}
