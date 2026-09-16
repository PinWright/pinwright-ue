// Copyright (c) 2026 Alexander Penkin. MIT License.

// AGIRTextEmitter.h
//
// Per-family AGIR text emitter for UAnimBlueprint contents. Walks
// UAnimGraphNode_Base subclasses and emits one AGIR line per node, recursing
// into state machines for nested `state_machine { ... }` blocks. Top-level
// entry block kind classification (`anim_graph` / `anim_layer` / `anim_function`)
// is the caller's responsibility — the AGIR decompiler in Wave 5A drives this.
//
// Reflective property emit routes through `FIrTextUtils::AppendReflectedFields`
// with AGIR-specific pose-link rejection. Runtime `FPoseLink` /
// `FComponentSpacePoseLink` properties are excluded from reflection and
// re-emitted from editor pin `LinkedTo` walks because the runtime `LinkID`
// field is post-compile only.
//
// Data-pin bindings (a wired variable getter, a property-access binding) are
// likewise invisible to reflection — the driven property keeps its literal
// default in the runtime struct — so they come from `AGIRPinBindings`, whose
// header documents the two AGIR spellings. A bound property's literal is
// suppressed from the reflected pass; links AGIR cannot spell accumulate in
// `Warnings` for the decompiler to relay.

#pragma once

#include "CoreMinimal.h"
#include "AGIR/AGIROpcodes.h"


class UAnimBlueprint;
class UEdGraph;
class UEdGraphNode;
class UAnimGraphNode_Base;
class UAnimGraphNode_StateMachineBase;
class UAnimStateNode;
class UAnimStateAliasNode;
class UAnimStateConduitNode;
class UAnimStateTransitionNode;
class UAnimationBlendSpaceSampleGraph;

class FAGIRTextEmitter
{
public:
    explicit FAGIRTextEmitter(UAnimBlueprint* InAnimBP);

    // Top-level entry block emit (`entry anim_graph <name-token> { ... }` etc.). Kind
    // is supplied by the caller because the same UEdGraph can be reachable as
    // either AnimGraph (top-level page) or AnimLayer (interface override).
    FString EmitGraph(UEdGraph* Graph, EAGIREntryKind Kind);

    // Per-node single-line emit; used by EmitGraph and exposed for tests.
    FString EmitNode(UAnimGraphNode_Base* Node);

    // Recursive nested-block emit for state machines and their inner contents.
    FString EmitStateMachine(UAnimGraphNode_StateMachineBase* MachineNode);

    // Static helper: builds AGIR-local id token (`%<mnemonic>_<n>`).
    static FString MakeLocalId(const FString& Mnemonic, int32 Counter);

    // Lossy-read reports accumulated across every EmitGraph call on this
    // emitter (currently: wired data pins whose driver AGIR cannot spell).
    // The decompiler appends these to its own result so a reader can tell an
    // incomplete text from a complete-and-empty one.
    const TArray<FString>& GetWarnings() const { return Warnings; }

private:
    UAnimBlueprint* AnimBlueprint = nullptr;
    TArray<FString> Warnings;

    // Pre-populated by EmitGraph before per-node emit so pose-link refs can
    // resolve `LinkedTo[0]` upstream nodes to a stable AGIR-local id.
    TMap<UEdGraphNode*, FString> LocalIds;
    TMap<FString, int32> LocalMnemonicCounters;

    // Lookup or allocate a stable local id for a node within the current graph.
    FString IdFor(UEdGraphNode* Node);

    // Pre-walk a graph's nodes assigning local ids in deterministic order.
    void AssignLocalIds(UEdGraph* Graph);

    // Per-family dispatch — cast-cascade returns the formatted line; falls
    // back to EmitGenericCall for the long tail of UAnimGraphNode_* subclasses.
    FString EmitBlendSpaceGraph(UAnimGraphNode_Base* Node);
    FString EmitLayeredBlend(UAnimGraphNode_Base* Node);
    FString EmitLinkedAnim(UAnimGraphNode_Base* Node);
    FString EmitLinkedInputPose(UAnimGraphNode_Base* Node);
    FString EmitSaveCachedPose(UAnimGraphNode_Base* Node);
    FString EmitUseCachedPose(UAnimGraphNode_Base* Node);
    FString EmitOutput(UAnimGraphNode_Base* Node);
    FString EmitGenericCall(UAnimGraphNode_Base* Node);

    // Inner-block emit helpers — used recursively by EmitStateMachine.
    FString EmitState(UAnimStateNode* StateNode);
    FString EmitPoseResultBodyGraph(UEdGraph* BodyGraph, UClass* ResultSinkClass);
    FString EmitTransition(UAnimStateTransitionNode* TransitionNode);
    FString EmitConduit(UAnimStateConduitNode* ConduitNode);
    FString EmitStateAlias(UAnimStateAliasNode* AliasNode);

    // Sample sub-graph emission for blend_space block bodies. Walks
    // the BlendSpaceGraph's owned UAnimationBlendSpaceSampleGraph entries and
    // emits a `sample_graph <name-token> { ... output %<mnemonic>_<n> }`
    // block per sample.
    FString EmitBlendSpaceSampleGraph(UAnimationBlendSpaceSampleGraph* SampleGraph);

    // Builds the trailing `guid="..." @(x, y)` annotation common to all node lines.
    FString FormatNodeAnnotation(UEdGraphNode* Node) const;

    // The full argument list for one node, in emit order: pose links, then
    // data-pin bindings, then reflected CDO-delta fields with every bound
    // property's (now dead) literal suppressed. `bIncludePoseLinks` is false
    // for the families whose pose input is not an editor pin (use_cached_pose,
    // linked_input_pose).
    void AppendNodeFields(UAnimGraphNode_Base* Node, TArray<FString>& OutFields, bool bIncludePoseLinks);

    // Walks the runtime `FAnimNode_*` struct on Node, appending non-pose-link
    // reflected properties whose values differ from the CDO to OutFields as
    // `Name: Value` strings. Mirrors MGIR's CDO-delta emit. Properties listed
    // in SuppressedProperties are skipped — a bound pin's struct value is
    // overwritten every frame, so emitting it would print a dead literal
    // beside the live binding under the same key.
    void AppendReflectedNodeFields(
        UAnimGraphNode_Base* Node,
        const TSet<FName>& SuppressedProperties,
        TArray<FString>& OutFields) const;

    // Walks editor pose-input pins on Node (skipping output pins), looking up
    // upstream UAnimGraphNode_Base via `Pin->LinkedTo[0]`, and appending
    // `<PinName>: %<mnemonic>_<n>` entries to OutFields.
    void AppendPoseLinkFields(UAnimGraphNode_Base* Node, TArray<FString>& OutFields);
};
