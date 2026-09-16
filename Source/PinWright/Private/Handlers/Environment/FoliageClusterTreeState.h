// Copyright (c) 2026 Alexander Penkin. MIT License.

// FoliageClusterTreeState.h - the CLUSTER-TREE layer of a foliage response, and the rebuild the
// engine's add path leaves to its caller.
//
// WHY THIS EXISTS. A HISM does not draw from its instance array; it draws from the cluster tree
// built off that array. `FFoliageInfo::AddInstancesImpl` brackets every add in
// `Implementation->BeginUpdate()` / `EndUpdate()`
// (Runtime/Foliage/Private/InstancedFoliage.cpp:2306-2326), and `FFoliageStaticMesh::BeginUpdate`
// clears `Component->bAutoRebuildTreeOnInstanceChanges` (:1355-1371) for exactly the window in
// which every instance is added. `UHierarchicalInstancedStaticMeshComponent::AddInstance` sets
// `bIsOutOfDate` and then rebuilds ONLY behind that flag
// (Runtime/Engine/Private/HierarchicalInstancedStaticMesh.cpp:2468-2499), and `EndUpdate`
// restores the flag without rebuilding (:1373-1388). So a batch of adds ends with the array full
// and the tree untouched, and a fresh info exits with `NumBuiltInstances == 0` - a scatter that
// is stored, saved and reloaded correctly, and draws NOTHING in this session.
//
// This is an API contract the caller owns, not an engine bug: `AddInstancesImpl` is the only
// bracketed mutator in that file that does not close with a `Refresh`. `RemoveInstancesImpl`
// does (`:2505-2508`), `FFoliageInfo::DuplicateInstances` - which adds through the same
// `AddInstance` in a loop - does (`:2575-2588`), and the engine's own procedural placement passes
// `InRebuildFoliageTree = true`
// (Editor/FoliageEdit/Private/ProceduralFoliageEditorLibrary.cpp:72).
//
// WHICH LAYER EACH FOLIAGE COUNT IS ON, because there are now four and three of them are one
// subtraction apart. Every foliage verb's counts stack like this, outermost first:
//
//   count / instances_count / instancesPlaced  FFoliageInfo::Instances - the editor-side LEDGER.
//   renderedInstanceCount                      Component->GetInstanceCount(), which is
//                                              PerInstanceSMData.Num() - a SECOND ARRAY on the
//                                              component. Written in lockstep with the ledger by
//                                              a single AddInstance, which is why
//                                              ledgerMatchesRendered cannot go false on an add.
//   builtInstanceCount        (THIS HEADER)    Component->NumBuiltInstances - "the number of
//                                              instances in the ClusterTree"
//                                              (HierarchicalInstancedStaticMeshComponent.h:156-158).
//                                              MEMBERSHIP: an instance below this line is not in
//                                              the tree and cannot be drawn from any camera.
//   expectedDrawnInstances    (ScalabilityDensityCVars.h) CULLING, one layer further in: of the
//                                              instances that ARE in the tree, how many survive
//                                              the foliage.DensityScale random draw
//                                              FClusterBuilder applies while building it.
//
// THE TWO ARE NOT INTERCHANGEABLE AND MUST NOT BE FOLDED TOGETHER. `builtInstanceCount` reads
// `NumBuiltInstances`, which is the PRE-density figure; the engine keeps the post-density one
// next door as `NumBuiltRenderInstances` ("normally equal to NumBuiltInstances, but can be lower
// if density scaling is in effect", :160-161). Reading `NumBuiltInstances` on purpose is what
// keeps the density cull reported once, by `expectedDrawnInstances`, instead of twice. A caller
// tells them apart by what a shortfall means: `builtInstanceCount < renderedInstanceCount` is a
// STALE TREE and is repaired by rebuilding (or by saving the level, which
// `HISM::Serialize` does on its way to disk, :2139-2143); `expectedDrawnInstances <
// renderedInstanceCount` is a scalability SETTING and is repaired by raising
// foliage.DensityScale. Neither is a camera or view-frustum figure - no field here is.
//
// Reporting rules are ScalabilityDensityCVars.h's, which are LightingHandler.cpp's: publish the
// MEASURED value beside the requested one, OMIT rather than zero when the host cannot answer,
// and warn naming the remedy.
#pragma once

