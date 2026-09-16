// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

// Named explicitly rather than inherited from a sibling header: unity builds paper over
// a missing include, the -StrictIncludes -DisableUnity Rocket packaging build does not.
#include "Compat/EngineVersionCompat.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Landscape.h"
#include "LandscapeComponent.h"
#include "LandscapeProxy.h"
#include "MaterialDomain.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "UObject/Object.h"
#include "Utils/DerivedStateReport.h"

// Landscape is the one consumer of a master UMaterial that keeps a PRIVATE cache of
// derived material instances the engine's own material-change plumbing cannot see.
//
// Editing the master graph and calling PreEditChange(nullptr) + PostEditChange()
// regenerates the master's StateId and recompiles it (Material.cpp:5412-5433), and a
// surrounding FMaterialUpdateContext recaches every loaded UMaterialInstance whose base
// material changed (MaterialShared.cpp:5099-5156). Neither touches
// ALandscapeProxy::MaterialInstanceConstantMap — the map of "combination materials"
// the landscape builds per weightmap-layer allocation and reuses across rebuilds
// (LandscapeEdit.cpp:710, ULandscapeComponent::GetCombinationMaterial). A graph edit
// that changes which layers exist, or how they blend, leaves those combination MICs
// keyed on the previous allocation, so the components keep rendering the old shader
// map and the edit measures as a no-op while every verb in the chain reports success.
//
// ALandscapeProxy::UpdateAllComponentMaterialInstances(bInInvalidateCombinationMaterials
// = true) (LandscapeProxy.h:1414, LANDSCAPE_API) resets that map and rebuilds every
// component's MICs (LandscapeEdit.cpp:845-876). It is the same work
// ALandscapeProxy::PostEditChangeProperty does on its LandscapeMaterial branch — the
// branch commit 11fe111a taught landscape.set_material to reach by naming the property
// in the change event. That fix covers material-instance/parameter changes routed
// through an ASSIGNMENT; it does nothing for a graph edit to the master, because no
// assignment happens. This is the same mechanism applied to the master-edit path.
namespace PinWright::MaterialConsumers
{
    // True when Candidate ultimately resolves to Material — directly, or through any
    // depth of material-instance parenting. UMaterialInterface::GetMaterial() walks the
    // Parent chain to the base UMaterial and returns itself for a UMaterial, so this one
    // test covers "the landscape holds the material" and "the landscape holds a MIC of
    // the material" without hand-walking Parent.
    inline bool ResolvesToMaterial(UMaterialInterface* Candidate, UMaterial* Material)
    {
        return Candidate && Material && Candidate->GetMaterial() == Material;
    }

    // Every material a landscape proxy renders with: the proxy's own surface and hole
    // materials plus each component's, since a component may override either.
    //
    // On 5.8 this is one engine call. On 5.3-5.7 ALandscapeProxy::RetrieveAllLandscapeMaterials
    // does not exist, so the pre-5.8 branch is a line-for-line transcription of the 5.8 body
    // (Landscape.cpp:5385 for the proxy, :1202 for the component), built out of
    // GetLandscapeMaterial / GetLandscapeHoleMaterial, which both engines carry as LANDSCAPE_API
    // and which are what the 5.8 implementation itself calls. The default-surface-material
    // exclusion is part of that body and is carried here for the same reason: an unset slot
    // reads back as the engine default, and reporting it would make every landscape a consumer
    // of it.
    inline void RetrieveLandscapeMaterials(const ALandscapeProxy* Proxy,
        TSet<UMaterialInterface*>& OutMaterials)
    {
        if (!Proxy)
        {
            return;
        }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        Proxy->RetrieveAllLandscapeMaterials(OutMaterials);
#else
        UMaterialInterface* const DefaultSurface = UMaterial::GetDefaultMaterial(MD_Surface);
        const auto AddIfSet = [&OutMaterials, DefaultSurface](UMaterialInterface* Material)
        {
            if (Material && Material != DefaultSurface)
            {
                OutMaterials.Add(Material);
            }
        };

        AddIfSet(Proxy->GetLandscapeMaterial());
        AddIfSet(Proxy->GetLandscapeHoleMaterial());

        for (ULandscapeComponent* Component : Proxy->LandscapeComponents)
        {
            if (Component != nullptr)
            {
                AddIfSet(Component->GetLandscapeMaterial());
                AddIfSet(Component->GetLandscapeHoleMaterial());
            }
        }
#endif
    }

