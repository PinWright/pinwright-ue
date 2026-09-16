// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once
#include "CoreMinimal.h"
#include "Engine/EngineTypes.h" // ECollisionChannel
#include "UObject/WeakObjectPtr.h"
#include "Utils/NameMatchFilter.h" // NameMatch::FFilter - the shared name/label match policy

class UWorld;
class AActor;
class UPrimitiveComponent;

// Shared world line-trace helpers for the spatial.* handler family. This is the
// single collision-query seam: spatial.raycast today, and (later) raycast_screen,
// place_on_surface, and verify_placement all route their world hits through here so
// the FCollisionQueryParams setup, the editor-vs-PIE physics-scene handling, and the
// result shape live in one place instead of being re-derived per handler.
namespace SpatialTraceUtils
{
    // Default and hard ceiling for the layered trace's pass budget. Each layer costs
    // one line trace, so the ceiling bounds worst-case cost per RPC. 32 layers clears
    // any realistic stack of props/foliage between a probe and the ground.
    constexpr int32 DefaultMaxLayers = 32;
    constexpr int32 MaxAllowedLayers = 256;

    // One resolved line-trace hit. Distances are in unreal units (cm). Weak actor/
    // component handles so a stored FSpatialHit never keeps a destroyed actor alive
    // (relevant once verify/place cache hits across ticks). bHit==false leaves every
    // other field at its zero default.
    struct FSpatialHit
    {
        bool bHit = false;
        FVector Location = FVector::ZeroVector; // world impact point (cm)
        FVector Normal = FVector::ZeroVector;   // unit surface normal at the impact
        TWeakObjectPtr<AActor> HitActor;
        TWeakObjectPtr<UPrimitiveComponent> HitComponent;
        float Distance = 0.f; // Start -> impact distance (cm)

        // Index of the struck triangle when the query resolved against a triangle mesh
        // or heightfield (the COMPLEX collision representation); INDEX_NONE for a
        // simple-primitive hit or when the physics backend reported none. Requested via
        // FCollisionQueryParams::bReturnFaceIndex on every trace this module issues.
        int32 FaceIndex = INDEX_NONE;

        // Count of SIMPLE collision primitives (box/sphere/capsule/convex) on the struck
        // component's UBodySetup, or -1 when the component exposes no body setup at all
        // (Landscape's heightfield collision component is the common -1 case: the
        // heightfield IS its collision, there is no simple/complex split to report).
        //
        // 0 is the load-bearing value: it means the struck component has NO simple
        // collision whatsoever, so a bTraceComplex query resolved against its RENDER
        // triangles. That is correct engine behaviour and the exact silent-wrong-height
        // trap where a collisionless foliage mesh blocks a downward ground probe.
        int32 SimpleCollisionShapes = -1;

        // True when this hit is a render-geometry hit: the query traced complex collision
        // AND the struck component has zero simple collision primitives. Set by the trace
        // helpers (they know bTraceComplex); handlers surface it verbatim so a caller can
        // detect the case without knowing the engine rule.
        bool bRenderGeometryHit = false;
    };

    // Which hits a layered trace is willing to ACCEPT. Every field empty = accept
    // everything (the legacy "first blocking hit wins" behaviour). Populated criteria
    // are ANDed.
    struct FSpatialHitFilter
    {
        // Accept only hits on one of these exact actors. Compared by pointer identity, so
        // a duplicate display label cannot silently widen the filter. Empty = unrestricted.
        TArray<TWeakObjectPtr<AActor>> OnlyActors;

        // Accept only hits whose actor internal NAME or display LABEL matches. Uses the
        // shared NameMatch policy (Utils/NameMatchFilter.h) - the same pattern +
        // matchMode + caseSensitive vocabulary actor.list and blueprint.graph.find_nodes
        // speak - rather than a per-verb rule, so the filter semantics never drift across
        // the read verbs. Inactive (empty pattern) = unrestricted.
        NameMatch::FFilter ActorFilter;

        // Accept only hits whose actor class - or ANY ancestor class - matches one of
        // these by class name or class path. Ancestry is what lets a single
        // "LandscapeProxy" entry cover both ALandscape and ALandscapeStreamingProxy,
        // which is the terrain-probe case this exists for. Matched as a plain
        // case-insensitive substring: these are engine class identifiers, not user-typed
        // labels, so the matchMode/caseSensitive knobs govern ActorFilter only.
        // Empty = unrestricted.
        TArray<FString> OnlyClasses;

