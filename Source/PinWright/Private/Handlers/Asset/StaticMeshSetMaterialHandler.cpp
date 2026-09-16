// Copyright (c) 2026 Alexander Penkin. MIT License.

// StaticMeshSetMaterialHandler.cpp - Assign a material to a StaticMesh asset slot (F-geometry-mesh-material-assign).
//
// The in-editor modeling -> bake flow (geometry.create_box -> ... ->
// geometry.convert_to_static_mesh) produced a /Game StaticMesh whose material
// slots were never populated: CreateNewStaticMeshAssetFromMesh bakes geometry
// only, so the saved asset a user drags into levels always came up on the default
// material. No verb assigned a material to a StaticMesh asset slot — only the LIVE
// component was reachable (actor.set_component_properties {OverrideMaterials:[...]}),
// which does not carry into the baked asset. This verb closes that gap: it binds a
// material to a StaticMesh's StaticMaterials slot by index and persists the asset,
// mirroring the established <ns>.set_material convention (spline.set_spline_mesh_material,
// landscape.set_material, water.set_water_body_material).
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"

#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Utils/AssetUtils.h"
#include "Utils/PathUtils.h"
#include "Dom/JsonObject.h"

REGISTER_RPC_HANDLER("static_mesh.set_material", "static_mesh",
    "Assign a material asset to a StaticMesh material slot by index and save the asset to disk. Populates the baked/imported StaticMesh's StaticMaterials slot so the saved asset a user drags into levels carries the material (e.g. after geometry.convert_to_static_mesh, which bakes geometry only). Errors on an out-of-range slot rather than silently no-op'ing.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "StaticMesh asset path"),
        RPC_PARAM_REQ("materialPath", "path", "Path to the material asset (UMaterialInterface) to assign to the slot"),
        RPC_PARAM_OPT("materialIndex", "integer", "Material slot index (default 0)"),
        RPC_PARAM_OPT("save", "boolean", "Persist the modified StaticMesh to disk (default true)")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(TEXT("assetPath"), AssetPath)) return true;

    FString MaterialPath;
    if (!Ctx.RequireString(TEXT("materialPath"), MaterialPath)) return true;

    const int32 MaterialIndex = Ctx.GetInt(TEXT("materialIndex"), 0);
    const bool bSave = Ctx.GetBool(TEXT("save"), true);

    // SECURITY: constrain the material to a project-relative asset root, matching
    // spline.set_spline_mesh_material / landscape.set_material.
    const FString SafeMaterialPath = SanitizeProjectRelativePath(MaterialPath);
    if (SafeMaterialPath.IsEmpty())
    {
        Ctx.SendError(TEXT("SECURITY_VIOLATION"),
            FString::Printf(TEXT("Invalid or unsafe materialPath: %s. Path must be relative to project (e.g. /Game/...)"), *MaterialPath));
        return true;
    }

    UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *AssetPath);
    if (!Mesh)
    {
        Ctx.SendError(TEXT("MESH_NOT_FOUND"),
            FString::Printf(TEXT("Could not load StaticMesh: %s"), *AssetPath));
        return true;
    }

    UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *SafeMaterialPath);
    if (!Material)
    {
        Ctx.SendError(TEXT("MATERIAL_NOT_FOUND"),
            FString::Printf(TEXT("Material not found: %s"), *SafeMaterialPath));
        return true;
    }

    // UStaticMesh::SetMaterial silently no-ops when the index is out of range (it
    // guards on GetStaticMaterials().IsValidIndex). Reject explicitly so an
    // out-of-range slot is a clear error, never a fake success on an unchanged asset.
    // Bind the slot array once (GetStaticMaterials() runs an async-property wait per
    // call): SetMaterial assigns into the existing element in place with no resize,
    // so this reference stays valid across the assignment and the post-assign read.
    const TArray<FStaticMaterial>& Slots = Mesh->GetStaticMaterials();
    const int32 SlotCount = Slots.Num();
    if (!Slots.IsValidIndex(MaterialIndex))
    {
        Ctx.SendError(TEXT("INVALID_MATERIAL_INDEX"),
            FString::Printf(TEXT("materialIndex %d out of range; StaticMesh has %d material slot(s)"),
                MaterialIndex, SlotCount));
        return true;
    }

    // Bind the material into the asset's slot (PreEditChange/PostEditChange +
    // transaction + default slot naming are handled by the engine helper).
    Mesh->SetMaterial(MaterialIndex, Material);

    // Persist so the SAVED asset carries the material — the whole point of the
    // ticket (the user drags the on-disk asset into levels). Report persistence
    // honestly via the shared save-report contract, mirroring convert_to_static_mesh.
    FString PackageName;
    int64 SizeBytes = 0;
    bool bSavedToDisk = false;
    // Threaded so the response carries saveState/saveDetail like every other save in the
    // family: without it a PIE-blocked write answered with a bare pendingFlush the caller was
    // documented to retry (B-asset-save-omits-savestate-pie-block).
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    if (bSave)
    {
        bSavedToDisk = SaveAssetToDiskReportingPresence(Mesh, /*bForce=*/true, &PackageName, &SizeBytes,
            &SaveState);
    }

    // Echo the resulting slot binding read straight off the asset (ground truth,
    // not the input echo) so the assignment is verifiable in one call.
    const FStaticMaterial& Slot = Slots[MaterialIndex];

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetNumberField(TEXT("materialIndex"), MaterialIndex);
    Result->SetNumberField(TEXT("materialSlots"), SlotCount);
    Result->SetStringField(TEXT("slot"), Slot.MaterialSlotName.ToString());
    Result->SetStringField(TEXT("materialPath"),
        Slot.MaterialInterface ? Slot.MaterialInterface->GetPathName() : FString());
    Result->SetStringField(TEXT("package"), PackageName);
    AddAssetSaveSizeReport(Result, SizeBytes, bSavedToDisk);
    AddAssetSaveReport(Result, /*bSaveRequested=*/bSave, bSavedToDisk, SaveState);
    Ctx.SendSuccess(TEXT("Material assigned to StaticMesh slot"), Result);
    return true;
}
