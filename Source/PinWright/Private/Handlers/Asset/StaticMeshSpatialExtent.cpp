// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/StaticMeshSpatialExtent.h"

#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"

namespace StaticMeshSpatialExtentHelpers
{
    TSharedPtr<FJsonObject> VectorJson(const FVector& V)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetNumberField(TEXT("x"), V.X);
        Out->SetNumberField(TEXT("y"), V.Y);
        Out->SetNumberField(TEXT("z"), V.Z);
        return Out;
    }

    // The CPU copies are kept in the editor (TResourceArray::Discard only empties them on
    // cooked-data platforms), but a section range past the resident data must not be read.
    bool HasCpuGeometry(const FStaticMeshLODResources& LOD, const FIndexArrayView& Indices)
    {
        const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
        return Indices.Num() > 0 && Positions.GetNumVertices() > 0 && Positions.GetVertexData() != nullptr;
    }

    bool SectionRangeResident(const FStaticMeshSection& Section, const FIndexArrayView& Indices)
    {
        return static_cast<uint64>(Section.FirstIndex) + 3ull * Section.NumTriangles
            <= static_cast<uint64>(Indices.Num());
    }
}

FBox StaticMeshSpatialExtent::ComputeSectionBox(const FStaticMeshLODResources& LOD, const FStaticMeshSection& Section)
{
    FBox Box(ForceInit);
    const FIndexArrayView Indices = LOD.IndexBuffer.GetArrayView();
    if (Section.NumTriangles == 0
        || !StaticMeshSpatialExtentHelpers::HasCpuGeometry(LOD, Indices)
        || !StaticMeshSpatialExtentHelpers::SectionRangeResident(Section, Indices))
    {
        return Box;
    }

    const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
    const uint32 NumVertices = Positions.GetNumVertices();
    const uint32 End = Section.FirstIndex + 3 * Section.NumTriangles;
    for (uint32 I = Section.FirstIndex; I < End; ++I)
    {
        const uint32 Vertex = Indices[I];
        if (Vertex < NumVertices)
        {
            Box += FVector(Positions.VertexPosition(Vertex));
        }
    }
    return Box;
}

TSharedPtr<FJsonValue> StaticMeshSpatialExtent::BoxToJsonValue(const FBox& Box)
{
    if (!Box.IsValid)
    {
        return MakeShared<FJsonValueNull>();
    }
    using StaticMeshSpatialExtentHelpers::VectorJson;
    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetObjectField(TEXT("min"), VectorJson(Box.Min));
    Out->SetObjectField(TEXT("max"), VectorJson(Box.Max));
    Out->SetObjectField(TEXT("size"), VectorJson(Box.GetSize()));
    Out->SetObjectField(TEXT("center"), VectorJson(Box.GetCenter()));
    return MakeShared<FJsonValueObject>(Out);
}

