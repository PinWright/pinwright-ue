// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "IrCore/IrCompileDiagnostic.h"

// AGIR opcodes — one per anim-node family. The generic `Call` opcode covers the
// long tail of `UAnimGraphNode_*` subclasses via reflective property iteration;
// the rest are special-cased because their content is structurally distinct
// (sub-graphs, cached-pose linkage, layered-blend pose targets, etc.).
enum class EAGIROpcode : uint8
{
    Call,             // Generic `call <ClassPath>(reflected fields)` for the ~80 long-tail subclasses.
    StateMachine,     // `state_machine <name-token> { ... }` block opcode.
    BlendSpace,       // `blend_space <name-token> asset=<path> { ... }` (Phase 3 compile rejects with AGIR_SUBGRAPH_NOT_SUPPORTED).
    LayeredBlend,     // `layered_blend <name-token> { ... }` (decompile-only).
    LinkedAnim,       // `linked_anim instance_class=<class> layer_name=<name>` (no owned sub-graph).
    LinkedInputPose,  // Function-side counterpart to LinkedAnim.
    SaveCachedPose,   // `save_cached_pose name=<cache_name>`.
    UseCachedPose,    // `use_cached_pose source=<cache_name>`.
    Output,           // Root pose result (terminator).
    State,            // `state <name-token> { ... }` (only inside state_machine block).
    Transition,       // `transition <name-token> -> <name-token> priority=N rule=<graph_name>` (only inside state_machine block).
    Conduit,          // `conduit <name-token> rule=<graph_name>` (only inside state_machine block).
    StateAlias,       // `state_alias <name-token> aliases="A,B,..." global_alias=true|false` (only inside state_machine block).
    BlendSpaceSampleGraph, // `sample_graph <name-token> { ... }` — child of a blend_space block; body is the sample sub-graph's anim instructions.
    CustomTransitionBody,  // `custom_transition_body { ... }` — child of a transition with logic_type=TLT_Custom; body is the custom transition graph's anim instructions.
    Implements,            // `implements <classpath>` — child of an InterfaceManifest entry block; declares one anim layer interface implemented by the AnimBP.
};

// Top-level block kinds — analogous to MGIR's Material / Function. AnimLayer is
// for anim layer interface override pose graphs; AnimFunction is for anim
// blueprint functions that return pose links.
enum class EAGIREntryKind : uint8
{
    AnimGraph,
    AnimLayer,
    AnimFunction,
    InterfaceManifest, // Top-level `interfaces { implements <classpath> ... }` block — declares the AnimBP's implemented anim layer interfaces.
};

struct FAGIRArg
{
    FString Name;
    FString Value;
};

struct FAGIRParseError : public FIrCompileDiagnostic
{
    FString Code;

    FAGIRParseError() = default;
    FAGIRParseError(int32 InLine, const FString& InMessage, const FString& InCode = FString())
        : FIrCompileDiagnostic(InLine, InMessage)
        , Code(InCode)
    {
    }
};

// A single AGIR instruction. state_machine instructions own state/conduit/
// transition instructions as siblings via `Children`. Transitions reference
// state names via args (`from`/`to`), not parent-child. Transition rule
// bodies themselves are NOT inlined — they are referenced by graph name and
// walked via BPIR.
struct FAGIRInstruction
{
    EAGIROpcode Opcode = EAGIROpcode::Call;
    FString ResultName;       // %nNN AGIR-local id (without %) — empty for terminators.
    FString SymbolName;       // Class path for Call; parsed name token for State/StateMachine/etc.
    TArray<FAGIRArg> Args;
    TArray<TSharedPtr<FAGIRInstruction>> Children;
    int32 SourceLine = -1;
    bool bHasPosition = false;
    FVector2D Position = FVector2D::ZeroVector;
    FString NodeGuid;         // Round-trip aid for cross-graph pointer fields (e.g. K2Node_TransitionRuleGetter::AssociatedAnimAssetPlayerNode).
};

struct FAGIREntryBlock
{
    EAGIREntryKind Kind = EAGIREntryKind::AnimGraph;
    FString Name;
    TArray<TSharedPtr<FAGIRInstruction>> Instructions;
};
