// Copyright (c) 2026 Alexander Penkin. MIT License.

// GeometryAssetCreate.h - Single-build DynamicMesh -> UStaticMesh asset creation.
//
// Why this exists rather than another call to CreateNewStaticMeshAssetFromMesh: that wrapper
// hides FStaticMeshAssetOptions::AssetMaterials and NumMaterialSlots (the engine source has the
// assignment commented out with "we could allow passing in materials as an option..."),
// hard-codes bDeferPostEditChange = false, and then calls PostEditChange() a SECOND time itself
// (CreateNewAssetUtilityFunctions.cpp:309-318). So the asset is built twice, and everything the
// caller actually wants on it - materials, lightmap index, simple collision - has to be patched
// on afterwards, onto an asset that has already been built and whose slots were never populated
// (StaticMeshSetMaterialHandler.cpp:7, "slots were never populated").
//
// With bDeferPostEditChange = true the returned asset has NEVER been built: CreateStaticMeshAsset
// guards its only PostEditChange() on that flag (CreateStaticMeshUtil.cpp:240-243) and
// CommitMeshDescription does not build. The honest framing is therefore not creation-time versus
// post-hoc but PRE-FIRST-BUILD versus POST-FIRST-BUILD, and everything below stays on the
// pre-build side with exactly one build.
#pragma once

#include "CoreMinimal.h"

#include "BodySetupEnums.h"              // ECollisionTraceFlag
#include "Engine/AssetUserData.h"
#include "PwSource/PwDiagnostic.h"
#include "Utils/AssetSaveState.h"        // EAssetSaveState
#include "PhysicsEngine/AggregateGeom.h" // FKAggregateGeom
#include "UObject/Object.h"

#include "GeometryAssetCreate.generated.h"

class UDynamicMesh;
class UStaticMesh;

// Provenance stamp attached to every asset this file generates from a .pwmodel source.
//
// UStaticMesh implements IInterface_AssetUserData and its AssetUserData array is a serialised
// UPROPERTY (StaticMesh.h:1598-1600, :2278-2283), so the stamp survives save/load and the
// question "is this asset generated, and from what?" has an answer stored ON the asset instead
// of being reconstructed after the fact. Reconstructing it is what a project ends up doing
// instead, and it does not scale: one level needed 231 placed actors reconciled by hand against
// the sources that were supposed to have produced them.
//
// SourcePath is stored project-relative when the source lies under the project directory and
// absolute otherwise, following the plugin's WikiOutputDirectory / AssetDumpRootDirectory idiom.
// Absolute-always would mismatch on every second machine and train the reflex of always passing
// overwrite=true, which is exactly the signal the stamp exists to preserve.
//
// SourceHash is recorded but not consulted in M1. Content-hash matching would be wrong as an
// overwrite gate - an edited source hashes differently and editing IS the iteration loop - but
// recording it now avoids a later migration to add it.
UCLASS()
class UPwModelAssetUserData : public UAssetUserData
{
    GENERATED_BODY()

public:
    UPROPERTY()
    FString SourcePath;

    UPROPERTY()
    FString SourceHash;

    UPROPERTY()
    int32 GeneratedStateVersion = 0;

    UPROPERTY()
    TMap<FString, FString> GeneratedState;
};

// Everything the created asset needs, supplied as INPUT so it can be written before the single
// build rather than patched onto a built asset.
struct FStaticMeshCreateSpec
{
    // Long package path of the asset to create, e.g. "/Game/Generated/Bracket". The leaf name
    // is the asset name.
    FString AssetPath;

    // PERMISSION, not mechanism: "I grant permission to replace content at AssetPath that this
    // path did not generate." It is read by exactly one gate - the provenance check - and it
    // does not choose how the write happens. A same-class occupant is ALWAYS rebuilt in place
    // (see FStaticMeshCreateResult::bUpdatedInPlace), with or without this flag, so its
    // referencers keep resolving either way and ASSET_IN_USE is unreachable from here.
    //
    // NOT the normal path: clean same-source iteration succeeds without it. It is needed for an
    // occupant carrying no stamp, one stamped with a different SourcePath, or an explicit choice
    // to discard live state named by PWSRC_RECOMPILE_UNMANAGED_STATE.
    //
    // It used to be handed to AssetCreatePolicy::Resolve as bOverwriteRequested, which put the
    // write on the delete-then-recreate branch and deadlocked against the provenance gate: each
    // refusal named the other's remedy and neither worked on a referenced, unstamped asset.
    bool bOverwrite = false;

