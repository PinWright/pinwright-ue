// Copyright (c) 2026 Alexander Penkin. MIT License.

// SpatialTraceUtils.cpp - shared world line-trace helpers for the spatial.* handlers.

#include "Handlers/Spatial/SpatialTraceUtils.h"

#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Compat/EngineVersionCompat.h" // MCP_OVERLAP_ITEM_INDEX - GetItemIndex() is 5.7+
#include "Engine/HitResult.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/InstancedStaticMeshComponent.h" // HISM and the foliage component derive from it
#include "Components/PrimitiveComponent.h"
#include "Materials/MaterialInterface.h" // UMaterialInterface::GetBlendMode - the intrinsic effect-card test
#include "UObject/Class.h"
#include "Utils/CollisionSummaryUtils.h" // the shared simple-primitive count

namespace
{
    // Copies an engine FHitResult into the module's result struct, including the two
    // diagnostic fields that make the "complex trace hit render geometry" case detectable.
    void SpatialTraceFillHit(SpatialTraceUtils::FSpatialHit& Out, const FHitResult& Hit,
                             bool bTraceComplex)
    {
        Out.bHit = true;
        Out.Location = Hit.ImpactPoint;
        Out.Normal = Hit.ImpactNormal;
        Out.HitActor = Hit.GetActor();
        Out.HitComponent = Hit.GetComponent();
        Out.Distance = Hit.Distance;
        Out.FaceIndex = Hit.FaceIndex;
        // Shared with editor.set_view_mode's collision report (Utils/CollisionSummaryUtils.h):
        // both answer "what collision does this thing actually have?", and two copies of the
        // AggGeom read is how they drift. The value is the engine's own GetElementCount(),
        // which sums MORE element kinds than the five broken out in the dump/report payloads -
        // do not redefine it as the sum of those five.
        Out.SimpleCollisionShapes =
            PinWrightCollisionSummary::CountSimpleShapes(Hit.GetComponent());
        // Only a COMPLEX query can resolve against render triangles, and only a component
        // with a body setup that holds zero simple primitives has nothing else to hit.
        // -1 (no body setup at all, e.g. Landscape) is explicitly NOT this case.
        Out.bRenderGeometryHit = bTraceComplex && Out.SimpleCollisionShapes == 0;
    }

    // True when Class or ANY ancestor class matches one of Patterns as a case-insensitive
    // substring of the bare class name OR the full /Script path, so a caller can write
    // "LandscapeProxy" or "/Script/Landscape.LandscapeProxy" and get the same answer.
    // Class-kind-agnostic on purpose: the actor axes (OnlyClasses / ExcludeClasses) and the
    // component axis (ExcludeComponentClasses) all route through it, so no two of them can
    // drift apart on what "matches a class" means.
    bool SpatialTraceClassChainMatches(const UClass* Class, const TArray<FString>& Patterns)
    {
        for (const UClass* Cls = Class; Cls; Cls = Cls->GetSuperClass())
        {
            const FString ClassName = Cls->GetName();
            const FString ClassPath = Cls->GetPathName();
            for (const FString& Wanted : Patterns)
            {
                if (ClassName.Contains(Wanted, ESearchCase::IgnoreCase)
                    || ClassPath.Contains(Wanted, ESearchCase::IgnoreCase))
                {
                    return true;
                }
            }
        }
        return false;
    }

    // The actor-class form of the above. Shared by the accept (OnlyClasses) and reject
    // (ExcludeClasses) directions.
    bool SpatialTraceMatchesAnyClass(AActor* Actor, const TArray<FString>& Patterns)
    {
        return Actor ? SpatialTraceClassChainMatches(Actor->GetClass(), Patterns) : false;
    }

    // Component classes that only ever draw effects or editor markers - never ground. Matched
    // by class-name ancestry (the same rule SpatialTraceMatchesAnyClass applies to actors)
    // rather than through StaticClass(), so recognising a Niagara or Cascade component costs
    // this module no link-time dependency on those plugins. Bare class names, because
    // GetName() on a UClass has no U prefix.
    const TCHAR* const SpatialTraceEffectComponentClasses[] = {
        TEXT("ParticleSystemComponent"), // Cascade
        TEXT("NiagaraComponent"),
        TEXT("DecalComponent"),
        TEXT("BillboardComponent"),
        TEXT("ArrowComponent"),
        TEXT("TextRenderComponent")
    };

