// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/SkeletalMeshDumpBuilder.h"

#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Materials/MaterialInterface.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Animation/Skeleton.h"
#include "Handlers/Asset/MeshBoundsHelpers.h"
#include "Handlers/Asset/SkeletonDumpBuilder.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Utils/JsonSidecarRegistry.h"

namespace
{
    using MeshBoundsHelpers::BuildBoundsJson;
}

TSharedPtr<FJsonObject> SkeletalMeshDumpBuilder::BuildSkeletalMeshJson(const USkeletalMesh* Mesh)
{
    if (!Mesh)
    {
        return nullptr;
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    Root->SetObjectField(TEXT("bounds"), BuildBoundsJson(Mesh->GetImportedBounds()));

    TArray<TSharedPtr<FJsonValue>> MaterialsArr;
    const TArray<FSkeletalMaterial>& SkelMaterials = Mesh->GetMaterials();
    for (const FSkeletalMaterial& Mat : SkelMaterials)
    {
        TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("slot"), Mat.MaterialSlotName.ToString());
        Entry->SetStringField(TEXT("path"), Mat.MaterialInterface ? Mat.MaterialInterface->GetPathName() : FString());
        Entry->SetStringField(TEXT("importedSlot"), Mat.ImportedMaterialSlotName.ToString());
        MaterialsArr.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Root->SetArrayField(TEXT("materials"), MaterialsArr);

    const int32 NumLODs = Mesh->GetLODNum();
    Root->SetNumberField(TEXT("lods"), NumLODs);

    TArray<TSharedPtr<FJsonValue>> TrianglesByLod;
    TArray<TSharedPtr<FJsonValue>> VerticesByLod;
    TArray<TSharedPtr<FJsonValue>> SectionsByLod;
    TArray<TSharedPtr<FJsonValue>> NumTexCoordsByLod;

    if (const FSkeletalMeshModel* ImportedModel = Mesh->GetImportedModel())
    {
        for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
        {
            if (!ImportedModel->LODModels.IsValidIndex(LODIndex))
            {
                continue;
            }
            const FSkeletalMeshLODModel& LODModel = ImportedModel->LODModels[LODIndex];
            int32 NumTriangles = 0;
            int32 NumVertices = 0;
            for (const FSkelMeshSection& Section : LODModel.Sections)
            {
                NumTriangles += Section.NumTriangles;
                NumVertices += Section.NumVertices;
            }
            TrianglesByLod.Add(MakeShared<FJsonValueNumber>(NumTriangles));
            VerticesByLod.Add(MakeShared<FJsonValueNumber>(NumVertices));
            SectionsByLod.Add(MakeShared<FJsonValueNumber>(LODModel.Sections.Num()));
            NumTexCoordsByLod.Add(MakeShared<FJsonValueNumber>(LODModel.NumTexCoords));
        }
    }

    Root->SetArrayField(TEXT("trianglesByLod"), TrianglesByLod);
    Root->SetArrayField(TEXT("verticesByLod"), VerticesByLod);
    Root->SetArrayField(TEXT("sectionsByLod"), SectionsByLod);
    Root->SetArrayField(TEXT("numTexCoordsByLod"), NumTexCoordsByLod);

    const UPhysicsAsset* PhysicsAsset = Mesh->GetPhysicsAsset();
    Root->SetStringField(TEXT("physicsAsset"), PhysicsAsset ? PhysicsAsset->GetPathName() : FString());

    const USkeleton* Skeleton = Mesh->GetSkeleton();
    Root->SetStringField(TEXT("skeleton"), Skeleton ? Skeleton->GetPathName() : FString());
    const TArray<USkeletalMeshSocket*> ActiveSockets = Mesh->GetActiveSocketList();
    Root->SetArrayField(TEXT("sockets"), SkeletonDumpBuilder::BuildSocketsJson(ActiveSockets));
    Root->SetArrayField(TEXT("virtualBones"), Skeleton ? SkeletonDumpBuilder::BuildVirtualBonesJson(Skeleton->GetVirtualBones()) : TArray<TSharedPtr<FJsonValue>>());

    return Root;
}

namespace
{
    UClass* GetSkeletalMeshSidecarClass()
    {
        return USkeletalMesh::StaticClass();
    }

    TSharedPtr<FJsonObject> BuildSkeletalMeshSidecar(UObject* Asset)
    {
        return SkeletalMeshDumpBuilder::BuildSkeletalMeshJson(Cast<USkeletalMesh>(Asset));
    }
}

REGISTER_DUMP_JSON_SIDECAR(TEXT("skeletal_mesh"), DumpFileNames::SkeletalMesh,
    &GetSkeletalMeshSidecarClass, &BuildSkeletalMeshSidecar,
    nullptr, nullptr, nullptr, 100);
