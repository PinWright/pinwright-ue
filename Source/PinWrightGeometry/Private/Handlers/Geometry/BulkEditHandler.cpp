// Copyright (c) 2026 Alexander Penkin. MIT License.

// BulkEditHandler.cpp - Bulk mesh geometry upload (append_buffers)
//
// What is left here is the JSON layer: turning wire arrays into typed buffers, and rejecting
// shapes that are not [x,y,z] / [i,j,k] / [u,v] / [r,g,b,a]. The COUNT rules - a non-empty
// vertex array, triangle indices in range, an optional attribute array matching the vertex
// count - live in GeometryOps_Advanced.h, and are applied here at the exact points this parse
// has always applied them so a multi-fault payload still reports the same fault first.
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Geometry/GeometryOpWarnings.h"
#include "Handlers/Geometry/GeometryOps_Advanced.h"
#include "Handlers/Geometry/GeometryTarget.h"
#include "DynamicMeshActor.h"
#include "Handlers/Geometry/GeometryUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// File-local JSON array parsers for the append_buffers payload. Uniquely named so a Unity
// build merging the geometry handlers' anonymous namespaces cannot hit an ODR redefinition.
namespace
{
    // Numeric value of a JSON element (0 when absent / non-numeric).
    double JsonNum(const TSharedPtr<FJsonValue>& V)
    {
        double D = 0.0;
        if (V.IsValid())
        {
            V->TryGetNumber(D);
        }
        return D;
    }

    // Parse a 3-component vector from either array [x,y,z] or object {x,y,z}.
    bool ParseVec3(const TSharedPtr<FJsonValue>& Val, FVector& Out)
    {
        if (!Val.IsValid())
        {
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Val->TryGetArray(Arr) && Arr && Arr->Num() >= 3)
        {
            Out = FVector(JsonNum((*Arr)[0]), JsonNum((*Arr)[1]), JsonNum((*Arr)[2]));
            return true;
        }
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (Val->TryGetObject(Obj) && Obj && (*Obj).IsValid())
        {
            double X = 0.0, Y = 0.0, Z = 0.0;
            (*Obj)->TryGetNumberField(TEXT("x"), X);
            (*Obj)->TryGetNumberField(TEXT("y"), Y);
            (*Obj)->TryGetNumberField(TEXT("z"), Z);
            Out = FVector(X, Y, Z);
            return true;
        }
        return false;
    }

    // Parse a UV from either array [u,v] or object {u,v}.
    bool ParseVec2(const TSharedPtr<FJsonValue>& Val, FVector2D& Out)
    {
        if (!Val.IsValid())
        {
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Val->TryGetArray(Arr) && Arr && Arr->Num() >= 2)
        {
            Out = FVector2D(JsonNum((*Arr)[0]), JsonNum((*Arr)[1]));
            return true;
        }
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (Val->TryGetObject(Obj) && Obj && (*Obj).IsValid())
        {
            double U = 0.0, V = 0.0;
            (*Obj)->TryGetNumberField(TEXT("u"), U);
            (*Obj)->TryGetNumberField(TEXT("v"), V);
            Out = FVector2D(U, V);
            return true;
        }
        return false;
    }

    // Parse a color from either array [r,g,b,a] (alpha optional, default 1) or object {r,g,b,a}.
    bool ParseColor(const TSharedPtr<FJsonValue>& Val, FLinearColor& Out)
    {
        if (!Val.IsValid())
        {
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Val->TryGetArray(Arr) && Arr && Arr->Num() >= 3)
        {
            const float A = (Arr->Num() >= 4) ? static_cast<float>(JsonNum((*Arr)[3])) : 1.0f;
            Out = FLinearColor(static_cast<float>(JsonNum((*Arr)[0])), static_cast<float>(JsonNum((*Arr)[1])),
                               static_cast<float>(JsonNum((*Arr)[2])), A);
            return true;
        }
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (Val->TryGetObject(Obj) && Obj && (*Obj).IsValid())
        {
            double R = 0.0, G = 0.0, B = 0.0, A = 1.0;
            (*Obj)->TryGetNumberField(TEXT("r"), R);
            (*Obj)->TryGetNumberField(TEXT("g"), G);
            (*Obj)->TryGetNumberField(TEXT("b"), B);
            (*Obj)->TryGetNumberField(TEXT("a"), A);
            Out = FLinearColor(static_cast<float>(R), static_cast<float>(G), static_cast<float>(B), static_cast<float>(A));
            return true;
        }
        return false;
    }
}