        // REJECT hits whose actor class - or ANY ancestor class - matches one of these, by
        // the same case-insensitive class-name/class-path substring rule as OnlyClasses.
        // Exclusions are evaluated AFTER the accept criteria and always win.
        //
        // Why both directions exist: "the ground is the landscape" is expressible as an
        // accept list, but "the ground is anything solid EXCEPT foliage and VFX cards" is
        // not - those actors share no single class to accept around. Empty = nothing excluded.
        TArray<FString> ExcludeClasses;

        // REJECT hits whose actor internal NAME or display LABEL matches one of these
        // wildcard patterns (FString::MatchesWildcard, case-insensitive; `*` and `?`).
        // Exclusions win over the accept criteria, as above.
        //
        // The case this exists for: a project whose haze/fog/decal cards share a naming
        // convention (e.g. an "FX_*" or "FG_*" prefix) can peel every one of them off a ground
        // probe in one entry, including cards whose collision setup makes them indistinguishable
        // from real geometry to a trace. That happened here: a downward probe reported a haze
        // card as terrain and lifted a whole batch of characters ~4300 uu into the air.
        //
        // A name pattern is project knowledge, so it is a CALLER-supplied list and no preset
        // populates it; bExcludeEffectGeometry below is the version that needs no such
        // knowledge. Empty = nothing excluded.
        TArray<FString> ExcludeNames;

        // REJECT hits answered by a COMPONENT whose class - or any ancestor class - matches one
        // of these, by the same case-insensitive class-name/class-path substring rule
        // ExcludeClasses applies to actors. Exclusions win over the accept criteria, as above.
        //
        // Why the actor axis is not enough: a scatter carried as ISM/HISM components on an
        // ordinary AActor has NO actor class that distinguishes it - the holder is an AActor
        // like every other - so ExcludeClasses cannot name it and ExcludeNames degrades to a
        // project-prefix pattern, the anti-pattern the ExcludeNames comment above records.
        // Only the struck component says what answered the probe, and this is the axis that
        // can read it. The case it exists for: a vegetation or debris scatter a caller does not
        // want treated as ground, e.g. {"excludeComponentClasses":
        // ["HierarchicalInstancedStaticMeshComponent"]}.
        //
        // Deliberately caller-supplied for the generic instanced classes: an ISM/HISM scatter of
        // paving stones, rocks or modular tiles IS legitimate ground, so no preset may exclude
        // UInstancedStaticMeshComponent wholesale. The any_solid preset populates only the two
        // engine component classes that exist for vegetation and nothing else - see ApplyPreset.
        //
        // A hit whose component could not be resolved is never excluded by this axis: it cannot
        // be SHOWN to match, and rejecting on an unknown is the same silent wrong answer in the
        // other direction. Empty = nothing excluded.
        TArray<FString> ExcludeComponentClasses;

        // REJECT hits on actors whose geometry is intrinsically effect/marker geometry rather
        // than ground — see IsEffectGeometryActor for the exact test. Intrinsic on purpose: it
        // protects a project whose effect cards follow a naming convention we have never seen,
        // which ExcludeNames cannot. Default false, so nothing changes for a caller who does
        // not ask; the any_solid preset turns it on.
        bool bExcludeEffectGeometry = false;

        bool IsEmpty() const
        {
            return OnlyActors.Num() == 0 && !ActorFilter.IsActive() && OnlyClasses.Num() == 0
                && ExcludeClasses.Num() == 0 && ExcludeNames.Num() == 0
                && ExcludeComponentClasses.Num() == 0
                && !bExcludeEffectGeometry;
        }

        // AND of every populated ACCEPT criterion, then NAND of every populated EXCLUDE
        // criterion. A null actor never matches a non-empty filter: an unidentifiable hit
        // cannot be shown to satisfy a caller's restriction.
        //
        // Component is the primitive that ANSWERED the query, and is the only input that can
        // decide ExcludeComponentClasses - the actor cannot, because an instanced scatter's
        // holder is an ordinary AActor. It defaults to nullptr so a caller testing an actor in
        // isolation still compiles and gets every actor-scoped criterion; the trace and overlap
        // paths both have the component in hand and pass it.
        bool Matches(AActor* Actor, const UPrimitiveComponent* Component = nullptr) const;
    };

