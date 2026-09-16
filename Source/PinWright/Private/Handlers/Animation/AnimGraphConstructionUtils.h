// Copyright (c) 2026 Alexander Penkin. MIT License.

// AnimGraphConstructionUtils.h
//
// Shared anim graph construction helpers consumed by both the imperative
// animation.authoring RPCs and the AGIR compiler. Refactored out of
// AnimationAuthoringHandler.cpp; behaviour is preserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Set.h"
#include "Templates/SharedPointer.h"

class FJsonValue;


class UAnimBlueprint;
class UAnimGraphNode_Base;
class UAnimGraphNode_LayeredBoneBlend;
class UAnimGraphNode_LinkedAnimLayer;
class UAnimGraphNode_StateMachine;
class UAnimStateNode;
class UAnimStateNodeBase;
class UAnimStateConduitNode;
class UAnimStateAliasNode;
class UAnimStateTransitionNode;
class UAnimationStateMachineGraph;
class UBlueprint;
class UEdGraph;
class UEdGraphNode;

namespace AnimGraphConstructionUtils
{
    // True iff `Graph` is exactly a `UAnimationGraph` instance whose schema
    // derives from `UAnimationGraphSchema`. State-machine, transition, and
    // blend-space sub-class graphs answer false here — those reach AGIR via
    // state-machine recursion, not as top-level entries. Shared between the
    // AGIR decompiler / layout engine and the dump builder so all three
    // classify "top-level anim graph" identically.
    bool IsTopLevelAnimGraph(const UEdGraph* Graph);

    // Walks `Blueprint->ImplementedInterfaces[i].Graphs` and their child
    // graphs, accumulating every reachable `UEdGraph` (no filtering — caller
    // applies its own classification). Used by AGIR + the anim_graph.json
    // dump aspect to surface anim layer interface override pose graphs that
    // are not also reachable via `Blueprint->FunctionGraphs`.
    void CollectInterfaceLayerGraphs(UBlueprint* Blueprint, TSet<const UEdGraph*>& OutGraphs);

    // Returns the root "AnimGraph" page on the supplied UAnimBlueprint, or nullptr if absent.
    PINWRIGHT_API UEdGraph* GetAnimGraphFromBlueprint(UAnimBlueprint* AnimBP);

    // Resolves the target graph by name on the supplied UAnimBlueprint: empty
    // or "AnimGraph" (case-insensitive) routes to GetAnimGraphFromBlueprint;
    // any other name walks AnimBP->FunctionGraphs by GetName (case-insensitive).
    // Returns nullptr if no graph matches.
    PINWRIGHT_API UEdGraph* ResolveAnimBlueprintGraph(
        UAnimBlueprint* AnimBP, const FString& GraphName);

    // Finds a state machine node by name (matches GetStateMachineName or node title) in the supplied graph.
    PINWRIGHT_API UAnimGraphNode_StateMachine* FindStateMachineNode(UEdGraph* Graph, const FString& Name);

    // Finds a state node by name within the supplied state machine graph.
    PINWRIGHT_API UAnimStateNode* FindStateNode(UAnimationStateMachineGraph* SMGraph, const FString& Name);

    // Creates a state machine on ParentGraph with the given name and editor position. Returns the new
    // editor node with its inner UAnimationStateMachineGraph linked and default entry node populated.
    // ParentGraph must be a UAnimationGraph-family graph; AnimBP must own ParentGraph.
    UAnimGraphNode_StateMachine* CreateStateMachine(UAnimBlueprint* AnimBP, UEdGraph* ParentGraph, FName Name, const FVector2D& Position);

    // Creates a generic UAnimGraphNode_Base-derived editor node of the supplied class on ParentGraph.
    // Position is applied; default pins are allocated. Returns null if NodeClass is not a UAnimGraphNode_Base
    // subclass or ParentGraph is null.
    PINWRIGHT_API UAnimGraphNode_Base* CreateAnimNode(UEdGraph* ParentGraph, UClass* NodeClass, const FVector2D& Position);

    // Outcome of ResolveAnimGraphNodeByTitle.
    enum class EAnimNodeResolveStatus : uint8
    {
        Found,      // exactly one match (exact-title or unique substring) — Node is set
        NotFound,   // no node title matched — Node is null, Candidates empty
        Ambiguous,  // >1 substring match and no single exact-title match — Node is null,
                    // Candidates lists every matching node's list-view title
    };

