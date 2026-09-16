// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

// Named explicitly rather than inherited from a sibling header: unity builds paper over
// a missing include, the -StrictIncludes -DisableUnity Rocket packaging build does not.
#include "Compat/EngineVersionCompat.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "LandscapeComponent.h"
#include "LandscapeGrassType.h"
#include "LandscapeProxy.h"
#include "LandscapeSubsystem.h"
#include "UObject/Object.h"
#include "Utils/DerivedStateReport.h"

// The grass twin of Handlers/Material/MaterialLandscapeConsumers.h, for the same defect
// class on a different cache (board B-grass-varieties-edit-does-not-reach-renderer).
//
// A ULandscapeGrassType is not rendered directly. Every landscape proxy that references
// one builds HierarchicalInstancedStaticMeshComponents from it and keeps them in a
// PRIVATE, transient cache — ALandscapeProxy::FoliageCache.CachedGrassComps, keyed on
// FCachedLandscapeFoliage::FGrassCompKey (LandscapeProxy.h). That key carries the grass
// type POINTER, the component, the subsection indices and the variety COUNT; it carries
// none of the values inside an FGrassVariety. So editing GrassDensity, ScaleX or
// GrassMesh leaves every cached key equal to itself, the cache is never invalidated, and
// the components keep drawing the instances they built from the previous values while
// every verb in the chain reports success.
//
// THE ENGINE USED TO DO THIS AND STOPPED. On UE 5.3,
// ULandscapeGrassType::PostEditChangeProperty walked the proxies and called
// Proxy->FlushGrassComponents() for any whose material referenced the edited type
// (5.3 LandscapeGrass.cpp). From UE 5.4 onward that body only calls
// ULandscapeComponent::InvalidateGrassTypeSummary() and recomputes StateHash; the
// StateHash feeds GrassTypesCRC (5.8 LandscapeGrass.cpp:1072) which governs which grass
// types the material DECLARES, not the instances already built. Nothing on 5.4+
// invalidates CachedGrassComps for a varieties edit — which is why a property change
// measured as a no-op at a fixed pose and only `grass.FlushCache` moved the frame.
//
// `grass.FlushCache` is a console command with no C++ entry point of its own: its body is
// a static FlushGrass() that calls ALandscapeProxy::FlushGrassComponents() on every proxy
// (5.8 LandscapeGrass.cpp:3441-3447, registered at :3474). FlushGrassComponents IS
// LANDSCAPE_API on 5.3-5.8 with an identical signature, so this reaches the mechanism
// directly instead of round-tripping a console string. ULandscapeSubsystem::RegenerateGrass
// (LandscapeSubsystem.h, LANDSCAPE_API on 5.3-5.8) is the rebuild half. Neither is a
// UFUNCTION on any of those engines, which is why `unreal.Landscape.flush_grass_components`
// does not exist in Python and cannot be made to exist from here.
//
// DO NOT COPY `grass.FlushCache`'s ARGUMENTS. It takes the defaulted bFlushGrassMaps=true
// and therefore DELETES the per-component grass density maps as well as the instances; the
// first version of this header did copy it, shipped in plugin commit d8f1bc32, and took a
// live level from 136 grass components to 0 with no recovery short of an editor restart.
// The instance invalidation this file needs is the part of FlushGrassComponents that runs
// unconditionally, so bFlushGrassMaps=false is both sufficient and non-destructive. The
// argument-by-argument citation is on RefreshGrassConsumers below.
namespace PinWright::GrassConsumers
{
    // The cached grass a proxy holds for ONE grass type.
    //
    // EntryCount is the number of FGrassComp entries keyed on the type — the unit the
    // flush destroys. FoliageComponents is the HISM pointer behind each entry that has
    // one, sorted so the comparison does not depend on TSet iteration order. A pending
    // entry has no HISM yet, so the two numbers legitimately differ and both are kept:
    // comparing only pointers would score a cache of pending entries as unchanged.
    struct FGrassCacheSnapshot
    {
        int32 EntryCount = 0;
        TArray<const UObject*> FoliageComponents;
    };