    // Identity snapshot of every per-component MIC on the proxy, flattened.
    //
    // This is the measurement, and it is one the refresh path cannot fake:
    // ULandscapeComponent::UpdateMaterialInstances_Internal allocates a BRAND NEW
    // ULandscapeMaterialInstanceConstant for each component on every rebuild
    // (LandscapeEdit.cpp:715 — `NewObject<ULandscapeMaterialInstanceConstant>`), so a
    // component whose pointer set is unchanged was demonstrably not rebuilt. Comparing
    // pointers therefore distinguishes "the rebuild ran" from "the call returned".
    inline void SnapshotComponentMaterialInstances(
        const ALandscapeProxy* Proxy,
        TArray<TArray<const UObject*>>& OutSnapshot)
    {
        OutSnapshot.Reset();
        if (!Proxy)
        {
            return;
        }

        OutSnapshot.Reserve(Proxy->LandscapeComponents.Num());
        for (const ULandscapeComponent* Component : Proxy->LandscapeComponents)
        {
            TArray<const UObject*>& Row = OutSnapshot.AddDefaulted_GetRef();
            if (!Component)
            {
                continue;
            }
            Row.Reserve(Component->MaterialInstances.Num());
            for (const TObjectPtr<UMaterialInstanceConstant>& Mic : Component->MaterialInstances)
            {
                Row.Add(Mic.Get());
            }
        }
    }

    // Total MIC pointers across a snapshot. Zero means the landscape caches no derived
    // material instance at all, which is a different fact from "the cache was replaced".
    inline int32 CountComponentMaterialInstances(const TArray<TArray<const UObject*>>& Snapshot)
    {
        int32 Total = 0;
        for (const TArray<const UObject*>& Row : Snapshot)
        {
            Total += Row.Num();
        }
        return Total;
    }

    // Number of components whose MIC pointer set differs between the two snapshots.
    // A row count mismatch means components appeared or disappeared, which is itself a
    // rebuild, so those rows count as changed. Element comparison is spelled out rather
    // than leaning on TArray's relational operators, so the meaning of "changed" here
    // is exactly "at least one MIC is a different object".
    inline int32 CountChangedComponents(
        const TArray<TArray<const UObject*>>& Before,
        const TArray<TArray<const UObject*>>& After)
    {
        int32 Changed = 0;
        const int32 RowCount = FMath::Max(Before.Num(), After.Num());
        for (int32 Row = 0; Row < RowCount; ++Row)
        {
            if (!Before.IsValidIndex(Row) || !After.IsValidIndex(Row))
            {
                ++Changed;
                continue;
            }

            const TArray<const UObject*>& BeforeRow = Before[Row];
            const TArray<const UObject*>& AfterRow = After[Row];
            bool bRowChanged = (BeforeRow.Num() != AfterRow.Num());
            for (int32 Index = 0; !bRowChanged && Index < BeforeRow.Num(); ++Index)
            {
                bRowChanged = (BeforeRow[Index] != AfterRow[Index]);
            }
            if (bRowChanged)
            {
                ++Changed;
            }
        }
        return Changed;
    }

    // Rebuild the per-component material instances of every landscape proxy in the
    // editor world that renders with Material, and MEASURE which ones actually
    // rebuilt. Fills Report so the caller can publish coverage instead of a claim.
    //
    // Scope, stated rather than implied:
    //   * Streaming proxies whose World Partition cell is not loaded are not visited
    //     and do not need to be — an unloaded proxy builds its MICs from the current
    //     material when it loads, so it cannot inherit a stale one.
    //   * A proxy with no ULandscapeComponents renders nothing and is not counted as a
    //     consumer; counting it would make coverage unreachable for a hollow actor.
    //   * bMeasured stays false when there is no editor world, so a caller never reads
    //     "0 of 0 consumers" from a run that never looked.
    inline void RefreshLandscapeConsumers(
        UMaterial* Material,
        PinWright::DerivedState::FConsumerRefreshReport& Report)
    {
        if (!Material)
        {
            return;
        }

        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!IsValid(World))
        {
            return;
        }

        Report.bMeasured = true;