    // True when Actor presents NO opaque collidable surface, i.e. everything a trace could hit
    // on it is effect or marker geometry rather than ground. Deliberately intrinsic — derived
    // from what the actor IS, not from what it is called — so a project that has never read our
    // documentation gets the same protection as one that names its cards to our convention.
    //
    // An actor qualifies when it owns at least one collision-enabled primitive component and
    // EVERY such component is one of:
    //   - an effect / editor-marker component class (particle, Niagara, decal, billboard,
    //     arrow, text render), matched by class-name ancestry rather than by StaticClass() so
    //     no plugin module has to be linked to recognise it; or
    //   - a component whose every assigned material is non-opaque (blend mode other than
    //     BLEND_Opaque / BLEND_Masked — the translucent, additive and modulated families that
    //     haze, fog, glow and light-shaft cards are drawn with).
    //
    // The "every" is what keeps it safe: one opaque collidable component is enough for the
    // actor to be real ground, so a building with a translucent window is never disqualified.
    // An actor with no collision-enabled primitive at all returns false — it cannot block a
    // trace, so it is not the failure this guards against and does not need a verdict.
    //
    // A component with zero material slots (a collision-only shape) counts as opaque: it draws
    // nothing to be translucent about, and treating it as an effect card would disqualify the
    // blocking volumes that legitimately are the surface.
    bool IsEffectGeometryActor(AActor* Actor);

    struct FSpatialLayeredTraceOptions
    {
        // Hard bound on trace passes, i.e. how many distinct actors may be peeled off the
        // ray. Also the cost bound: the layered trace issues at most this many line traces.
        int32 MaxLayers = DefaultMaxLayers;
        // Stop as soon as this many hits have PASSED the filter. 1 gives "the nearest hit
        // that satisfies the filter" for the single-hit response shape.
        int32 MaxAcceptedHits = DefaultMaxLayers;
        FSpatialHitFilter Filter;
    };

    struct FSpatialLayeredTraceResult
    {
        // Accepted hits, nearest first, at most one per actor.
        TArray<FSpatialHit> Hits;
        // Actors that blocked the ray but failed the filter, in the order they were peeled.
        // Handlers report these so a filtered trace says what it skipped instead of
        // silently swallowing it.
        TArray<TWeakObjectPtr<AActor>> RejectedActors;
        // True when the pass budget ran out while the ray was still hitting geometry, so
        // there may be more behind the last reported layer. Raise maxHits to see further.
        bool bTruncated = false;
    };

    // Single line trace from Start to End against Channel. Wraps
    // UWorld::LineTraceSingleByChannel with an FCollisionQueryParams that ignores
    // IgnoreActors and honors bTraceComplex (per-triangle vs simple collision).
    // Returns a miss (bHit==false) for a null World or a degenerate zero-length
    // segment rather than asserting. Runs against whatever physics scene World owns:
    // in the editor (non-PIE) that is the editor world's scene, which does answer
    // queries for placed/spawned StaticMeshActors (they carry blocking collision by
    // default) - the same path the editor click-select uses.
    FSpatialHit TraceLine(UWorld* World, const FVector& Start, const FVector& End,
                          ECollisionChannel Channel = ECC_Visibility,
                          bool bTraceComplex = false,
                          const TArray<AActor*>& IgnoreActors = TArray<AActor*>());

    // Layered trace: line-trace Start->End, record the nearest hit, then EXCLUDE that
    // hit's actor and trace again, so the caller can see what is BEHIND a blocking
    // surface and pick the hit it actually wants.
    //
    // Why peel instead of UWorld::LineTraceMultiByChannel: the engine's multi-trace
    // stops at the first BLOCKING hit by design (it returns overlaps up to that point,
    // not solids behind it). A ground probe blocked by a collisionless foliage mesh
    // under bTraceComplex therefore gets nothing useful out of the engine multi-trace -
    // peeling the blocker and re-tracing is the only way to reach the landscape behind
    // it, and it is exactly the workaround every caller was hand-rolling.
    //
    // Semantics: at most ONE hit per actor, ordered nearest-first, bounded at
    // Options.MaxLayers passes. Hits whose actor fails Options.Filter are peeled but not
    // recorded (they land in RejectedActors), so a filtered call returns only accepted
    // hits. A miss ends the walk with a complete result; exhausting the layer budget ends
    // it with bTruncated set.
    FSpatialLayeredTraceResult TraceLineLayered(UWorld* World, const FVector& Start, const FVector& End,
                                                ECollisionChannel Channel, bool bTraceComplex,
                                                const TArray<AActor*>& IgnoreActors,
                                                const FSpatialLayeredTraceOptions& Options);

    // Straight-down trace from the bottom-center of WorldBounds, going MaxDrop cm
    // below it, for grounded/support checks (used later by verify/place). Pass the
    // actor being tested in IgnoreActors so the trace reports the surface under it,
    // not the actor itself. Returns a miss for a null World or an invalid box.
    FSpatialHit TraceGroundBelow(UWorld* World, const FBox& WorldBounds, float MaxDrop,
                                 const TArray<AActor*>& IgnoreActors = TArray<AActor*>());

