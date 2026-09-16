// Copyright (c) 2026 Alexander Penkin. MIT License.

// AGIRCliffHandlers.h
//
// Per-family compile handlers for the AGIR cliff opcodes (blend_space,
// layered_blend, linked_anim, linked_input_pose, save/use_cached_pose) split
// out of AGIRCompiler.cpp into per-family translation units so Wave 2 chunks
// stay file-disjoint. Wave 1 stubs each handler to return
// AGIR_SUBGRAPH_NOT_SUPPORTED; Wave 2 fills in the bodies.

#pragma once

#include "CoreMinimal.h"
#include "AGIR/AGIRCompiler.h"
#include "AGIR/AGIROpcodes.h"

class UAnimBlueprint;
class UAnimGraphNode_Base;
class UAnimGraphNode_SaveCachedPose;
class UAnimGraphNode_UseCachedPose;
class UEdGraph;
class UEdGraphNode;

namespace AGIRCliff
{
// Pose-link wire deferred to Pass 2: created during Pass 1 when an arg's value
// starts with `%`, resolved against the symbol map after every node has been
// instantiated so forward references work. Promoted from the AGIRCompiler.cpp
// anonymous namespace so per-family handlers in sibling .cpp files can see it.
struct FAGIRPendingPoseWire
{
    UAnimGraphNode_Base* DownstreamNode = nullptr;
    FName InputPinName;
    FString UpstreamRef; // raw `%nNN` or `%name` token
    int32 SourceLine = -1;
};

using FAGIRSymbolMap = TMap<FString, UEdGraphNode*>;
using FAGIRPendingPoseWires = TArray<FAGIRPendingPoseWire>;
using FAGIRCacheNameMap = TMap<FString, UAnimGraphNode_SaveCachedPose*>;

// A use_cached_pose whose SaveCachedPoseNode linkage is deferred to a post-loop
// Pass-2 resolved against the completed FAGIRCacheNameMap, mirroring how
// FAGIRPendingPoseWire defers pose-pin wiring. On the canonical locomotion
// emission order the use_cached_pose is emitted BEFORE its save_cached_pose (a
// forward reference), so the save node is not yet in the block-scoped map when
// the use compiles; resolving all use nodes after every instruction in the
// block has compiled makes the linkage order-independent. Without it a
// forward-referenced use node's weak ptr is left null at AGIR compile time
// (violating the invariant the backward-ref RoundTrip test asserts) and the
// warm re-decompile drops the cache-name source= label to empty.
struct FAGIRPendingCacheLink
{
    UAnimGraphNode_UseCachedPose* UseNode = nullptr;
    FString CacheName;
    int32 SourceLine = -1;
};
using FAGIRPendingCacheLinks = TArray<FAGIRPendingCacheLink>;

// Generic diagnostic steer attached to AGIR_SYMBOL_NOT_FOUND pose-resolution
// failures. The raw "did not resolve" message is locally true but cannot tell
// "you wrote a bad %-ref" from "the decompiler emitted a %-ref its own compiler
// can't reconsume" (a round-trip gap). It is class-level — true for ANY
// unresolved pose ref — so every pose-resolution site (top-level, blend-space
// sample graphs, the pin-resolver missing-node relay) attaches it, and it stays
// correct as individual round-trip bugs are fixed or appear. anim.compile_agir
// relays it as a `hint` field; see docs/wiki-src/anim.md "Round-trip
// limitations" and E-agir-roundtrip-symbol-not-found-undiagnosable. Declared
// here (shared by the per-family handlers) so all pose-resolution sites use the
// one definition rather than duplicating the literal per translation unit.
extern const TCHAR* const GAGIRPoseSymbolNotFoundHint;

// Per-family Pass-1 handlers. Each creates the editor node, applies args
// (queueing pose wires for Pass 2), and returns true on success. On error the
// handler populates OutResult and returns false; the caller propagates.
bool CompileBlendSpaceInstruction(
    UAnimBlueprint* AnimBP,
    UEdGraph* TargetGraph,
    const FAGIRInstruction& Inst,
    FAGIRSymbolMap& Symbols,
    FAGIRPendingPoseWires& PendingWires,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings,
    FAGIRCompileResult& OutResult);

bool CompileLayeredBlendInstruction(
    UAnimBlueprint* AnimBP,
    UEdGraph* TargetGraph,
    const FAGIRInstruction& Inst,
    FAGIRSymbolMap& Symbols,
    FAGIRPendingPoseWires& PendingWires,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings,
    FAGIRCompileResult& OutResult);

bool CompileLinkedAnimInstruction(
    UAnimBlueprint* AnimBP,
    UEdGraph* TargetGraph,
    const FAGIRInstruction& Inst,
    FAGIRSymbolMap& Symbols,
    FAGIRPendingPoseWires& PendingWires,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings,
    FAGIRCompileResult& OutResult);

bool CompileLinkedInputPoseInstruction(
    UAnimBlueprint* AnimBP,
    UEdGraph* TargetGraph,
    const FAGIRInstruction& Inst,
    FAGIRSymbolMap& Symbols,
    FAGIRPendingPoseWires& PendingWires,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings,
    FAGIRCompileResult& OutResult);

bool CompileSaveCachedPoseInstruction(
    UAnimBlueprint* AnimBP,
    UEdGraph* TargetGraph,
    const FAGIRInstruction& Inst,
    FAGIRSymbolMap& Symbols,
    FAGIRPendingPoseWires& PendingWires,
    FAGIRCacheNameMap& CacheNameMap,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings,
    FAGIRCompileResult& OutResult);

bool CompileUseCachedPoseInstruction(
    UAnimBlueprint* AnimBP,
    UEdGraph* TargetGraph,
    const FAGIRInstruction& Inst,
    FAGIRSymbolMap& Symbols,
    FAGIRPendingPoseWires& PendingWires,
    FAGIRPendingCacheLinks& PendingCacheLinks,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings,
    FAGIRCompileResult& OutResult);
}