    inline void SnapshotCachedGrass(const ALandscapeProxy* Proxy,
        const ULandscapeGrassType* GrassType, FGrassCacheSnapshot& OutSnapshot)
    {
        OutSnapshot.EntryCount = 0;
        OutSnapshot.FoliageComponents.Reset();
        if (!Proxy || !GrassType)
        {
            return;
        }

        for (const FCachedLandscapeFoliage::FGrassComp& Comp : Proxy->FoliageCache.CachedGrassComps)
        {
            if (Comp.Key.GrassType.Get() != GrassType)
            {
                continue;
            }
            ++OutSnapshot.EntryCount;
            if (const UObject* Foliage = Comp.Foliage.Get())
            {
                OutSnapshot.FoliageComponents.Add(Foliage);
            }
        }
        OutSnapshot.FoliageComponents.Sort();
    }

    // "The cache entries for this grass type are not the ones we found." A rebuild
    // allocates a fresh HISM per cluster, and a flush removes the entries outright, so
    // either outcome shows up here; a proxy whose snapshot is identical demonstrably kept
    // its stale instances. This is the measurement the refresh cannot fake.
    inline bool GrassSnapshotsDiffer(const FGrassCacheSnapshot& Before,
        const FGrassCacheSnapshot& After)
    {
        if (Before.EntryCount != After.EntryCount
            || Before.FoliageComponents.Num() != After.FoliageComponents.Num())
        {
            return true;
        }
        for (int32 Index = 0; Index < Before.FoliageComponents.Num(); ++Index)
        {
            if (Before.FoliageComponents[Index] != After.FoliageComponents[Index])
            {
                return true;
            }
        }
        return false;
    }

    // Per-component grass DENSITY MAP survival across a refresh, measured on both sides.
    //
    // A grass map is not the grass. It is the GPU-rasterised per-component density/weight
    // data produced from the landscape material's grass output, held in
    // ULandscapeComponent::GrassData and SERIALISED into the landscape package
    // (5.8 Landscape.cpp:1114, `Ar << GrassData.Get()`). The HISM instances are rebuilt
    // from it whenever the camera comes into range; IT is rebuilt only by a GPU
    // rasterisation pass that the editor schedules amortised, camera-driven and gated
    // (LandscapeGrassMapsBuilder.cpp:880 needs GGrassEnable, a non-empty camera list and a
    // free pipeline slot). So discarding a map is a categorically larger event than
    // invalidating the instance cache, it is not recovered by anything this plugin can
    // call synchronously, and a SAVE after one writes the emptied data back to the
    // .uasset. This refresh must never cause one; this struct measures that instead of
    // asserting it (board B-grass-varieties-edit-does-not-reach-renderer #3/#4).
    struct FGrassMapIntegrity
    {
        // False until components were actually counted. A false here means the numbers
        // below are not measurements, exactly as FConsumerRefreshReport::bMeasured.
        bool bMeasured = false;

        // Landscape components across all consumers holding computed grass data before
        // the refresh, and after it. Equal is the contract.
        int32 ComponentsHoldingMapsBefore = 0;
        int32 ComponentsHoldingMapsAfter = 0;

        // Components whose grass map went away across the refresh. Contractually always
        // zero; published only when it is not, because a non-zero here is the shipped
        // regression this measurement exists to catch.
        int32 Discarded() const
        {
            return FMath::Max(0, ComponentsHoldingMapsBefore - ComponentsHoldingMapsAfter);
        }
    };

