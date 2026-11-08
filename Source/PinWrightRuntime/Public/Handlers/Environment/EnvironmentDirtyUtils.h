// Copyright (c) 2026 Alexander Penkin. MIT License.

// EnvironmentDirtyUtils.h - Dirty ceremony for main-module handlers that mutate
// live level actors.
//
// The bug these exist to close: UWorld::SpawnActor dirties the level only under a
// transaction (`if (GUndo) ModifyLevel(LevelToSpawnIn);`, LevelActor.cpp:735-739),
// and the engine's light/fog setters push render state through a fast path that
// never marks the package dirty (USkyLightComponent::SetIntensity ->
// UpdateLimitedRenderingStateFast, SkyLightComponent.cpp:971-980). Handlers here
// open no transaction, so both a spawn and a property write left the level clean:
// the viewport updated, `level.save` no-opped, and the edit vanished on close.
//
// Deliberately NOT used: FScopedTransaction (undo for these verbs is a separate
// feature) and PreEditChange/PostEditChangeProperty (UActorComponent::PreEditChange
// flushes rendering commands on every call, unacceptable in loop-driven verbs).
//
// UWorld::DestroyActor already calls MarkPackageDirty() unconditionally
// (LevelActor.cpp:1056), so destroy paths need nothing from this header.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/Level.h"
#include "GameFramework/Actor.h"

namespace PinWright
{
    // Call BEFORE the mutation.
    //
    // Ordering is the whole point: Modify() must precede the write so that the call
    // stays state-neutral if an FScopedTransaction is ever wrapped around one of
    // these verbs. (A post-hoc Modify() would snapshot the already-changed value and
    // make Ctrl+Z a no-op.) MarkPackageDirty() is order-independent — it sets a flag
    // on the package and snapshots nothing — so both halves live in one pre-mutation
    // call rather than a begin/end pair that is easy to half-apply.
    //
    // Dirties the ACTOR's package, never the world's. Under World Partition / OFPA
    // the actor lives in its own external package; dirtying the world package marks
    // the wrong one. UObject::MarkPackageDirty resolves the outermost package,
    // correct in both layouts.
    //
    // MarkPackageDirty() after Modify() is belt-and-braces, not redundancy theatre:
    // UObject::Modify (Obj.cpp:1652-1680) only reaches its own MarkPackageDirty()
    // when SaveToTransactionBuffer returns false AND the outermost package is not a
    // script package. Calling it explicitly makes the dirty unconditional, which is
    // the property this code actually needs.
    //
    // EditedComponent is passed when the mutated UPROPERTY lives on a component
    // (light intensity on LightComponent0, fog on ExponentialHeightFogComponent0,
    // skylight on SkyLightComponent0) so that a future transaction snapshots the
    // object that actually changes. Null-safe; both arguments may be null.
    inline void MarkLevelActorModified(AActor* Actor, UActorComponent* EditedComponent = nullptr)
    {
        if (!Actor)
        {
            return;
        }

        Actor->Modify();
        if (EditedComponent)
        {
            EditedComponent->Modify();
        }
        Actor->MarkPackageDirty();
    }

    // Call AFTER the mutation, and ONLY when the component property was written as a
    // raw field assignment. Every engine setter used in this cluster
    // (SetIntensity / SetLightColor / SetCastShadows / SetAttenuationRadius /
    // SetInnerConeAngle / SetOuterConeAngle / SetSourceWidth / SetSourceHeight /
    // SetAtmosphereSunLight) already pushes render state itself, so adding this after
    // one of those is pure waste. Raw writes (SkyComp->SourceType, SkyComp->Cubemap,
    // FogComp->bEnableVolumetricFog, FogComp->VolumetricFogDistance) push nothing and
    // need it.
    //
    // Does NOT apply to APostProcessVolume: FPostProcessSettings is a UPROPERTY on the
    // ACTOR (PostProcessVolume.h:27-28), not on a component, and the renderer samples
    // the volume list per frame. PPV verbs need MarkLevelActorModified only.
    inline void MarkComponentRenderStateDirty(UActorComponent* Component)
    {
        if (Component)
        {
            Component->MarkRenderStateDirty();
        }
    }

    // Call after a successful spawn, before any post-spawn property writes.
    //
    // Dirties BOTH the actor's package and its level's: a spawn changes the level's
    // actor list, which under OFPA is a different package from the actor's own. On a
    // classic map both resolve to the same UPackage and MarkPackageDirty() is
    // idempotent, so there is no cost. Null-safe.
    inline void MarkLevelActorSpawned(AActor* Actor)
    {
        if (!Actor)
        {
            return;
        }

        Actor->Modify();
        Actor->MarkPackageDirty();

        if (ULevel* Level = Actor->GetLevel())
        {
            Level->MarkPackageDirty();
        }
    }
}