// ============================================================================
// append_buffers
// ============================================================================
REGISTER_RPC_HANDLER("geometry.append_buffers", "geometry",
    "Bulk-append vertices + indexed triangles (with optional per-vertex normals/UVs/colors) to a DynamicMeshActor in one call, instead of per-element append_vertex/append_triangle round-trips",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the target DynamicMeshActor"),
        RPC_PARAM_REQ("vertices", "array", "Vertex positions in mesh-LOCAL space, each [x,y,z] or {x,y,z}"),
        RPC_PARAM_REQ("triangles", "array", "Triangles as index triples [i,j,k] into vertices (CCW winding faces outward)"),
        RPC_PARAM_OPT("normals", "array", "Per-vertex normals [x,y,z]; if present, count must equal vertices"),
        RPC_PARAM_OPT("uvs", "array", "Per-vertex UV0 coords [u,v]; if present, count must equal vertices"),
        RPC_PARAM_OPT("colors", "array", "Per-vertex colors [r,g,b,a] (alpha optional); if present, count must equal vertices"),
        RPC_PARAM_OPT("groupId", "integer", "Polygroup ID for all appended triangles (default 0)"),
        RPC_PARAM_OPT("materialId", "integer", "Material ID for all appended triangles (default 0)")
    ))
{
    const FString ActorName = Ctx.GetString(TEXT("actorName"));

    GeometryOps::FAppendBuffersParams Params;

    // Forward a rule violation from GeometryOps verbatim - code and message both.
    auto SendOpError = [&Ctx](const GeometryOps::FOpResult& OpResult)
    {
        Ctx.SendError(*OpResult.ErrorCode, OpResult.ErrorMessage);
    };

    // --- Vertices (required, non-empty) ---
    // A missing `vertices` field and a present-but-empty one are the same rejection, so both go
    // through the op's own rule rather than a second copy of its sentence living here. The
    // duplicate is what this replaces: two spellings of one wire message, either of which could
    // be reworded without the other moving.
    const TArray<TSharedPtr<FJsonValue>>* VerticesArr = Ctx.GetArray(TEXT("vertices"));
    const int32 SuppliedVertexCount = VerticesArr ? VerticesArr->Num() : 0;
    {
        const GeometryOps::FOpResult VertexCheck =
            GeometryOps::ValidateAppendBufferVertexCount(SuppliedVertexCount);
        if (!VertexCheck.bSuccess)
        {
            SendOpError(VertexCheck);
            return true;
        }
    }
    const int32 NumVerts = SuppliedVertexCount;

    Params.Vertices.Reserve(NumVerts);
    for (int32 i = 0; i < NumVerts; ++i)
    {
        FVector V;
        if (!ParseVec3((*VerticesArr)[i], V))
        {
            Ctx.SendError(TEXT("INVALID_PARAMS"),
                FString::Printf(TEXT("vertex %d is not [x,y,z] or {x,y,z}"), i));
            return true;
        }
        Params.Vertices.Add(V);
    }

    // --- Triangles (required; may be empty to append loose vertices) ---
    const TArray<TSharedPtr<FJsonValue>>* TrianglesArr = Ctx.GetArray(TEXT("triangles"));
    if (!TrianglesArr)
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("triangles must be an array of [i,j,k] index triples"));
        return true;
    }

    Params.Triangles.Reserve(TrianglesArr->Num());
    for (int32 t = 0; t < TrianglesArr->Num(); ++t)
    {
        const TArray<TSharedPtr<FJsonValue>>* Tri = nullptr;
        if (!(*TrianglesArr)[t]->TryGetArray(Tri) || !Tri || Tri->Num() < 3)
        {
            Ctx.SendError(TEXT("INVALID_PARAMS"),
                FString::Printf(TEXT("triangle %d must be an array of 3 vertex indices"), t));
            return true;
        }
        const FIntVector Triangle(
            FMath::RoundToInt(JsonNum((*Tri)[0])),
            FMath::RoundToInt(JsonNum((*Tri)[1])),
            FMath::RoundToInt(JsonNum((*Tri)[2])));

        const GeometryOps::FOpResult RangeCheck =
            GeometryOps::ValidateAppendBufferTriangle(t, Triangle, NumVerts);
        if (!RangeCheck.bSuccess)
        {
            SendOpError(RangeCheck);
            return true;
        }
        Params.Triangles.Add(Triangle);
    }

    // --- Optional per-vertex arrays: if present, each must match the vertex count ---
    // The count is checked BEFORE the elements are parsed, so a short array that is also
    // malformed still reports the count, which is the order this verb has always used.
    if (const TArray<TSharedPtr<FJsonValue>>* NormalsArr = Ctx.GetArray(TEXT("normals")))
    {
        const GeometryOps::FOpResult CountCheck =
            GeometryOps::ValidateAppendBufferAttributeCount(TEXT("normals"), NormalsArr->Num(), NumVerts);
        if (!CountCheck.bSuccess)
        {
            SendOpError(CountCheck);
            return true;
        }
        Params.Normals.Reserve(NumVerts);
        for (int32 i = 0; i < NumVerts; ++i)
        {
            FVector N;
            if (!ParseVec3((*NormalsArr)[i], N))
            {
                Ctx.SendError(TEXT("INVALID_PARAMS"),
                    FString::Printf(TEXT("normal %d is not [x,y,z] or {x,y,z}"), i));
                return true;
            }
            Params.Normals.Add(N);
        }
    }

    if (const TArray<TSharedPtr<FJsonValue>>* UVsArr = Ctx.GetArray(TEXT("uvs")))
    {
        const GeometryOps::FOpResult CountCheck =
            GeometryOps::ValidateAppendBufferAttributeCount(TEXT("uvs"), UVsArr->Num(), NumVerts);
        if (!CountCheck.bSuccess)
        {
            SendOpError(CountCheck);
            return true;
        }
        Params.UVs.Reserve(NumVerts);
        for (int32 i = 0; i < NumVerts; ++i)
        {
            FVector2D UV;
            if (!ParseVec2((*UVsArr)[i], UV))
            {
                Ctx.SendError(TEXT("INVALID_PARAMS"),
                    FString::Printf(TEXT("uv %d is not [u,v] or {u,v}"), i));
                return true;
            }
            Params.UVs.Add(UV);
        }
    }

    if (const TArray<TSharedPtr<FJsonValue>>* ColorsArr = Ctx.GetArray(TEXT("colors")))
    {
        const GeometryOps::FOpResult CountCheck =
            GeometryOps::ValidateAppendBufferAttributeCount(TEXT("colors"), ColorsArr->Num(), NumVerts);
        if (!CountCheck.bSuccess)
        {
            SendOpError(CountCheck);
            return true;
        }
        Params.Colors.Reserve(NumVerts);
        for (int32 i = 0; i < NumVerts; ++i)
        {
            FLinearColor C;
            if (!ParseColor((*ColorsArr)[i], C))
            {
                Ctx.SendError(TEXT("INVALID_PARAMS"),
                    FString::Printf(TEXT("color %d is not [r,g,b,a] or {r,g,b,a}"), i));
                return true;
            }
            Params.Colors.Add(C);
        }
    }

    Params.GroupId = Ctx.GetInt(TEXT("groupId"), 0);
    Params.MaterialId = Ctx.GetInt(TEXT("materialId"), 0);

    // --- Resolve the target mesh ---
    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
    {
        return true;
    }

    // Params is moved, not copied: this verb exists to avoid per-element round-trips and a copy
    // of the payload here would give most of that back.
    GeometryOps::FAppendBuffersOutputs Outputs;
    const GeometryOps::FOpResult Op =
        GeometryOps::AppendBuffers(Target.Mesh, MoveTemp(Params), Outputs);
    if (!Op.bSuccess)
    {
        SendOpError(Op);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("appendedVertices"), Outputs.AppendedVertices);
    Result->SetNumberField(TEXT("appendedTriangles"), Outputs.AppendedTriangles);
    Result->SetNumberField(TEXT("totalVertices"), Op.VerticesAfter);
    Result->SetNumberField(TEXT("totalTriangles"), Op.TrianglesAfter);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Buffers appended"), Result);
    return true;
}