    // Components on this proxy whose grass map has been computed (or deserialised).
    //
    // FLandscapeComponentGrassData::HasValidData() is the engine's own name for this test
    // and its body is exactly this comparison (5.8 LandscapeGrass.cpp:1653-1659), but the
    // struct carries no LANDSCAPE_API (LandscapeComponent.h:207 on 5.3-5.8), so its
    // out-of-line members do not link from a plugin module. NumElements is a public data
    // member with the identical documented meaning on every supported engine
    // (LandscapeComponent.h:230-233 on 5.8, :209-212 on 5.3): >= 0 means computed, where 0
    // is a valid all-zero map, and UnknownNumElements (-1) means never computed. A
    // freshly-allocated FLandscapeComponentGrassData - which is precisely what
    // ULandscapeComponent::RemoveGrassMap() installs (LandscapeGrass.cpp:1233-1239) -
    // reads -1, so this counter drops the moment a map is discarded.
    inline int32 CountComponentsHoldingGrassMaps(const ALandscapeProxy* Proxy)
    {
        if (!Proxy)
        {
            return 0;
        }
        int32 Count = 0;
        for (const ULandscapeComponent* Component : Proxy->LandscapeComponents)
        {
            if (Component && Component->GrassData->NumElements >= 0)
            {
                ++Count;
            }
        }
        return Count;
    }

    // The two measurements a grass refresh owes a caller, kept in one object because they
    // are taken in the same pass and one without the other is what shipped the regression:
    // "the stale cache entries are gone" reads identically whether the grass was
    // invalidated or destroyed, and only GrassMaps can tell those apart.
    struct FGrassRefreshReport
    {
        PinWright::DerivedState::FConsumerRefreshReport Consumers;
        FGrassMapIntegrity GrassMaps;
    };

    // True when the proxy's landscape material DECLARES this grass type, whether or not it
    // has built anything from it yet. Used alongside cache membership so a landscape that
    // uses the type but has no grass built (camera out of range) is still counted as a
    // consumer and reported as "nothing to refresh" rather than being invisible.
    //
    // The accessor is engine-version-shaped: 5.8 replaced the per-component GrassTypes
    // array with the NamedGrassTypes map (LandscapeComponent.h:598/:922), 5.4-5.7 carry
    // GetGrassTypes(), and 5.3's ULandscapeComponent has neither — on 5.3 the material's
    // grass output is the only source, and 5.3's own PostEditChangeProperty already
    // flushes the consumers, so cache membership is left as the sole test there.
    // GrassType is taken non-const because the engine's own containers are keyed on
    // TObjectPtr<ULandscapeGrassType>, which does not accept a pointer-to-const.
    inline bool ProxyDeclaresGrassType(const ALandscapeProxy* Proxy,
        ULandscapeGrassType* GrassType)
    {
        if (!Proxy || !GrassType)
        {
            return false;
        }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        for (const ULandscapeComponent* Component : Proxy->LandscapeComponents)
        {
            if (Component && Component->GetNamedGrassTypes().FindKey(GrassType) != nullptr)
            {
                return true;
            }
        }
#elif UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        for (const ULandscapeComponent* Component : Proxy->LandscapeComponents)
        {
            if (Component && Component->GetGrassTypes().Contains(GrassType))
            {
                return true;
            }
        }
#endif
        return false;
    }