    // ---- Footprint occupancy (spatial.find_clear_placement) -----------------------------
    //
    // A ray answers "what is at this point"; these two answer "is there a BOX of clear space
    // here, and if not what is standing in it". They are backed by UWorld::OverlapMultiByChannel
    // rather than by client-side OBB/SAT arithmetic for one reason that is not convenience: a
    // physics overlap resolves against the per-INSTANCE bodies of an ISM/HISM, so a scatter is
    // visible to it. Every name-based occupancy input in this namespace is blind to that - a
    // holder's single AABB spans the whole scatter, so naming the holder says nothing about which
    // instance is in the way.

    // One thing standing in a probed footprint.
    struct FSpatialOccupant
    {
        TWeakObjectPtr<AActor> Actor;
        TWeakObjectPtr<UPrimitiveComponent> Component;

        // Instance index within an ISM/HISM component, or INDEX_NONE for a plain component.
        // Resolved from the engine's own per-instance query, never re-derived from transforms.
        int32 InstanceIndex = INDEX_NONE;

        // True when Component is a UInstancedStaticMeshComponent - HISM and the foliage
        // component both derive from it, so one flag covers every scatter shape.
        bool bInstanced = false;
    };

    // The box a footprint probe tests, before any clearance inflation.
    struct FSpatialFootprintProbe
    {
        FVector Center = FVector::ZeroVector;     // world centre of the box
        FVector HalfExtent = FVector::ZeroVector; // half size along the box's own axes (cm)
        FQuat Rotation = FQuat::Identity;         // the pose's yaw
        ECollisionChannel Channel = ECC_Visibility;

        // Which overlapped actors COUNT as obstacles. An empty filter counts everything the
        // channel answers for; the same policy object spatial.raycast's hit filters use.
        FSpatialHitFilter Filter;

        // Excluded from the query entirely (engine-level ignore list), so they are neither
        // obstacles nor reported. The actor being placed and the surface it rests on go here.
        TArray<AActor*> IgnoreActors;

        // Stop collecting after this many DISTINCT occupants. The verdict is unaffected - one
        // occupant is already a rejection - so this only bounds how much the caller is told.
        int32 MaxOccupants = 4;
    };

    // Box overlap at the probe pose, with the box grown by InflateXYCm on its two HORIZONTAL
    // axes only (vertical size is never inflated: a footprint search asks about room around the
    // thing, not headroom above it). Returns true when at least one occupant passed the filter,
    // and fills OutOccupants with up to Probe.MaxOccupants of them, de-duplicated per
    // (component, instance). False for a null World or a degenerate box.
    bool ProbeFootprintOccupancy(UWorld* World, const FSpatialFootprintProbe& Probe,
                                 double InflateXYCm, TArray<FSpatialOccupant>& OutOccupants);

    struct FSpatialClearanceResult
    {
        // The un-inflated footprint itself was free. False means Binders is what is standing in it.
        bool bClear = false;

        // The footprint was free at MinClearanceCm as well - a DIRECT probe at exactly that
        // inflation, never the bisected number compared against it. That distinction is the
        // whole reason this flag exists: ClearanceCm is a lower bound, so a pose whose real
        // clearance is exactly the caller's threshold would fail a `>=` test against it.
        // Equal to bClear when MinClearanceCm <= 0.
        bool bMeetsMinimum = false;

        // Largest horizontal inflation SHOWN to be free, in cm - a lower bound, resolved to
        // the bisected interval / 2^Iterations. 0 when bClear is false.
        double ClearanceCm = 0.0;

        // The footprint was still free at the full MaxClearanceCm, so ClearanceCm is the probe
        // ceiling rather than a measurement of the real gap. Binders is empty in this case.
        bool bCapped = false;

        // What ended the measurement: the occupants of the failed footprint when bClear is
        // false, otherwise the occupants at the smallest inflation shown to be blocked - i.e.
        // what binds this pose's clearance.
        TArray<FSpatialOccupant> Binders;
    };

    // Bisects ProbeFootprintOccupancy to MEASURE the free space around the footprint instead of
    // only answering yes/no at one distance. Valid because the predicate is monotone - growing
    // the box can only add overlaps - so it has exactly one crossing. MinClearanceCm is probed
    // directly (and becomes the bisection's known-clear floor when it passes); the remaining
    // room is bisected up to MaxClearanceCm. Costs 2-3 + Iterations overlap queries for a clear
    // pose and 1 for a blocked one.
    FSpatialClearanceResult MeasureFootprintClearance(UWorld* World, const FSpatialFootprintProbe& Probe,
                                                      double MinClearanceCm, double MaxClearanceCm,
                                                      int32 Iterations);
}
