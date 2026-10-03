// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

class UStaticMesh;
struct FStaticMeshLODResources;
struct FStaticMeshSection;

// Where a compiled StaticMesh's geometry is, read off its render data: a box per section and per
// slot (static_mesh.describe / static_mesh.json), and LOD0 connected components ("islands", the
// per-part answer, opt-in on static_mesh.describe because it is the one full traversal here).
namespace StaticMeshSpatialExtent
{
    // Mesh-local box over the vertex positions the section's index range references. Invalid
    // (IsValid == 0) when the section has no triangles or the LOD's CPU geometry copy is absent.
    FBox ComputeSectionBox(const FStaticMeshLODResources& LOD, const FStaticMeshSection& Section);

    // {min, max, size, center} as {x, y, z} objects, or JSON null for an invalid box.
    TSharedPtr<FJsonValue> BoxToJsonValue(const FBox& Box);

    // LOD0 connected components. Triangles are connected when they share a vertex POSITION
    // (exact match), so render vertices split at UV seams / hard edges are welded back together
    // and an island is a spatially contiguous piece of surface. Rows are sorted by triangleCount
    // descending: {index, triangleCount, boundingBox, materialSlots[{materialIndex,
    // materialSlotName}]}; only the first MaxRows are serialized, OutIslandCount is the full count.
    // Returns false with OutUnavailableReason when LOD0's CPU geometry is not resident; a mesh with
    // no render data or no LOD0 returns true with no islands.
    bool BuildLod0Islands(const UStaticMesh* Mesh, int32 MaxRows, TArray<TSharedPtr<FJsonValue>>& OutIslands,
        int32& OutIslandCount, FString& OutUnavailableReason);
}