    bool SpatialTraceIsEffectComponentClass(const UPrimitiveComponent* Component)
    {
        if (!Component)
        {
            return false;
        }
        for (const UClass* Cls = Component->GetClass(); Cls; Cls = Cls->GetSuperClass())
        {
            const FString ClassName = Cls->GetName();
            for (const TCHAR* Wanted : SpatialTraceEffectComponentClasses)
            {
                if (ClassName.Equals(Wanted, ESearchCase::IgnoreCase))
                {
                    return true;
                }
            }
        }
        return false;
    }

    // True when the component draws nothing opaque: every material slot it exposes resolves to
    // a non-opaque blend mode. BLEND_Opaque and BLEND_Masked are the two that write depth like
    // solid geometry (Engine/EngineTypes.h:245-259); everything else - translucent, additive,
    // modulate, alpha composite, alpha holdout - is the family haze/fog/glow cards use.
    //
    // Zero material slots means "collision-only shape", which is opaque for this purpose: a
    // blocking volume with no visual IS the surface a trace should rest on. A NULL slot is
    // treated the same way, because the engine substitutes the opaque default material.
    bool SpatialTraceDrawsOnlyNonOpaque(const UPrimitiveComponent* Component)
    {
        if (!Component)
        {
            return false;
        }
        const int32 NumMaterials = Component->GetNumMaterials();
        if (NumMaterials <= 0)
        {
            return false;
        }
        for (int32 Index = 0; Index < NumMaterials; ++Index)
        {
            const UMaterialInterface* Material = Component->GetMaterial(Index);
            if (!Material)
            {
                return false;
            }
            const EBlendMode BlendMode = Material->GetBlendMode();
            if (BlendMode == BLEND_Opaque || BlendMode == BLEND_Masked)
            {
                return false;
            }
        }
        return true;
    }

    // One component's verdict: effect/marker by class, or effect by the way it draws.
    bool SpatialTraceIsEffectPrimitive(const UPrimitiveComponent* Component)
    {
        return SpatialTraceIsEffectComponentClass(Component)
            || SpatialTraceDrawsOnlyNonOpaque(Component);
    }
}

namespace SpatialTraceUtils
{
    bool FSpatialHitFilter::Matches(AActor* Actor, const UPrimitiveComponent* Component) const
    {
        if (IsEmpty())
        {
            return true;
        }
        if (!Actor)
        {
            // A hit with no resolvable actor cannot be shown to satisfy a restriction;
            // reporting it would be exactly the silent-wrong-data failure this filter exists
            // to prevent.
            return false;
        }

        if (OnlyActors.Num() > 0)
        {
            bool bFound = false;
            for (const TWeakObjectPtr<AActor>& Wanted : OnlyActors)
            {
                if (Wanted.Get() == Actor)
                {
                    bFound = true;
                    break;
                }
            }
            if (!bFound)
            {
                return false;
            }
        }

        // MatchesEither applies the caller's resolved matchMode + caseSensitive to both the
        // internal name and the display label; an inactive filter matches everything, so
        // the IsActive() guard exists only to skip materializing the two strings.
        if (ActorFilter.IsActive()
            && !ActorFilter.MatchesEither(Actor->GetName(), Actor->GetActorLabel()))
        {
            return false;
        }

        if (OnlyClasses.Num() > 0 && !SpatialTraceMatchesAnyClass(Actor, OnlyClasses))
        {
            return false;
        }

        // ---- Exclusions. Evaluated last so an exclusion always beats an accept. ----
        if (ExcludeClasses.Num() > 0 && SpatialTraceMatchesAnyClass(Actor, ExcludeClasses))
        {
            return false;
        }

        if (ExcludeNames.Num() > 0)
        {
            const FString InternalName = Actor->GetName();
            const FString Label = Actor->GetActorLabel();
            for (const FString& Pattern : ExcludeNames)
            {
                if (Pattern.IsEmpty())
                {
                    continue;
                }
                if (InternalName.MatchesWildcard(Pattern, ESearchCase::IgnoreCase)
                    || Label.MatchesWildcard(Pattern, ESearchCase::IgnoreCase))
                {
                    return false;
                }
            }
        }

        // The component axis. Only the primitive that ANSWERED the query can decide it - the
        // actor of an instanced scatter is an ordinary AActor and says nothing - so a caller
        // who could not supply one gets no verdict here rather than a rejection on an unknown.
        if (ExcludeComponentClasses.Num() > 0 && Component
            && SpatialTraceClassChainMatches(Component->GetClass(), ExcludeComponentClasses))
        {
            return false;
        }

        // Last, and only when asked: the intrinsic test. It walks components and materials, so
        // it is the most expensive criterion here and runs only after every cheap one has had
        // its say.
        if (bExcludeEffectGeometry && IsEffectGeometryActor(Actor))
        {
            return false;
        }

        return true;
    }