    struct FAnimNodeResolveResult
    {
        EAnimNodeResolveStatus Status = EAnimNodeResolveStatus::NotFound;
        // The resolved node on Found; null otherwise. May be null even on a
        // title match if the matched node is not a UAnimGraphNode_Base (the
        // caller's Cast then surfaces the wrong-type path).
        UAnimGraphNode_Base* Node = nullptr;
        // The list-view titles of every title-substring match. Single entry on
        // Found, the full candidate list on Ambiguous, empty on NotFound.
        TArray<FString> Candidates;
    };

    // Two-pass exact-then-substring resolution of a `nodeName` against an anim
    // graph, mirroring the disambiguation shipped for the BPIR async-action
    // matcher. Collects every node whose GetNodeTitle(ListView) equals (pass 1)
    // or contains (pass 2) NodeName:
    //   - exactly one exact-title match -> Found (that node) even if other
    //     nodes' titles merely contain the string (exact wins);
    //   - else exactly one substring match -> Found (that node);
    //   - else >1 substring match -> Ambiguous, Candidates lists their titles;
    //   - else -> NotFound.
    // The resolved node's actual list-view title is always Candidates[0] on
    // Found, so callers can echo the real title instead of the raw input.
    PINWRIGHT_API FAnimNodeResolveResult ResolveAnimGraphNodeByTitle(
        UEdGraph* Graph, const FString& NodeName);

    // Creates a state on the supplied state machine graph; renames its bound graph to StateName.
    UAnimStateNode* CreateState(UAnimationStateMachineGraph* MachineGraph, FName StateName, const FVector2D& Position);

    // Creates a transition between FromState and ToState. Pin connections are wired via UAnimStateTransitionNode::CreateConnections.
    // Endpoints accept UAnimStateNodeBase so conduits (UAnimStateConduitNode) can be wired alongside states.
    UAnimStateTransitionNode* CreateTransition(UAnimStateNodeBase* FromState, UAnimStateNodeBase* ToState, const FVector2D& Position);

    // Wires the state machine's entry node to TargetState by connecting the entry node's
    // first pin to the state's input pin via the schema. Returns true iff the connection
    // was made; on failure OutErrorCode is set to one of ENTRY_NODE_NOT_FOUND /
    // STATE_INPUT_PIN_MISSING / CONNECTION_FAILED and OutErrorMessage to a description.
    // Shared by animation.create_state_machine and animation.authoring.set_state_machine_entry
    // so the entry-wiring sequence lives in one place alongside the other SM primitives.
    bool SetStateMachineEntry(UAnimationStateMachineGraph* SMGraph, UAnimStateNode* TargetState,
        FString& OutErrorCode, FString& OutErrorMessage);

    // Creates a conduit on the supplied state machine graph; renames its bound graph to ConduitName.
    UAnimStateConduitNode* CreateConduit(UAnimationStateMachineGraph* MachineGraph, FName ConduitName, const FVector2D& Position);

    // Creates a state alias on the supplied state machine graph; sets StateAliasName so
    // GetStateName() returns AliasName (matching the by-name resolution that transitions
    // and AGIR symbol lookup rely on). The aliased-state set is left empty for the caller
    // to populate via UAnimStateAliasNode::GetAliasedStates() in a post-pass once all
    // referenced states exist.
    UAnimStateAliasNode* CreateStateAlias(UAnimationStateMachineGraph* MachineGraph, FName AliasName, const FVector2D& Position);

    // Wires an upstream output pin to a downstream input pin on the schema. Returns true iff the
    // connection was made. Either side may be any UEdGraphNode (anim graph nodes or schema-internal nodes).
    PINWRIGHT_API bool WirePoseLink(UEdGraphNode* UpstreamNode, FName UpstreamPinName, UEdGraphNode* DownstreamNode, FName DownstreamPinName);

    // Writes a single field on a UAnimGraphNode_Base's runtime FAnimNode_* struct via reflection.
    // Returns an empty string on success; otherwise an error message describing the failure.
    FString WriteAnimNodeFieldByName(UAnimGraphNode_Base* Node, FName FieldName, const FString& ValueAsText);

