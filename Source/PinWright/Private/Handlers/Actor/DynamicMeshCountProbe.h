// Copyright (c) 2026 Alexander Penkin. MIT License.

// Reflection-only triangle-count probe for UDynamicMeshComponent, plus the pure
// verdict classifier and cvar scope guard that `actor.duplicate` uses to detect the
// engine's silent placeholder-cube substitution.
//
// WHY THIS EXISTS: UDynamicMesh::ImportCustomProperties (the T3D-paste half of every
// editor duplicate) falls back to a 12-triangle 50-unit box when it cannot recover the
// source mesh, and still returns SUCCESS
// (Engine/Source/Runtime/GeometryFramework/Private/UDynamicMesh.cpp:568 on 5.8, :529 on
// 5.3 — "if we got here we failed. Rather than produce an empty mesh, we generate a
// small cube"). Two independent triggers, both cvar-governed (declared at
// UDynamicMesh.cpp:31-39 on 5.8):
//   - geometry.DynamicMesh.TextBasedDupeTriThreshold (default 200000) — above it the
//     Base64 text-copy fallback is never written on the export side.
//   - geometry.DynamicMesh.DupeStashTimeout (default 5*60 s) — the FDynamicMeshCopyHelper
//     pointer stash the fast path depends on.
// The only user-visible signal is a 5 s editor toast plus a LogGeometry warning, neither
// of which reaches an RPC caller. Detection is therefore done by POST-HOC COMPARISON of
// source vs. duplicate triangle counts rather than by predicting either threshold: that
// catches both triggers and stays correct if Epic changes the defaults.
//
// WHY REFLECTION: the main PinWright module deliberately does not link GeometryFramework
// or DynamicMesh (CLAUDE.md, "Module Split & Integration Gating") so that
// UnrealEditor-PinWright.dll never hard-imports an engine plugin a consumer disabled.
// Never #include a GeometryFramework header from this module.
#pragma once

#include "CoreMinimal.h"

class AActor;
class IConsoleVariable;

namespace DynamicMeshCountProbe
{
    // Total triangle count across every UDynamicMeshComponent on Actor.
    // Returns false (OutTriangles untouched) when GeometryFramework is not loaded in this
    // host, when the actor carries no dynamic mesh component, or when any part of the
    // reflection chain has drifted. In every one of those cases the caller must behave
    // exactly as it did before this probe existed — the failure mode is "no opinion",
    // never "substituted".
    bool TryGetTotalTriangleCount(const AActor* Actor, int64& OutTriangles);

    // Outcome of comparing a duplicate's dynamic-mesh triangle count with its source's.
    enum class EDuplicateMeshVerdict : uint8
    {
        // At least one side could not be probed: no claim either way.
        NotProbed,
        // Both probed and equal: the mesh survived the duplicate.
        Ok,
        // Both probed and different: the engine substituted geometry.
        Substituted
    };

    // Pure decision function, split out from the handler so the truth table is unit
    // testable with no world and no GeometryFramework present.
    EDuplicateMeshVerdict ClassifyDuplicate(bool bSourceProbed, bool bDupProbed,
                                            int64 SourceTris, int64 DupTris);

    // Raises geometry.DynamicMesh.TextBasedDupeTriThreshold above RequiredTriangles for
    // this object's lifetime and restores the previous value in the destructor.
    //
    // Opt-in only (actor.duplicate's allowSlowLargeMeshCopy): the engine calls the Base64
    // text path "quite slow" in the cvar's own help string (UDynamicMesh.cpp:35) and it
    // costs O(mesh) memory. Mutating a global cvar for the span of one RPC is safe here
    // for the same reason the plugin's other global-state guards are: RPCs are serialized
    // onto the game thread by the dispatcher, and the restore is unconditional.
    class FScopedDupeTriThresholdRaise
    {
    public:
        explicit FScopedDupeTriThresholdRaise(int64 RequiredTriangles);
        ~FScopedDupeTriThresholdRaise();

        FScopedDupeTriThresholdRaise(const FScopedDupeTriThresholdRaise&) = delete;
        FScopedDupeTriThresholdRaise& operator=(const FScopedDupeTriThresholdRaise&) = delete;

        // False when the cvar does not exist in this host (GeometryFramework not loaded),
        // in which case the guard is inert and the opt-in changed nothing.
        bool IsActive() const { return CVar != nullptr; }

    private:
        IConsoleVariable* CVar = nullptr;
        int32 PreviousThreshold = 0;
        // Only a guard that actually wrote the cvar restores it; a host already running a
        // more permissive threshold is left alone in both directions.
        bool bRaised = false;
    };
}