    bool IsEffectGeometryActor(AActor* Actor)
    {
        if (!Actor)
        {
            return false;
        }

        TArray<UPrimitiveComponent*> Components;
        Actor->GetComponents<UPrimitiveComponent>(Components);

        int32 CollidableCount = 0;
        for (UPrimitiveComponent* Component : Components)
        {
            if (!Component || !Component->IsCollisionEnabled())
            {
                // A component a trace cannot hit cannot be the thing that was mistaken for
                // ground, so it gets no vote either way.
                continue;
            }
            ++CollidableCount;
            if (!SpatialTraceIsEffectPrimitive(Component))
            {
                // One opaque collidable component is enough: this actor really does present a
                // surface. Answering here rather than after the loop is what keeps a building
                // with a translucent window classified as ground.
                return false;
            }
        }

        // No collision-enabled primitive at all: nothing to misidentify. Reported as "not
        // effect geometry" so this predicate never becomes a second, quieter way of rejecting
        // actors the trace was never going to hit.
        return CollidableCount > 0;
    }

    FSpatialHit TraceLine(UWorld* World, const FVector& Start, const FVector& End,
                          ECollisionChannel Channel, bool bTraceComplex,
                          const TArray<AActor*>& IgnoreActors)
    {
        FSpatialHit Result;
        if (!World)
        {
            return Result;
        }

        // A degenerate (zero-length) segment can't produce a meaningful hit; treat
        // it as a miss instead of leaning on undefined trace behavior.
        if (Start.Equals(End))
        {
            return Result;
        }

        FCollisionQueryParams QueryParams(FName(TEXT("PinWrightSpatialTrace")), bTraceComplex);
        QueryParams.AddIgnoredActors(IgnoreActors);
        // Ask the physics backend for the struck triangle index. It is the engine's own
        // signal that the query resolved against a triangle mesh / heightfield rather than
        // a simple primitive, and it is what lets the response tell a caller WHICH
        // representation answered. Output-only: it does not change what the ray hits.
        QueryParams.bReturnFaceIndex = true;

        FHitResult Hit;
        if (World->LineTraceSingleByChannel(Hit, Start, End, Channel, QueryParams))
        {
            SpatialTraceFillHit(Result, Hit, bTraceComplex);
        }
        return Result;
    }

    FSpatialLayeredTraceResult TraceLineLayered(UWorld* World, const FVector& Start, const FVector& End,
                                                ECollisionChannel Channel, bool bTraceComplex,
                                                const TArray<AActor*>& IgnoreActors,
                                                const FSpatialLayeredTraceOptions& Options)
    {
        FSpatialLayeredTraceResult Result;
        if (!World || Start.Equals(End))
        {
            return Result;
        }

        const int32 MaxLayers = FMath::Clamp(Options.MaxLayers, 1, MaxAllowedLayers);
        const int32 MaxAccepted = FMath::Max(Options.MaxAcceptedHits, 1);

        // Peeled starts as the caller's ignore set and grows by one actor per layer, so a
        // blocking surface is removed from consideration before the next pass.
        TArray<AActor*> Peeled = IgnoreActors;

        for (int32 Layer = 0; Layer < MaxLayers; ++Layer)
        {
            const FSpatialHit Hit = TraceLine(World, Start, End, Channel, bTraceComplex, Peeled);
            if (!Hit.bHit)
            {
                // Ray ran out of geometry: the walk is complete, not truncated.
                return Result;
            }

            AActor* HitActor = Hit.HitActor.Get();
            // The struck component travels with the hit and is what ExcludeComponentClasses
            // reads: an instanced scatter is distinguishable only there, never on the actor.
            if (Options.Filter.Matches(HitActor, Hit.HitComponent.Get()))
            {
                Result.Hits.Add(Hit);
                if (Result.Hits.Num() >= MaxAccepted)
                {
                    return Result;
                }
            }
            else if (HitActor)
            {
                Result.RejectedActors.AddUnique(TWeakObjectPtr<AActor>(HitActor));
            }

            if (!HitActor)
            {
                // Nothing to add to the ignore set, so the next pass would return this same
                // hit forever. Stop rather than spin; the caller still gets what was found.
                return Result;
            }
            Peeled.AddUnique(HitActor);
        }

        // Budget exhausted while the ray was still hitting things - there may be more behind.
        Result.bTruncated = true;
        return Result;
    }