    // Invalidate the built grass of every landscape in the editor world that consumes
    // GrassType, then rebuild, and MEASURE which ones actually changed. Fills Report so
    // the caller publishes coverage instead of a claim.
    //
    // Two engine calls:
    //   * ALandscapeProxy::FlushGrassComponents(nullptr, bFlushGrassMaps=FALSE) per
    //     consumer. The instance invalidation this verb exists for is the no-filter
    //     branch's UNCONDITIONAL half: FoliageComponents.Empty(),
    //     FoliageCache.ClearCache() and the destruction of every owned and attached HISM
    //     all run before bFlushGrassMaps is even read (5.8 LandscapeGrass.cpp:2691-2721),
    //     and ClearCache() is `CachedGrassComps.Empty()` (LandscapeProxy.h:386-389) — i.e.
    //     the pointer-and-count-keyed entries this file exists to invalidate are gone
    //     either way.
    //     bFlushGrassMaps gates ONLY the extra WITH_EDITOR block at :2726-2736, which
    //     calls ULandscapeComponent::RemoveGrassMap() on every landscape component; that
    //     is one statement replacing GrassData with a freshly allocated empty one
    //     (:1233-1239), it schedules no rebuild, and the data it drops is serialised into
    //     the landscape package. So the destructive argument buys this path NOTHING and
    //     costs a per-component GPU re-rasterisation that only the amortised, camera-gated
    //     builder can supply. Measured in the field at plugin d8f1bc32: 136 grass
    //     components -> 0, no recovery over three minutes, none from `grass.Enable 0/1`.
    //
    //     THE ENGINE'S OWN ANSWER TO THIS EXACT EVENT IS bFlushGrassMaps=false, and it
    //     says why in a comment. When FLandscapeGrassMapsBuilder detects that a
    //     component's grass TYPES changed it notes "this invalidates foliage instances but
    //     not the grass maps" (5.8 LandscapeGrassMapsBuilder.cpp:500), collects the
    //     component into ComponentsToRemoveFoliageInstances (:504) and hands it to
    //     ULandscapeSubsystem::RemoveGrassInstances (:579), which ends at
    //     LandscapeSubsystem.cpp:609 with FlushGrassComponents(Components,
    //     /*bFlushGrassMaps = */false). `grass.FlushCachePIE` passes false as well
    //     (LandscapeGrass.cpp:3449-3455); only `grass.FlushCache` takes the default true
    //     (:3441-3447), which is why the console command that was measured as "the thing
    //     that works" is also the one that destroys the carpet.
    //   * ULandscapeSubsystem::RegenerateGrass(bInFlushGrass=false, bInForceSync=true)
    //     once — the rebuild half. bInFlushGrass is false because the per-consumer flush
    //     above already ran and is scoped to the consumers; RegenerateGrass's own flush
    //     branch would re-flush every proxy in the world through RemoveGrassInstances().
    //
    // There is deliberately NO opt-in to the destructive variant. A parameter is only
    // honest if some edit class needs it, and none does: a ULandscapeGrassType edit
    // changes which meshes/densities the instances are built with, never the material's
    // rasterised weight output the map holds — which is the distinction the engine draws
    // at LandscapeGrassMapsBuilder.cpp:500. A caller who genuinely wants every map in the
    // process dropped still has `system.console_command {command: "grass.FlushCache"}`,
    // where the cost is opted into by name.
    //
    // Scope, stated rather than implied:
    //   * Only the editor world. A PIE world keeps its own proxies and its own cache;
    //     nothing here reaches them, and AddKnownUnrefreshedGrassConsumers says so.
    //   * Proxies in unloaded World Partition cells are not visited and do not need to be:
    //     an unloaded proxy builds its grass from the current asset when it loads.
    //   * A proxy with no ULandscapeComponents, or no root component, is not a consumer.
    //     The root-component test is not cosmetic: FlushGrassComponents' no-filter branch
    //     asserts on RootComponent (5.8 LandscapeGrass.cpp:2706).
    //   * bMeasured stays false when there is no editor world, so a caller never reads
    //     "0 of 0 consumers" from a run that never looked. Report.GrassMaps.bMeasured is
    //     kept on the same footing for the same reason.
    inline void RefreshGrassConsumers(
        ULandscapeGrassType* GrassType,
        FGrassRefreshReport& Report)
    {
        if (!GrassType)
        {
            return;
        }

        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!IsValid(World))
        {
            return;
        }

        Report.Consumers.bMeasured = true;