        // Collect first, mutate second. The rebuild allocates UObjects and recreates
        // render states while a live TActorIterator would still be walking the level's
        // actor arrays; nothing observed says it spawns or destroys actors, but a
        // two-phase walk removes the question rather than relying on the answer.
        TArray<ALandscapeProxy*> Consumers;
        for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
        {
            ALandscapeProxy* Proxy = *It;
            if (!IsValid(Proxy) || Proxy->LandscapeComponents.Num() == 0)
            {
                continue;
            }

            // RetrieveAllLandscapeMaterials (LANDSCAPE_API, Landscape.cpp:5385) gathers
            // LandscapeMaterial, LandscapeHoleMaterial and every per-component override
            // in one call, so a hand-rolled property sweep cannot drift from it.
            // It is 5.8-only (LandscapeProxy.h:1319); RetrieveLandscapeMaterials below
            // transcribes it for 5.3-5.7 out of the accessors those engines do carry.
            TSet<UMaterialInterface*> UsedMaterials;
            RetrieveLandscapeMaterials(Proxy, UsedMaterials);

            for (UMaterialInterface* Candidate : UsedMaterials)
            {
                if (ResolvesToMaterial(Candidate, Material))
                {
                    Consumers.Add(Proxy);
                    break;
                }
            }
        }

        Report.ConsumersFound = Consumers.Num();

        for (ALandscapeProxy* Proxy : Consumers)
        {
            if (!IsValid(Proxy))
            {
                continue;
            }

            TArray<TArray<const UObject*>> Before;
            SnapshotComponentMaterialInstances(Proxy, Before);

            Proxy->UpdateAllComponentMaterialInstances(/*bInInvalidateCombinationMaterials=*/true);

            TArray<TArray<const UObject*>> After;
            SnapshotComponentMaterialInstances(Proxy, After);

            const int32 ChangedComponents = CountChangedComponents(Before, After);
            if (ChangedComponents > 0)
            {
                ++Report.ConsumersRefreshed;
                Report.SubObjectsRefreshed += ChangedComponents;
                Report.Refreshed.Add(Proxy->GetActorNameOrLabel());
            }
            else if (CountComponentMaterialInstances(After) == 0)
            {
                // The rebuild ran and produced no MICs, which is the correct outcome when
                // GetCombinationMaterial declines to build one (LandscapeEdit.cpp:591-608,
                // holes painted against a special engine material). Nothing was cached, so
                // nothing could be stale — but it is not the same fact as "rebuilt", and
                // reporting it as one would let an empty landscape look refreshed.
                ++Report.ConsumersWithNothingToRefresh;
            }
        }
    }

    // Announce a finished edit to a master UMaterial so the instances derived from it
    // recache their static permutations.
    //
    // A bare Material->PostEditChange() is NOT this. It regenerates the master's StateId
    // and recompiles the master (Material.cpp:5412-5433) but opens no
    // FMaterialUpdateContext, and that destructor is the only code that recaches dependent
    // UMaterialInstances (MaterialShared.cpp:5099-5156) — so every instance keeps the
    // shader map it built against the PREVIOUS master. The engine's own recompile scopes
    // the change exactly this way (UMaterialEditingLibrary::RecompileMaterialInternal,
    // MaterialEditingLibrary.cpp:988-998); a handler that writes the bare pair silently
    // does less than the editor does for the same edit.
    //
    // This is deliberately NOT folded into RefreshLandscapeConsumers: some callers must
    // run extra work (ForceRecompileForRendering) between the notify and the landscape
    // rebuild, so that the components build their MICs against a current master.
    inline void NotifyMasterMaterialChanged(UMaterial* Material)
    {
        if (!Material)
        {
            return;
        }

        FMaterialUpdateContext UpdateContext;
        UpdateContext.AddMaterial(Material);

        Material->PreEditChange(nullptr);
        Material->PostEditChange();
    }

    // The whole "this master edit is finished" step, in one call: recache the dependent
    // material instances, then rebuild the landscape consumers, measured into Report.
    //
    // Every verb that COMPLETES a unit of work on a master material should call this
    // instead of a bare PostEditChange(). The two halves are separable (above) but the
    // pairing is what callers actually need, and having one name for it is what stops the
    // next verb from shipping the bare pair again — which is exactly how this defect
    // survived its first fix (docs/rpc-design.md §5b).
    inline void ApplyMasterMaterialEdit(
        UMaterial* Material,
        PinWright::DerivedState::FConsumerRefreshReport& Report)
    {
        NotifyMasterMaterialChanged(Material);
        RefreshLandscapeConsumers(Material, Report);
    }

    // The consumer kind no material verb pushes into, named so a coverage number is not
    // read as broader than it is (rpc-design.md §1).
    inline void AddKnownUnrefreshedConsumers(
        PinWright::DerivedState::FConsumerRefreshReport& Report)
    {
        Report.NotRefreshed.AddUnique(
            TEXT("open asset editors, which keep their own preview material state"));
        if (!Report.IsComplete())
        {
            Report.Remedy = TEXT(
                "Re-apply the material with landscape.set_material on each landscape "
                "missing from refreshed[].");
        }
    }
}