    FSpatialHit TraceGroundBelow(UWorld* World, const FBox& WorldBounds, float MaxDrop,
                                 const TArray<AActor*>& IgnoreActors)
    {
        if (!World || !WorldBounds.IsValid)
        {
            return FSpatialHit();
        }

        const FVector Center = WorldBounds.GetCenter();
        const FVector BottomCenter(Center.X, Center.Y, WorldBounds.Min.Z);
        const FVector End = BottomCenter - FVector(0.f, 0.f, FMath::Max(MaxDrop, 0.f));
        return TraceLine(World, BottomCenter, End, ECC_Visibility, /*bTraceComplex=*/false,
                         IgnoreActors);
    }

    bool ProbeFootprintOccupancy(UWorld* World, const FSpatialFootprintProbe& Probe,
                                 double InflateXYCm, TArray<FSpatialOccupant>& OutOccupants)
    {
        OutOccupants.Reset();
        if (!World)
        {
            return false;
        }

        const double Inflate = FMath::Max(InflateXYCm, 0.0);
        // A zero half-extent on any axis makes the query shape degenerate, which the physics
        // backend answers with nothing at all rather than with an error. Floor every axis.
        const FVector Half(
            FMath::Max(Probe.HalfExtent.X, UE_KINDA_SMALL_NUMBER) + Inflate,
            FMath::Max(Probe.HalfExtent.Y, UE_KINDA_SMALL_NUMBER) + Inflate,
            FMath::Max(Probe.HalfExtent.Z, UE_KINDA_SMALL_NUMBER));

        FCollisionQueryParams QueryParams(FName(TEXT("PinWrightFootprintProbe")),
                                         /*bTraceComplex=*/false);
        QueryParams.AddIgnoredActors(Probe.IgnoreActors);

        TArray<FOverlapResult> Overlaps;
        World->OverlapMultiByChannel(Overlaps, Probe.Center, Probe.Rotation, Probe.Channel,
                                     FCollisionShape::MakeBox(Half), QueryParams);

        const int32 MaxOccupants = FMath::Max(Probe.MaxOccupants, 1);
        bool bAnyOccupant = false;

        for (const FOverlapResult& Overlap : Overlaps)
        {
            UPrimitiveComponent* Component = Overlap.GetComponent();
            if (!Component)
            {
                continue;
            }
            AActor* Actor = Overlap.GetActor();
            if (!Probe.Filter.Matches(Actor, Component))
            {
                continue;
            }

            bAnyOccupant = true;
            if (OutOccupants.Num() >= MaxOccupants)
            {
                // The verdict is already decided; keep scanning only for the report, and the
                // report is full. A HISM can produce one overlap result per instance body, so
                // this is the difference between a bounded response and a thousand-entry one.
                break;
            }

            const int32 ItemIndex = MCP_OVERLAP_ITEM_INDEX(Overlap);
            bool bAlreadyRecorded = false;
            for (const FSpatialOccupant& Existing : OutOccupants)
            {
                if (Existing.Component.Get() == Component && Existing.InstanceIndex == ItemIndex)
                {
                    bAlreadyRecorded = true;
                    break;
                }
            }
            if (bAlreadyRecorded)
            {
                continue;
            }

            FSpatialOccupant Occupant;
            Occupant.Actor = Actor;
            Occupant.Component = Component;
            Occupant.InstanceIndex = ItemIndex;
            Occupant.bInstanced = Component->IsA<UInstancedStaticMeshComponent>();
            OutOccupants.Add(Occupant);
        }

        // Instance resolution, after de-duplication so the per-instance query runs once per
        // component rather than once per overlap result. An overlap carries a per-body item
        // index only when the component opted into multi-body overlap reporting
        // (UPrimitiveComponent::bMultiBodyOverlap, off by default), so on an ordinary scatter it
        // arrives as INDEX_NONE and the component has to be asked directly. The box handed to
        // GetInstancesOverlappingBox is the world AABB of the (possibly yawed) probe box, so the
        // named instance is a conservative pick within the same query volume.
        for (FSpatialOccupant& Occupant : OutOccupants)
        {
            if (!Occupant.bInstanced || Occupant.InstanceIndex != INDEX_NONE)
            {
                continue;
            }
            const UInstancedStaticMeshComponent* Instanced =
                Cast<UInstancedStaticMeshComponent>(Occupant.Component.Get());
            if (!Instanced)
            {
                continue;
            }
            const FBox WorldBox =
                FBox(-Half, Half).TransformBy(FTransform(Probe.Rotation, Probe.Center));
            const TArray<int32> Touched =
                Instanced->GetInstancesOverlappingBox(WorldBox, /*bBoxInWorldSpace=*/true);
            if (Touched.Num() > 0)
            {
                Occupant.InstanceIndex = Touched[0];
            }
        }

        return bAnyOccupant;
    }

