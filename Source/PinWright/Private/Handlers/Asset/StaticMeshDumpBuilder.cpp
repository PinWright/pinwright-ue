// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/StaticMeshDumpBuilder.h"

#include "Compat/EngineVersionCompat.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshResources.h"
#include "Handlers/Asset/MeshBoundsHelpers.h"
#include "Handlers/Asset/StaticMeshTextEmitter.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Utils/JsonSidecarRegistry.h"

namespace
{
    using MeshBoundsHelpers::BuildBoundsJson;
}

TSharedPtr<FJsonObject> StaticMeshDumpBuilder::BuildStaticMeshJson(const UStaticMesh* Mesh)
{
    if (!Mesh)
    {
        return nullptr;
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    Root->SetObjectField(TEXT("bounds"), BuildBoundsJson(Mesh->GetExtendedBounds()));

    TArray<TSharedPtr<FJsonValue>> MaterialsArr;
    const TArray<FStaticMaterial>& StaticMaterials = Mesh->GetStaticMaterials();
    for (const FStaticMaterial& Mat : StaticMaterials)
    {
        TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("slot"), Mat.MaterialSlotName.ToString());
        Entry->SetStringField(TEXT("path"), Mat.MaterialInterface ? Mat.MaterialInterface->GetPathName() : FString());
        MaterialsArr.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Root->SetArrayField(TEXT("materials"), MaterialsArr);

    const int32 NumLODs = Mesh->GetNumLODs();
    Root->SetNumberField(TEXT("lods"), NumLODs);

    TArray<TSharedPtr<FJsonValue>> TrianglesByLod;
    TArray<TSharedPtr<FJsonValue>> VerticesByLod;
    for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
    {
        TrianglesByLod.Add(MakeShared<FJsonValueNumber>(Mesh->GetNumTriangles(LODIndex)));
        VerticesByLod.Add(MakeShared<FJsonValueNumber>(Mesh->GetNumVertices(LODIndex)));
    }
    Root->SetArrayField(TEXT("trianglesByLod"), TrianglesByLod);
    Root->SetArrayField(TEXT("verticesByLod"), VerticesByLod);

    TArray<TSharedPtr<FJsonValue>> Sections;
    TArray<TSharedPtr<FJsonValue>> UvChannelsByLod;
    TArray<uint64> Lod0TrianglesByMaterial;
    Lod0TrianglesByMaterial.SetNumZeroed(StaticMaterials.Num());
    uint64 Lod0TriangleCount = 0;
    if (const FStaticMeshRenderData* RenderData = Mesh->GetRenderData())
    {
        for (int32 LODIndex = 0; LODIndex < RenderData->LODResources.Num(); ++LODIndex)
        {
            const FStaticMeshLODResources& LODResources = RenderData->LODResources[LODIndex];
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
            const int32 NumUVChannels = Mesh->GetNumUVChannels(LODIndex);
#else
            // UStaticMesh::GetNumUVChannels became const in UE 5.6. On older engines it is the
            // same read-only lookup into RenderData->LODResources, just not marked const.
            const int32 NumUVChannels =
                const_cast<UStaticMesh*>(Mesh)->GetNumUVChannels(LODIndex);
#endif
            UvChannelsByLod.Add(MakeShared<FJsonValueNumber>(NumUVChannels));
            for (int32 SectionIndex = 0; SectionIndex < LODResources.Sections.Num(); ++SectionIndex)
            {
                const FStaticMeshSection& Section = LODResources.Sections[SectionIndex];
                TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetNumberField(TEXT("lodIndex"), LODIndex);
                Entry->SetNumberField(TEXT("index"), SectionIndex);
                Entry->SetNumberField(TEXT("materialIndex"), Section.MaterialIndex);
                Entry->SetStringField(TEXT("materialSlotName"),
                    StaticMaterials.IsValidIndex(Section.MaterialIndex)
                        ? StaticMaterials[Section.MaterialIndex].MaterialSlotName.ToString()
                        : FString());
                Entry->SetNumberField(TEXT("firstIndex"), Section.FirstIndex);
                Entry->SetNumberField(TEXT("numTriangles"), Section.NumTriangles);
                Entry->SetNumberField(TEXT("minVertexIndex"), Section.MinVertexIndex);
                Entry->SetNumberField(TEXT("maxVertexIndex"), Section.MaxVertexIndex);
                Entry->SetBoolField(TEXT("bEnableCollision"), Section.bEnableCollision);
                Entry->SetBoolField(TEXT("bCastShadow"), Section.bCastShadow);
                Sections.Add(MakeShared<FJsonValueObject>(Entry));

                if (LODIndex == 0)
                {
                    Lod0TriangleCount += Section.NumTriangles;
                    if (Lod0TrianglesByMaterial.IsValidIndex(Section.MaterialIndex))
                    {
                        Lod0TrianglesByMaterial[Section.MaterialIndex] += Section.NumTriangles;
                    }
                }
            }
        }
    }
    Root->SetArrayField(TEXT("sections"), Sections);
    Root->SetArrayField(TEXT("uvChannelsByLod"), UvChannelsByLod);

    TArray<TSharedPtr<FJsonValue>> SlotUsage;
    for (int32 MaterialIndex = 0; MaterialIndex < StaticMaterials.Num(); ++MaterialIndex)
    {
        TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetNumberField(TEXT("materialIndex"), MaterialIndex);
        Entry->SetStringField(TEXT("materialSlotName"), StaticMaterials[MaterialIndex].MaterialSlotName.ToString());
        Entry->SetNumberField(TEXT("lod0TriangleCount"), static_cast<double>(Lod0TrianglesByMaterial[MaterialIndex]));
        Entry->SetNumberField(TEXT("lod0TriangleFraction"), Lod0TriangleCount > 0
            ? static_cast<double>(Lod0TrianglesByMaterial[MaterialIndex]) / static_cast<double>(Lod0TriangleCount)
            : 0.0);
        SlotUsage.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Root->SetArrayField(TEXT("slotUsage"), SlotUsage);

    Root->SetNumberField(TEXT("lightmapResolution"), Mesh->GetLightMapResolution());
    Root->SetNumberField(TEXT("lightMapCoordinateIndex"), Mesh->GetLightMapCoordinateIndex());

    FString CollisionTrace;
    if (const UBodySetup* BodySetup = Mesh->GetBodySetup())
    {
        const UEnum* TraceFlagEnum = StaticEnum<ECollisionTraceFlag>();
        if (TraceFlagEnum)
        {
            CollisionTrace = TraceFlagEnum->GetNameStringByValue(static_cast<int64>(BodySetup->CollisionTraceFlag));
        }

        TSharedRef<FJsonObject> Collision = MakeShared<FJsonObject>();
        TSharedRef<FJsonObject> Elements = MakeShared<FJsonObject>();
        const FKAggregateGeom& AggGeom = BodySetup->AggGeom;
        Elements->SetNumberField(TEXT("sphere"), AggGeom.SphereElems.Num());
        Elements->SetNumberField(TEXT("box"), AggGeom.BoxElems.Num());
        Elements->SetNumberField(TEXT("sphyl"), AggGeom.SphylElems.Num());
        Elements->SetNumberField(TEXT("convex"), AggGeom.ConvexElems.Num());
        Elements->SetNumberField(TEXT("taperedCapsule"), AggGeom.TaperedCapsuleElems.Num());
        Collision->SetObjectField(TEXT("elements"), Elements);
        Root->SetObjectField(TEXT("collision"), Collision);
    }
    Root->SetStringField(TEXT("collisionTraceFlag"), CollisionTrace);

    return Root;
}

namespace
{
    UClass* GetStaticMeshSidecarClass()
    {
        return UStaticMesh::StaticClass();
    }

    TSharedPtr<FJsonObject> BuildStaticMeshSidecar(UObject* Asset)
    {
        return StaticMeshDumpBuilder::BuildStaticMeshJson(Cast<UStaticMesh>(Asset));
    }
}

REGISTER_DUMP_JSON_SIDECAR(TEXT("static_mesh"), DumpFileNames::StaticMesh,
    &GetStaticMeshSidecarClass, &BuildStaticMeshSidecar,
    DumpFileNames::StaticMeshTxt, &StaticMeshTextEmitter::BuildText,
    nullptr, 100);