    // Validates a reflected field using the same resolver, token unwrapping, and ImportText path
    // as WriteAnimNodeFieldByName, but against a scratch copy of the current value. The node is
    // not mutated. Returns an empty string on success; otherwise the same import/resolve error.
    FString ValidateAnimNodeFieldByName(UAnimGraphNode_Base* Node, FName FieldName, const FString& ValueAsText);

    // Applies a JSON value to a single field on a UAnimGraphNode_Base's runtime FAnimNode_* struct.
    PINWRIGHT_API bool ApplyJsonValueToAnimNodeFieldByName(
        UAnimGraphNode_Base* Node,
        FName FieldName,
        const TSharedPtr<FJsonValue>& Value,
        FString& OutError);

    // Toggles "Expose as Pin" on an optional property of an AnimGraph node by walking
    // ShowPinForProperties for the matching PropertyName and delegating to the engine's
    // public UAnimGraphNode_Base::SetPinVisibility (which runs the EvaluateOldShownPins
    // binding-preservation step). Promotes bCanToggleVisibility on demand. Returns false
    // with OutError set to "Node is null" or "OPTIONAL_PIN_NOT_FOUND" on failure.
    PINWRIGHT_API bool ToggleOptionalPinExposed(UAnimGraphNode_Base* Node, FName PropertyName, bool bExposed, FString& OutError);

    // Validates the supplied layer JSON without touching a node. Each layer JSON entry shape:
    // { branchFilters: [{ boneName, blendDepth }] }. Returns an empty FString on success;
    // otherwise a diagnostic ("INVALID_LAYERS: ...").
    PINWRIGHT_API FString ValidateLayeredBlendLayers(
        const TArray<TSharedPtr<FJsonValue>>& LayersJson);

    // Rewrites the LayerSetup / BlendPoses / BlendWeights / BlendMasks arrays on
    // a UAnimGraphNode_LayeredBoneBlend in lock-step to match the supplied
    // LayersJson array. The helper validates the complete input before its first write.
    // The engine's FAnimNode_LayeredBoneBlend::AddPose and SyncBlendMasksAndLayers are not
    // DLL-exported in UE 5.6, so the helper inlines the array-grow pattern using only public
    // state (pattern copied from AGIRCompiler_LayeredBlend.cpp:117-129). The caller is
    // expected to call ReconstructNode() after this returns so per-layer input pose pins
    // repopulate.
    PINWRIGHT_API FString WriteLayeredBlendLayers(
        UAnimGraphNode_LayeredBoneBlend* Node,
        const TArray<TSharedPtr<FJsonValue>>& LayersJson);

    // Applies LayersJson after the caller has successfully run ValidateLayeredBlendLayers.
    // This is the non-fallible mutation half used when the caller must not have a handled error
    // path after another field (such as BlendMode) has been written.
    PINWRIGHT_API void ApplyValidatedLayeredBlendLayers(
        UAnimGraphNode_LayeredBoneBlend* Node,
        const TArray<TSharedPtr<FJsonValue>>& LayersJson);

    // Wires a freshly-created UAnimGraphNode_LinkedAnimLayer to an interface layer by name.
    // Wraps UAnimGraphNode_LinkedAnimLayer::SetupFromLayerId, which is protected on the engine
    // class (only the engine's BP node spawner lambda can normally call it). Populates
    // Node.Layer / Node.Interface / InterfaceGuid / FunctionReference in one shot — required
    // before ReconstructNode synthesises the input-pose pins from the layer's UFunction.
    PINWRIGHT_API void SetupLinkedAnimLayerFromLayerId(UAnimGraphNode_LinkedAnimLayer* LayerNode, FName LayerId);

    // Creates a new override implementation graph for an anim layer interface function on the supplied
    // anim BP. The graph uses UAnimationGraph + UAnimationGraphSchema. Returns the new graph or nullptr on failure.
    UEdGraph* CreateAnimLayerInterfaceImplementationGraph(UAnimBlueprint* AnimBP, UClass* InterfaceClass, FName FunctionName);

    // Clears every UAnimationGraph-class top-level graph reachable from AnimBP — both FunctionGraphs
    // and per-interface implementation graphs — by removing all nodes other than the schema-default
    // UAnimGraphNode_Root. Used by AGIR Replace-mode pre-clear.
    void ClearAnimGraph(UAnimBlueprint* AnimBP);
}