#include "CoreMinimal.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "InstancedFoliage.h"

namespace PinWrightFoliageClusterTree
{
    // What a foliage response knows about the cluster trees behind its scope. Filled by the
    // caller, like FFoliageDensityScope, because the verbs reach their infos differently (one
    // resolved type for paint / add_instances, a walk over every FFoliageInfo for get_instances).
    struct FFoliageClusterTreeScope
    {
        // False once anything in scope holds drawn instances this helper cannot read a tree off.
        // builtInstanceCount is then OMITTED - a partial sum would read as a divergence the level
        // does not have.
        bool bMeasured = true;
        // True once at least one HISM was actually read. With no component anywhere there is
        // nothing to report and nothing to warn about: a fresh type before its first add.
        bool bSawComponent = false;
        // Sum of NumBuiltInstances over the scope's HISMs - the instances in the built trees.
        int32 BuiltInstances = 0;
        // Sum of GetInstanceCount() over the same components, so the report compares like with
        // like even when the caller's own ledger field counts something slightly different
        // (get_instances excludes orphans from `count`, for instance).
        int32 ComponentInstances = 0;
        // AND of IsTreeFullyBuilt() over the scope. False means at least one tree is known stale,
        // which is true even when the counts happen to line up (a removal that shrank the array
        // back onto a stale NumBuiltInstances).
        bool bAllTreesUpToDate = true;
        // Why bMeasured went false: an info whose implementation is not the StaticMesh one, so
        // its drawn instances live behind FISMClientHandle or on spawned actors instead of on a
        // HISM cluster tree. Named in the warning so the omission is not mistaken for a bug.
        bool bSawNonMeshImpl = false;
    };

    // Folds one foliage info into the scope. Const because every read here is a read.
    inline void NoteFoliageClusterTreeForInfo(FFoliageClusterTreeScope& Scope,
        const FFoliageInfo& Info)
    {
        if (!Info.IsInitialized())
        {
            // No implementation yet: FFoliageStaticMesh creates its HISM lazily inside the first
            // AddInstance, so a type nothing has been placed on legitimately has none. Nothing
            // stored, nothing drawn, nothing to build - contributes zero rather than making the
            // whole scope unmeasurable.
            return;
        }

        // Null for every non-static-mesh impl type (FFoliageInfo::GetComponent tests
        // EFoliageImplType::StaticMesh, InstancedFoliage.cpp:2039-2048).
        const UHierarchicalInstancedStaticMeshComponent* Component = Info.GetComponent();
        if (!Component)
        {
            if (Info.Implementation->GetInstanceCount() > 0)
            {
                Scope.bMeasured = false;
                Scope.bSawNonMeshImpl = true;
            }
            return;
        }

        Scope.bSawComponent = true;
        Scope.ComponentInstances += Component->GetInstanceCount();
        Scope.BuiltInstances += Component->NumBuiltInstances;
        Scope.bAllTreesUpToDate = Scope.bAllTreesUpToDate && Component->IsTreeFullyBuilt();
    }

