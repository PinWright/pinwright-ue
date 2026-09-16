// Copyright (c) 2026 Alexander Penkin. MIT License.

// DecompilerTypes.h - Shared types for the Blueprint decompiler

#pragma once

#include "CoreMinimal.h"

class UEdGraphPin;
class UEdGraphNode;

// Shared knot-traversal utility used by both BpirDecompiler and BpirTextEmitter.
namespace BpirDecompiler::Helpers
{
    // Walks backward through chained UK2Node_Knot reroute nodes starting from a
    // data SourcePin (the result of Pin->LinkedTo[0] on a non-knot input). Returns
    // the upstream pin whose owning node is no longer a knot (the real source), OR
    // nullptr when the chain breaks. OutDeadEndKnotInput receives the KnotInput pin
    // of the terminal knot in the dead-end case so the caller can recurse on its
    // default value.
    UEdGraphPin* FollowKnotsBackward(UEdGraphPin* SourcePin, UEdGraphPin*& OutDeadEndKnotInput);

    // True when the node's runtime semantics are "invoke once per linked source"
    // so the decompiler must fan out one statement per LinkedTo entry on Self
    // rather than warning about only the first being used. Mirrors UE's
    // CanFunctionSupportMultipleTargets test: UK2Node_CallFunction reports it via
    // AllowMultipleSelfs(false); UK2Node_BaseMCDelegate dispatcher nodes hardcode
    // it true. The K2Node_BaseMCDelegate probe is gated by __has_include because
    // not all UE 5.x source trees expose the header.
    bool NodeSupportsMultiSelf(UEdGraphNode* Node);
}

// Semantic classification of a Blueprint graph node for decompilation
enum class ENodeSemantics : uint8
{
    FunctionCall,
    VariableGet,
    VariableSet,
    Branch,
    ForEach,
    WhileLoop,
    Switch,
    Sequence,
    Cast,
    Return,
    Latent,
    Timeline,
    Gate,
    DoOnce,
    FlipFlop,
    Dispatcher,
    MacroInstance,
    Comment,
    Knot,
    TunnelEntry,    // Entry tunnel in macro graph (UK2Node_Tunnel, bCanHaveOutputs=true)
    TunnelExit,     // Exit tunnel in macro graph (UK2Node_Tunnel, bCanHaveInputs=true)
    Unknown
};
