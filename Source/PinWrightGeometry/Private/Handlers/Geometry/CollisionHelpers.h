// Copyright (c) 2026 Alexander Penkin. MIT License.

// CollisionHelpers.h - Version-compat wrapper for generating and applying simple collision
//   from a dynamic mesh. Three handlers (generate_collision, generate_complex_collision,
//   simplify_collision) share an identical version-guard block; this helper centralises it.
#pragma once
#include "Misc/EngineVersionComparison.h"
#include "Components/DynamicMeshComponent.h"
#include "UDynamicMesh.h"
#include "GeometryScript/CollisionFunctions.h"
#include "PhysicsEngine/BodySetup.h"  // UBodySetup / FKAggregateGeom — shape-count reads and the StaticMesh collision transfer
#include "Engine/StaticMesh.h"        // UStaticMesh::CreateBodySetup / GetBodySetup — carry simple collision through the bake

namespace GeometryUtils
{

// Generate simple collision shapes from Mesh using Options, apply them to Component, and
// return the resulting shape count.
//
// On UE 5.4 GenerateCollisionFromMesh does not exist; SetDynamicMeshCollisionFromMesh
// generates and applies collision in one call, then GetSimpleCollisionFromComponent reads
// the result back so ShapeCount can be reported.  On UE 5.5+ GenerateCollisionFromMesh
// returns the collision object directly before it is applied.
inline int32 GenerateAndApplyCollision(
    UDynamicMesh* Mesh,
    UDynamicMeshComponent* Component,
    const FGeometryScriptCollisionFromMeshOptions& Options)
{
#if UE_VERSION_OLDER_THAN(5,4,0)
    // UE 5.3: FGeometryScriptSimpleCollision and the GetSimpleCollision* helpers do not exist
    // yet. Apply the collision, then read the resulting shape count off the component's body
    // setup aggregate geometry.
    UGeometryScriptLibrary_CollisionFunctions::SetDynamicMeshCollisionFromMesh(
        Mesh, Component, Options, nullptr);
    if (UBodySetup* BodySetup = Component ? Component->GetBodySetup() : nullptr)
    {
        return BodySetup->AggGeom.GetElementCount();
    }
    return 0;
#elif UE_VERSION_OLDER_THAN(5,5,0)
    UGeometryScriptLibrary_CollisionFunctions::SetDynamicMeshCollisionFromMesh(
        Mesh, Component, Options, nullptr);
    FGeometryScriptSimpleCollision Collision =
        UGeometryScriptLibrary_CollisionFunctions::GetSimpleCollisionFromComponent(Component, nullptr);
    return UGeometryScriptLibrary_CollisionFunctions::GetSimpleCollisionShapeCount(Collision);
#else
    FGeometryScriptSimpleCollision Collision =
        UGeometryScriptLibrary_CollisionFunctions::GenerateCollisionFromMesh(Mesh, Options, nullptr);
    FGeometryScriptSetSimpleCollisionOptions SetOptions;
    UGeometryScriptLibrary_CollisionFunctions::SetSimpleCollisionOfDynamicMeshComponent(
        Collision, Component, SetOptions, nullptr);
    return UGeometryScriptLibrary_CollisionFunctions::GetSimpleCollisionShapeCount(Collision);
#endif
}

// Carry a DynamicMeshComponent's simple collision (AggGeom primitives + trace flag) onto a
// freshly-baked StaticMesh's own BodySetup. CreateNewStaticMeshAssetFromMesh (the bake behind
// geometry.convert_to_static_mesh) rebuilds geometry only and leaves the new StaticMesh's
// BodySetup empty, so a prior geometry.generate_collision — which applies the shapes to the
// source DynamicMeshComponent's BodySetup, not to the mesh — is otherwise silently dropped by
// the bake (static_mesh.describe then reports box:0 despite a successful shapeCount:1). Copies
// the aggregate geometry so the simple collision survives the convert. Returns the number of
// primitives carried over (0 when the source has no simple collision, in which case nothing is
// copied and the baked mesh's CTF_UseDefault is left as-is).
inline int32 TransferSimpleCollisionToStaticMesh(UDynamicMeshComponent* SourceComponent, UStaticMesh* TargetMesh)
{
    // generate_collision applies the simple shapes to the component's BodySetup, so that is
    // where the collision to carry lives.
    const UBodySetup* SourceBodySetup = SourceComponent ? SourceComponent->GetBodySetup() : nullptr;
    if (!SourceBodySetup || !TargetMesh)
    {
        return 0;
    }
    // Count the source's primitives once: the AggGeom copy below makes the target's count
    // identical, so this same value is the return.
    const int32 ElementCount = SourceBodySetup->AggGeom.GetElementCount();
    if (ElementCount == 0)
    {
        return 0;
    }

    // CreateBodySetup is a no-op when the baked mesh already owns one (it does — the bake
    // leaves an empty CTF_UseDefault BodySetup); GetBodySetup then returns that target.
    TargetMesh->CreateBodySetup();
    UBodySetup* TargetBodySetup = TargetMesh->GetBodySetup();
    if (!TargetBodySetup)
    {
        return 0;
    }

    TargetBodySetup->Modify();
    TargetBodySetup->AggGeom = SourceBodySetup->AggGeom;
    TargetBodySetup->CollisionTraceFlag = SourceBodySetup->CollisionTraceFlag;
    // Drop any stale cooked physics so the copied primitives recook on load/save.
    TargetBodySetup->InvalidatePhysicsData();
    TargetMesh->MarkPackageDirty();
    return ElementCount;
}

} // namespace GeometryUtils