bool StaticMeshSpatialExtent::BuildLod0Islands(const UStaticMesh* Mesh, int32 MaxRows, TArray<TSharedPtr<FJsonValue>>& OutIslands,
    int32& OutIslandCount, FString& OutUnavailableReason)
{
    OutIslands.Reset();
    OutIslandCount = 0;
    const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
    if (!RenderData || RenderData->LODResources.Num() == 0)
    {
        return true;
    }

    const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
    const FIndexArrayView Indices = LOD.IndexBuffer.GetArrayView();
    if (!StaticMeshSpatialExtentHelpers::HasCpuGeometry(LOD, Indices))
    {
        if (LOD.Sections.Num() == 0)
        {
            return true;
        }
        OutUnavailableReason = TEXT("LOD0 CPU index/position data is not resident");
        return false;
    }

    // Weld render vertices by exact position. -0.0 is folded into +0.0 per component: they compare
    // equal but hash differently. Explicit compare, not `+ 0.0f`, which fast-math may drop.
    const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
    const uint32 NumVertices = Positions.GetNumVertices();
    TArray<int32> WeldOf;
    WeldOf.SetNumUninitialized(NumVertices);
    TMap<FVector3f, int32> WeldByPosition;
    WeldByPosition.Reserve(NumVertices);
    for (uint32 V = 0; V < NumVertices; ++V)
    {
        const FVector3f& Raw = Positions.VertexPosition(V);
        const FVector3f P(Raw.X == 0.0f ? 0.0f : Raw.X, Raw.Y == 0.0f ? 0.0f : Raw.Y, Raw.Z == 0.0f ? 0.0f : Raw.Z);
        const int32 NextId = WeldByPosition.Num();
        WeldOf[V] = WeldByPosition.FindOrAdd(P, NextId);
    }

    TArray<int32> Parent;
    Parent.SetNumUninitialized(WeldByPosition.Num());
    for (int32 I = 0; I < Parent.Num(); ++I)
    {
        Parent[I] = I;
    }
    const auto FindRoot = [&Parent](int32 X)
    {
        while (Parent[X] != X)
        {
            Parent[X] = Parent[Parent[X]];
            X = Parent[X];
        }
        return X;
    };

    const auto ForEachTriangle = [&](TFunctionRef<void(const FStaticMeshSection&, const uint32*)> Visit)
    {
        for (const FStaticMeshSection& Section : LOD.Sections)
        {
            if (!StaticMeshSpatialExtentHelpers::SectionRangeResident(Section, Indices))
            {
                continue;
            }
            for (uint32 T = 0; T < Section.NumTriangles; ++T)
            {
                const uint32 Base = Section.FirstIndex + 3 * T;
                const uint32 Corners[3] = { Indices[Base], Indices[Base + 1], Indices[Base + 2] };
                if (Corners[0] < NumVertices && Corners[1] < NumVertices && Corners[2] < NumVertices)
                {
                    Visit(Section, Corners);
                }
            }
        }
    };

    ForEachTriangle([&](const FStaticMeshSection&, const uint32* Corners)
    {
        const int32 A = FindRoot(WeldOf[Corners[0]]);
        for (int32 C = 1; C < 3; ++C)
        {
            const int32 B = FindRoot(WeldOf[Corners[C]]);
            if (A != B)
            {
                Parent[B] = A;
            }
        }
    });

    struct FIsland
    {
        int32 TriangleCount = 0;
        FBox Box = FBox(ForceInit);
        TSet<int32> MaterialIndices;
    };
    TMap<int32, FIsland> IslandsByRoot;
    ForEachTriangle([&](const FStaticMeshSection& Section, const uint32* Corners)
    {
        FIsland& Island = IslandsByRoot.FindOrAdd(FindRoot(WeldOf[Corners[0]]));
        ++Island.TriangleCount;
        for (int32 C = 0; C < 3; ++C)
        {
            Island.Box += FVector(Positions.VertexPosition(Corners[C]));
        }
        Island.MaterialIndices.Add(Section.MaterialIndex);
    });

    TArray<FIsland> Islands;
    IslandsByRoot.GenerateValueArray(Islands);
    Islands.Sort([](const FIsland& L, const FIsland& R)
    {
        if (L.TriangleCount != R.TriangleCount)
        {
            return L.TriangleCount > R.TriangleCount;
        }
        if (L.Box.Min.X != R.Box.Min.X) return L.Box.Min.X < R.Box.Min.X;
        if (L.Box.Min.Y != R.Box.Min.Y) return L.Box.Min.Y < R.Box.Min.Y;
        return L.Box.Min.Z < R.Box.Min.Z;
    });

    const TArray<FStaticMaterial>& StaticMaterials = Mesh->GetStaticMaterials();
    OutIslandCount = Islands.Num();
    const int32 RowCount = FMath::Min(Islands.Num(), FMath::Max(MaxRows, 0));
    for (int32 IslandIndex = 0; IslandIndex < RowCount; ++IslandIndex)
    {
        FIsland& Island = Islands[IslandIndex];
        Island.MaterialIndices.Sort([](int32 L, int32 R) { return L < R; });
        TArray<TSharedPtr<FJsonValue>> Slots;
        for (const int32 MaterialIndex : Island.MaterialIndices)
        {
            TSharedRef<FJsonObject> Slot = MakeShared<FJsonObject>();
            Slot->SetNumberField(TEXT("materialIndex"), MaterialIndex);
            Slot->SetStringField(TEXT("materialSlotName"), StaticMaterials.IsValidIndex(MaterialIndex)
                ? StaticMaterials[MaterialIndex].MaterialSlotName.ToString() : FString());
            Slots.Add(MakeShared<FJsonValueObject>(Slot));
        }

        TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetNumberField(TEXT("index"), IslandIndex);
        Row->SetNumberField(TEXT("triangleCount"), Island.TriangleCount);
        Row->SetField(TEXT("boundingBox"), BoxToJsonValue(Island.Box));
        Row->SetArrayField(TEXT("materialSlots"), Slots);
        OutIslands.Add(MakeShared<FJsonValueObject>(Row));
    }
    return true;
}