    // Publishes the cluster-tree block onto a foliage response. RequestedFieldName is the counter
    // the response already carries (instances_count, instancesPlaced, renderedInstanceCount, ...)
    // so the warning names the field the caller is about to misread as "what is on screen".
    inline void AddFoliageClusterTreeReport(const TSharedPtr<FJsonObject>& Resp,
        const FFoliageClusterTreeScope& Scope, const TCHAR* RequestedFieldName)
    {
        if (!Resp.IsValid())
        {
            return;
        }

        if (!Scope.bMeasured)
        {
            if (Scope.bSawNonMeshImpl)
            {
                Resp->SetStringField(TEXT("clusterTreeWarning"), FString::Printf(
                    TEXT("builtInstanceCount and clusterTreeUpToDate are OMITTED, not zero: at ")
                    TEXT("least one foliage type in this scope is not an instanced-static-mesh ")
                    TEXT("type, so its instances are not drawn from a HISM cluster tree this verb ")
                    TEXT("can read. %s is still the count that was stored. Scope the read to one ")
                    TEXT("instanced-static-mesh foliage type to get the cluster-tree figures."),
                    RequestedFieldName));
            }
            return;
        }

        if (!Scope.bSawComponent)
        {
            // Nothing in scope has a component at all, so there is no tree to describe and no
            // divergence to warn about. Omitted rather than published as a pair of zeroes that
            // would read as "built nothing".
            return;
        }

        Resp->SetNumberField(TEXT("builtInstanceCount"), Scope.BuiltInstances);
        Resp->SetBoolField(TEXT("clusterTreeUpToDate"), Scope.bAllTreesUpToDate);

        if (Scope.BuiltInstances == Scope.ComponentInstances && Scope.bAllTreesUpToDate)
        {
            return;
        }

        // Deliberately does NOT claim RequestedFieldName equals ComponentInstances: on a write
        // verb the named field counts THIS call's batch while the components hold the whole
        // type. Both measured numbers are stated outright instead.
        Resp->SetStringField(TEXT("clusterTreeWarning"), FString::Printf(
            TEXT("The foliage components in this scope hold %d instances but only %d of them are ")
            TEXT("in the built HISM cluster tree%s, so %s does NOT describe what is on screen. A ")
            TEXT("HISM draws from the cluster tree, not from its instance array, and the ")
            TEXT("difference is drawn from no camera at any distance - while ledgerMatchesRendered ")
            TEXT("compares two arrays a single AddInstance writes together and stays true ")
            TEXT("throughout. This is a STALE TREE, a different layer from expectedDrawnInstances, ")
            TEXT("which is the foliage.DensityScale cull applied to instances that ARE in the ")
            TEXT("tree. Rebuild it by adding or painting one instance of this type, or save the ")
            TEXT("level - UHierarchicalInstancedStaticMeshComponent::Serialize rebuilds on its ")
            TEXT("way to disk, so the stored instances and the saved level are correct either ")
            TEXT("way and nothing is lost."),
            Scope.ComponentInstances, Scope.BuiltInstances,
            Scope.bAllTreesUpToDate ? TEXT("") : TEXT(" and the tree is marked out of date"),
            RequestedFieldName));
    }

    // THE FIX HALF. Rebuilds the cluster tree an add left stale, through the engine's own entry
    // point: FFoliageInfo::Refresh (Public/InstancedFoliage.h:377) ->
    // FFoliageStaticMesh::Refresh -> Component->BuildTreeIfOutdated. This is the exact call
    // RemoveInstancesImpl makes for itself at InstancedFoliage.cpp:2507, and it is a no-op on the
    // impls that have no tree.
    //
    // SYNCHRONOUS ON PURPOSE, where the engine's in-file callers pass Async=true. These verbs
    // publish builtInstanceCount in the SAME response, and an async build returns with the tree
    // still out of date - the caller would read a warning about a scatter that is merely a few
    // frames early, which is the shape of the defect this file exists to close rather than a fix
    // for it. The engine already forces sync for the first build of a component either way
    // (`bForceSync = NumBuiltInstances == 0 && !World->HasBegunPlay()`,
    // HierarchicalInstancedStaticMesh.cpp:2812), so this only changes the append case.
    //
    // Force=true rather than relying on bIsOutOfDate: BuildTreeIfOutdated's own gate also fires
    // on NumBuiltInstances != PerInstanceSMData.Num() (:2794), but stating the intent costs
    // nothing and matches RemoveInstancesImpl.
    //
    // Returns whether a rebuild was attempted, so a caller can tell "refreshed" from "there was
    // no implementation to refresh" without re-deriving the test.
    inline bool RebuildFoliageClusterTreeAfterAdd(FFoliageInfo& Info)
    {
        if (!Info.IsInitialized())
        {
            return false;
        }
        Info.Refresh(/*Async*/ false, /*Force*/ true);
        return true;
    }
}
