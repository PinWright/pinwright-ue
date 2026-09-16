// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Asset/StaticMeshDumpBuilder.h"
#include "Utils/MeshRenderConsumerScan.h"

#include "Components/ActorComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

REGISTER_RPC_HANDLER("static_mesh.describe", "static_mesh",
    "Return read-only StaticMesh metadata using the same JSON shape as static_mesh.json asset dumps: bounds, materials, per-LOD sections and UV counts, LOD0 slot usage, lightmap settings, and collision trace flag; plus rebuildRenderConsumers, the live components a rebuild of this mesh in place would have to quiesce first.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Static mesh asset path")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(TEXT("assetPath"), AssetPath)) return true;

    UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *AssetPath);
    if (!Mesh)
    {
        Ctx.SendError(TEXT("MESH_NOT_FOUND"),
            FString::Printf(TEXT("Could not load StaticMesh: %s"), *AssetPath));
        return true;
    }

    TSharedPtr<FJsonObject> Result = StaticMeshDumpBuilder::BuildStaticMeshJson(Mesh);

    // A model.compile onto an occupied path rebuilds the mesh IN PLACE, and the rebuild guard
    // refuses that with MESH_REBUILD_CONSUMER_NOT_QUIESCABLE when a live component's scene
    // proxy still caches the old render data afterwards. Without this field the refusal is the
    // first time a caller hears such a component exists. It is computed from the same scan the
    // guard acts on (Utils/MeshRenderConsumerScan.h), so the read and the refusal cannot
    // disagree about who is holding the mesh.
    //
    // UStaticMeshComponents are matched to this mesh, while Niagara components are matched by
    // their assigned system's enabled mesh-renderer properties. Dynamic Niagara mesh bindings
    // remain conservative when their runtime object is not available. Using the helper's exact
    // target-aware scan keeps this read from answering "safe" for a rebuild the guard refuses.
    TArray<UClass*> ScannedClasses;
    ScannedClasses.Add(UStaticMeshComponent::StaticClass());
    ScannedClasses.Append(PinWrightMeshRebuild::ResolveStaleRenderStateComponentClasses());

    // Reported separately from the components because an empty list means two different things:
    // no Niagara class is loaded in this editor (the StaticMeshComponent scan still exists),
    // versus a scanned class with no live instance (looked for, found none).
    TArray<TSharedPtr<FJsonValue>> ScannedClassValues;
    ScannedClassValues.Reserve(ScannedClasses.Num());
    for (const UClass* CandidateClass : ScannedClasses)
    {
        ScannedClassValues.Add(MakeShared<FJsonValueString>(CandidateClass->GetPathName()));
    }

    TArray<UStaticMesh*> TargetMeshes;
    TargetMeshes.Add(Mesh);
    const TArray<UActorComponent*> Consumers =
        PinWrightMeshRebuild::ScanForStaticMeshRebuildConsumers(TargetMeshes);

    TArray<TSharedPtr<FJsonValue>> ComponentValues;
    ComponentValues.Reserve(Consumers.Num());
    for (const UActorComponent* Component : Consumers)
    {
        ComponentValues.Add(MakeShared<FJsonValueString>(Component->GetPathName()));
    }

    TSharedPtr<FJsonObject> RenderConsumers = MakeShared<FJsonObject>();
    RenderConsumers->SetArrayField(TEXT("scannedClasses"), ScannedClassValues);
    RenderConsumers->SetArrayField(TEXT("components"), ComponentValues);
    Result->SetObjectField(TEXT("rebuildRenderConsumers"), RenderConsumers);

    Ctx.SendSuccess(Result);
    return true;
}