        // Collect first, mutate second: the flush destroys components and the rebuild
        // allocates them, and neither should run while a live TActorIterator is walking
        // the level's actor arrays.
        TArray<ALandscapeProxy*> Consumers;
        TArray<FGrassCacheSnapshot> Before;
        for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
        {
            ALandscapeProxy* Proxy = *It;
            if (!IsValid(Proxy) || Proxy->LandscapeComponents.Num() == 0
                || Proxy->GetRootComponent() == nullptr)
            {
                continue;
            }

            FGrassCacheSnapshot Snapshot;
            SnapshotCachedGrass(Proxy, GrassType, Snapshot);
            if (Snapshot.EntryCount == 0 && !ProxyDeclaresGrassType(Proxy, GrassType))
            {
                continue;
            }

            Consumers.Add(Proxy);
            Before.Add(MoveTemp(Snapshot));
        }

        Report.Consumers.ConsumersFound = Consumers.Num();
        if (Consumers.Num() == 0)
        {
            return;
        }

        // Taken across the same consumer set the flush is about to touch, on both sides,
        // so "the maps survived" is a measurement of these proxies and not a global claim.
        Report.GrassMaps.bMeasured = true;
        for (const ALandscapeProxy* Proxy : Consumers)
        {
            Report.GrassMaps.ComponentsHoldingMapsBefore += CountComponentsHoldingGrassMaps(Proxy);
        }

        for (ALandscapeProxy* Proxy : Consumers)
        {
            if (IsValid(Proxy))
            {
                Proxy->FlushGrassComponents(/*OnlyForComponents=*/nullptr, /*bFlushGrassMaps=*/false);
            }
        }

        if (ULandscapeSubsystem* Subsystem = World->GetSubsystem<ULandscapeSubsystem>())
        {
            Subsystem->RegenerateGrass(/*bInFlushGrass=*/false, /*bInForceSync=*/true);
        }

        for (const ALandscapeProxy* Proxy : Consumers)
        {
            if (IsValid(Proxy))
            {
                Report.GrassMaps.ComponentsHoldingMapsAfter += CountComponentsHoldingGrassMaps(Proxy);
            }
        }