    // Slot names in material-ID order: index == the material ID carried on the mesh triangles.
    // Empty means one auto-named default slot, matching the engine util's minimum.
    TArray<FString> MaterialSlots;

    // Slot name -> "/Game/..." UMaterialInterface path. A slot with no binding, or one whose
    // binding fails to load, keeps the default surface material and is reported in
    // FStaticMeshCreateResult::UnboundSlots.
    TMap<FString, FString> MaterialBindings;

    // UV channel to use as the lightmap channel, or INDEX_NONE to leave the asset default.
    int32 LightMapChannel = INDEX_NONE;

    // Lightmap texture resolution in texels per side, written to BOTH UStaticMesh::
    // LightMapResolution and FMeshBuildSettings::MinLightmapResolution, or INDEX_NONE to leave
    // the asset default - which is 4 (StaticMesh.cpp:4738), small enough that leaving it there
    // wastes whatever lightmap UVs were authored. Independent of LightMapChannel: setting
    // either one alone is honoured, and requesting a channel WITHOUT a resolution is reported
    // as a warning rather than accepted silently.
    int32 LightMapResolution = INDEX_NONE;

    ECollisionTraceFlag CollisionTrace = ECollisionTraceFlag::CTF_UseDefault;

    // Simple-collision primitives written straight into the pre-build UBodySetup. Unset leaves
    // the asset's body setup empty.
    TOptional<FKAggregateGeom> SimpleCollision;

    // Provenance. Empty SourcePath means "not generated from a durable source" and no stamp is
    // written - an empty stamp would match every other empty stamp and defeat the overwrite rule.
    FString SourcePath;
    FString SourceHash;

    bool bRecomputeNormals = false;
    bool bRecomputeTangents = false;

    // Write the .uasset to disk. False leaves the package dirty-in-memory for a later flush.
    bool bSave = true;
};

struct FStaticMeshCreateResult
{
    bool bSuccess = false;

    // An ErrorCodes.h ERR_* value; empty on success.
    FString ErrorCode;
    FString ErrorMessage;

    // Non-fatal notes: a clamped lightmap index, a UV channel that had to be seeded, a slot
    // whose bound material would not load.
    TArray<FString> Warnings;
    TArray<FPwDiagnostic> Diagnostics;

    // Slot names that ended up on the default surface material.
    TArray<FString> UnboundSlots;

    UStaticMesh* Asset = nullptr;
    FString PackageName;

    // The asset already existed and was rebuilt in place rather than created fresh, so its
    // referencers still resolve to it - the SAME UStaticMesh object, at the same address.
    // True for every occupied path this call writes, bOverwrite or not; false only for a
    // create onto an empty path. It stopped being a proxy for "no flag was passed" when
    // bOverwrite became permission rather than mechanism.
    bool bUpdatedInPlace = false;

    // The ASSET's LOD0 counts, read off the built render data - NOT the counts of the mesh
    // that was passed in. The build changes both: it drops degenerate triangles
    // (FMeshBuildSettings::bRemoveDegenerates, default true -> StaticMeshBuilder.cpp:1666-1674)
    // and splits vertices at every normal/tangent/UV/color seam. A caller that wants the
    // source mesh's numbers already has the mesh.
    int32 TriangleCount = 0;
    int32 VertexCount = 0;
    int32 CollisionElements = 0;

    bool bSavedToDisk = false;

    // WHY it is not saved, when it is not. bSavedToDisk answers only "is it durable" and
    // therefore answers false identically for a deferred edit, a failed write and a package
    // that can never be written - three situations with three different remedies. Carried all
    // the way to the wire (`saveState` / `saveDetail`) so a caller never has to guess which
    // one it got. NotRequested when Spec.bSave is false.
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    // A save was requested but no .uasset reached disk - an editor.save_all is still owed.
    bool bPendingFlush = false;

    int64 SizeBytes = 0;
};

// Create (or rebuild) the StaticMesh asset described by Spec from Mesh, with exactly one build.
//
// Opens no dialog on any branch, spawns no actor, and reads no FHandlerContext: the RPC wrapper
// and the .pwmodel compiler both call this the same way. Mesh is copied before the create call,
// so no pointer into it is retained.
//
// Mesh IS mutated in one case: a mesh with no usable UV channel 0 has one box-projected onto it,
// because the render build's MikkT pass indexes [0] into a size-0 UV array and hard-crashes the
// async build worker. That happens after every refusal this function can raise, so a call that
// writes no asset leaves the caller's mesh untouched.
FStaticMeshCreateResult CreateStaticMesh(UDynamicMesh* Mesh, const FStaticMeshCreateSpec& Spec);