    FSpatialClearanceResult MeasureFootprintClearance(UWorld* World, const FSpatialFootprintProbe& Probe,
                                                      double MinClearanceCm, double MaxClearanceCm,
                                                      int32 Iterations)
    {
        FSpatialClearanceResult Result;
        TArray<FSpatialOccupant> Occupants;

        if (ProbeFootprintOccupancy(World, Probe, 0.0, Occupants))
        {
            Result.Binders = MoveTemp(Occupants);
            return Result;
        }
        Result.bClear = true;

        const double MinClearance = FMath::Max(MinClearanceCm, 0.0);
        const double MaxClearance = FMath::Max(MaxClearanceCm, MinClearance);

        double Lo = 0.0;          // shown clear
        double Hi = MaxClearance; // upper end of the interval being bisected

        if (MinClearance > 0.0)
        {
            if (ProbeFootprintOccupancy(World, Probe, MinClearance, Occupants))
            {
                // The threshold itself is blocked, so the pose does not meet the minimum.
                // Measure what room it DOES have inside [0, MinClearance] anyway - a rejection
                // that carries a number and a binder is actionable; a bare false is not.
                Result.Binders = Occupants;
                Hi = MinClearance;
            }
            else
            {
                Result.bMeetsMinimum = true;
                Lo = MinClearance;
            }
        }
        else
        {
            Result.bMeetsMinimum = true;
        }

        if (Result.bMeetsMinimum)
        {
            if (Lo >= MaxClearance)
            {
                // The threshold IS the ceiling; there is nothing left to bisect.
                Result.ClearanceCm = Lo;
                Result.bCapped = true;
                return Result;
            }
            if (!ProbeFootprintOccupancy(World, Probe, MaxClearance, Occupants))
            {
                // Free at the ceiling: the real gap is at least this, and no binder was reached.
                Result.ClearanceCm = MaxClearance;
                Result.bCapped = true;
                return Result;
            }
            Result.Binders = Occupants;
        }

        const int32 Steps = FMath::Clamp(Iterations, 1, 16);
        for (int32 Step = 0; Step < Steps; ++Step)
        {
            const double Mid = 0.5 * (Lo + Hi);
            if (ProbeFootprintOccupancy(World, Probe, Mid, Occupants))
            {
                Hi = Mid;
                // Keep the binders from the TIGHTEST blocked inflation: that is the thing
                // actually closest to the footprint, not merely something within the ceiling.
                Result.Binders = Occupants;
            }
            else
            {
                Lo = Mid;
            }
        }

        Result.ClearanceCm = Lo;
        return Result;
    }
}