        for (int32 Index = 0; Index < Consumers.Num(); ++Index)
        {
            ALandscapeProxy* Proxy = Consumers[Index];
            if (!IsValid(Proxy))
            {
                continue;
            }

            FGrassCacheSnapshot After;
            SnapshotCachedGrass(Proxy, GrassType, After);

            if (GrassSnapshotsDiffer(Before[Index], After))
            {
                ++Report.Consumers.ConsumersRefreshed;
                // The stale clusters that were dropped, or the fresh ones that replaced
                // them, whichever is the larger fact about this proxy. Purely evidential:
                // it separates "invalidated 96 grass clusters" from "visited a landscape
                // that had none".
                Report.Consumers.SubObjectsRefreshed +=
                    FMath::Max(Before[Index].EntryCount, After.EntryCount);
                Report.Consumers.Refreshed.Add(Proxy->GetActorNameOrLabel());
            }
            else if (Before[Index].EntryCount == 0)
            {
                // Declares the grass type, had nothing built from it. The refresh ran and
                // correctly changed nothing — a different fact from "was rebuilt", and
                // collapsing the two would let an unbuilt landscape score as refreshed.
                ++Report.Consumers.ConsumersWithNothingToRefresh;
            }
        }
    }

    // Announce a finished edit to a ULandscapeGrassType.
    //
    // The empty FPropertyChangedEvent a bare PostEditChange() builds is enough HERE, and
    // only here: ULandscapeGrassType::PostEditChangeProperty branches on no property name
    // at all (5.8 LandscapeGrass.cpp:1592-1617) — it invalidates the grass-type summaries
    // and recomputes StateHash whatever changed. Do not read this as a licence to use the
    // bare call on other classes, where a null Property matches no GET_MEMBER_NAME_CHECKED
    // branch and the derived state never recomputes.
    inline void NotifyGrassTypeChanged(ULandscapeGrassType* GrassType)
    {
        if (!GrassType)
        {
            return;
        }
        GrassType->PostEditChange();
    }

    // The whole "this grass-type edit is finished" step, in one call: notify, then
    // invalidate and rebuild the landscapes that consume it, measured into Report.
    //
    // Every verb that COMPLETES a unit of work on a ULandscapeGrassType should call this
    // instead of a bare PostEditChange(), for the reason its material twin
    // (MaterialConsumers::ApplyMasterMaterialEdit) states: having one name for the pairing
    // is what stops the next verb from shipping the notify alone.
    //
    // landscape.create_grass_type is the one grass verb that deliberately does NOT call
    // this, and the exemption is structural rather than an oversight: it creates an asset
    // no material references yet, so it has no consumers to find. If it ever grows a
    // create-or-update path, that path must call this.
    inline void ApplyGrassTypeEdit(
        ULandscapeGrassType* GrassType,
        FGrassRefreshReport& Report)
    {
        NotifyGrassTypeChanged(GrassType);
        RefreshGrassConsumers(GrassType, Report);
    }

    // The consumer kinds no grass verb pushes into, named so a coverage number is not read
    // as broader than it is (rpc-design.md §1).
    inline void AddKnownUnrefreshedGrassConsumers(
        PinWright::DerivedState::FConsumerRefreshReport& Report)
    {
        Report.NotRefreshed.AddUnique(
            TEXT("play-in-editor worlds, which keep their own landscape proxies and grass cache"));
        Report.NotRefreshed.AddUnique(
            TEXT("landscape proxies in unloaded World Partition cells, which rebuild their grass on load"));
        if (!Report.IsComplete())
        {
            Report.Remedy = TEXT(
                "Re-run landscape.flush_grass for this grass type; if a landscape is still "
                "missing from refreshed[], system.console_command {command: \"grass.FlushCache\"} "
                "flushes every proxy in the process. Note that grass.FlushCache ALSO deletes "
                "the per-component grass density maps, which this verb deliberately does not.");
        }
    }

    // Emits `grassMaps: {measured, componentsHoldingMapsBefore, componentsHoldingMapsAfter,
    // discarded?, warning?}` — the measurement that separates "the stale grass was
    // invalidated" from "the grass data was destroyed". Those two are indistinguishable in
    // consumerRefresh, and that is exactly how the destructive version shipped.
    //
    // Reporting rules, per rpc-design.md §1: every field here is read off the components,
    // never off what was requested — there is no request to name, because this path has no
    // destructive mode to ask for. `discarded` is OMITTED when it is zero, so its presence
    // is the signal rather than its value, and it arrives with the warning that says what
    // it means. A zero published on every call would train a reader to skip the field.
    inline void AddGrassMapIntegrityReport(
        const TSharedPtr<FJsonObject>& Result,
        const FGrassMapIntegrity& Integrity)
    {
        if (!Result.IsValid())
        {
            return;
        }

        const TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
        Block->SetBoolField(TEXT("measured"), Integrity.bMeasured);
        Block->SetNumberField(TEXT("componentsHoldingMapsBefore"),
            Integrity.ComponentsHoldingMapsBefore);
        Block->SetNumberField(TEXT("componentsHoldingMapsAfter"),
            Integrity.ComponentsHoldingMapsAfter);

        const int32 Discarded = Integrity.Discarded();
        if (Discarded > 0)
        {
            Block->SetNumberField(TEXT("discarded"), Discarded);
            Block->SetStringField(TEXT("warning"), FString::Printf(TEXT(
                "%d landscape component(s) lost their grass density map across this refresh. "
                "This verb flushes grass INSTANCES only and must never cause that, so treat "
                "it as a defect in the refresh path, not as a cost of the edit. The maps are "
                "rebuilt only by the editor's amortised, camera-driven grass-map builder, and "
                "saving the level while they are empty writes the empty data to the package."),
                Discarded));
        }

        Result->SetObjectField(TEXT("grassMaps"), Block);
    }
}
