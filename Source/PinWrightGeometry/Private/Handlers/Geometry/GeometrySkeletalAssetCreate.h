// Copyright (c) 2026 Alexander Penkin. MIT License.

// GeometrySkeletalAssetCreate.h - value-in/value-out DynamicMesh -> USkeletalMesh creation.
//
// The RPC wrapper and a source compiler use the same seam.  In particular, neither caller
// needs to hand FHandlerContext into the asset construction path or repeat the engine utility's
// collision, provenance, material, and save decisions.
#pragma once

#include "CoreMinimal.h"

#include "PwSource/PwDiagnostic.h"
#include "Utils/AssetSaveState.h"

class UDynamicMesh;
class USkeletalMesh;
class USkeleton;

struct FSkeletalMeshCreateSpec
{
    FString AssetPath;

    // Permission for an unstamped or differently stamped occupant, or for discarding inventoried
    // live state. A same-class occupant is rebuilt in place so its referencers continue to resolve.
    bool bOverwrite = false;

    USkeleton* Skeleton = nullptr;

    // Slot names are in mesh material-ID order. Missing trailing names are supplied as
    // deterministic MaterialN placeholders through the highest dynamic-mesh material ID before
    // the engine build; empty means that no explicit material bindings were requested.
    TArray<FString> MaterialSlots;
    TMap<FString, FString> MaterialBindings;

    bool bRecomputeNormals = false;
    bool bRecomputeTangents = false;

    // Empty SourcePath means that no model provenance stamp is written.
    FString SourcePath;
    FString SourceHash;

    // False leaves both packages dirty for a later explicit flush.
    bool bSave = true;
};

struct FSkeletalMeshMaterialCoverage
{
    int32 RequiredMaterialSlotCount = 1;
    int32 MaterialSlotCount = 0;
};

struct FSkeletalMeshCreateResult
{
    bool bSuccess = false;
    FString ErrorCode;
    FString ErrorMessage;
    TArray<FString> Warnings;
    TArray<FPwDiagnostic> Diagnostics;
    TArray<FString> UnboundSlots;

    // What the rebuild destroyed and did not put back: existing morph-target names, the existing
    // physics-asset path, and any material the occupant had bound that the new source does not
    // rebind. The engine utility empties GetMaterials() unconditionally
    // (CreateSkeletalMeshUtil.cpp), so a source that names no material leaves every previous
    // binding gone; reporting only the physics asset and morph targets made that silent.
    TArray<FString> ClearedFeatures;

    USkeletalMesh* Asset = nullptr;
    FString PackageName;
    FString SkeletonPackageName;
    bool bUpdatedInPlace = false;

    // Counts read from the created asset's imported LOD0 sections, not from the source mesh.
    int32 TriangleCount = 0;
    int32 VertexCount = 0;
    FSkeletalMeshMaterialCoverage MaterialCoverage;

    bool bSavedToDisk = false;
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    // NotRequested means this invocation observed no serialized skeleton change and therefore
    // did not ask the shared skeleton package to save.
    EAssetSaveState SkeletonSaveState = EAssetSaveState::NotRequested;
    bool bPendingFlush = false;
    int64 SizeBytes = 0;
};

// Create (or rebuild in place) a SkeletalMesh from Mesh.  The engine skeletal utility owns the
// one FScopedSkeletalMeshPostEditChange/build boundary; this seam deliberately does not call
// PostEditChange again after it returns.
FSkeletalMeshCreateResult CreateSkeletalMesh(UDynamicMesh* Mesh, const FSkeletalMeshCreateSpec& Spec);

// Imported skeletal sections reference the asset-wide material array by index. Repair the
// post-build array without compacting those indices, filling null entries and padding through
// the highest imported section MaterialIndex (with at least one slot).
bool EnsureSkeletalMeshMaterialSlots(
    USkeletalMesh* Mesh, FSkeletalMeshMaterialCoverage& OutCoverage);
