// Copyright (c) 2026 Alexander Penkin. MIT License.

// MeshInfoHandler.cpp - Mesh info, vertex/triangle ops, UV ops (Phase 18)
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Geometry/GeometryOpWarnings.h"
#include "Handlers/Geometry/GeometryOps_Elements.h"
#include "Handlers/Geometry/GeometryOps_Modeling.h"
#include "Handlers/Geometry/GeometryTarget.h"
#include "DynamicMeshActor.h"
#include "Handlers/Geometry/GeometryUtils.h"
#include "Utils/JsonBuilders.h"
#include "Dom/JsonObject.h"


// No EngineUtils.h: the label scan that needed TActorIterator moved into GeometryTarget, so this
// file no longer names it. It was left behind as a dead include; under bUseUnity a dead include is
// not free, since it is what lets a LATER file in the same blob compile without declaring its own
// dependency. DynamicMeshActor.h is declared above because this file still hands the resolved
// ADynamicMeshActor* to AActor* parameters, which needs the complete type.
#include "Components/DynamicMeshComponent.h"
#include "UDynamicMesh.h"
#include "Editor.h"

#if __has_include("GeometryScript/GeometryScriptTypes.h")
#include "GeometryScript/GeometryScriptTypes.h"
#else
#include "GeometryScriptTypes.h"
#endif

#include "GeometryScript/MeshQueryFunctions.h"


// ============================================================================
// get_mesh_info
// ============================================================================
REGISTER_RPC_HANDLER("geometry.get_mesh_info", "geometry", "Get information about a dynamic mesh: vertex/triangle count, feature flags, and the local-space boundingBox {min,max,origin,extent}",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    int32 VertexCount = UGeometryScriptLibrary_MeshQueryFunctions::GetVertexCount(Target.Mesh);
    int32 TriangleCount = Target.Mesh->GetTriangleCount();
    bool bHasNormals = UGeometryScriptLibrary_MeshQueryFunctions::GetHasTriangleNormals(Target.Mesh);
    int32 NumUVSets = UGeometryScriptLibrary_MeshQueryFunctions::GetNumUVSets(Target.Mesh);
    bool bHasVertexColors = UGeometryScriptLibrary_MeshQueryFunctions::GetHasVertexColors(Target.Mesh);
    bool bHasMaterialIDs = UGeometryScriptLibrary_MeshQueryFunctions::GetHasMaterialIDs(Target.Mesh);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("vertexCount"), VertexCount);
    Result->SetNumberField(TEXT("triangleCount"), TriangleCount);
    Result->SetBoolField(TEXT("hasNormals"), bHasNormals);
    Result->SetBoolField(TEXT("hasUVs"), NumUVSets > 0);
    Result->SetBoolField(TEXT("hasColors"), bHasVertexColors);
    Result->SetBoolField(TEXT("hasPolygroups"), bHasMaterialIDs);

    // Local-space bounding box of the dynamic mesh geometry. Read off the same
    // Target.Mesh handle the counts above use, so a single get_mesh_info call answers
    // "did the op land at the expected size/extent?" without a separate
    // actor.get_bounding_box round-trip (and unlike that verb, this is the mesh-LOCAL
    // extent — the two diverge once the actor carries a non-identity transform).
    // Sibling geometry ops (spherify/cylindrify, AdvancedMeshOps) already read this FBox.
    const FBox MeshBounds = UGeometryScriptLibrary_MeshQueryFunctions::GetMeshBoundingBox(Target.Mesh);

    // An empty (0-vertex) mesh -- a fresh create_procedural_mesh, or a mesh emptied
    // by a boolean/delete/simplify -- yields the inverted "empty" FBox (Min=+DBL_MAX,
    // Max=-DBL_MAX). GetExtent()=(Max-Min)*0.5 then overflows to a non-finite value
    // (-inf), which serializes to a bare `inf`/`nan` token: invalid JSON the client
    // rejects wholesale, mis-reporting a live editor as unreachable. Report a zeroed
    // box for the empty/degenerate case instead of the inverted sentinel (min/max
    // would otherwise be nonsensical +/-DBL_MAX). BuildVectorJson also finite-guards
    // each component as defense-in-depth for any other degenerate-bounds path.
    // Select the box once, then derive: FBox(Zero,Zero) has IsValid=1 and both
    // GetCenter()/GetExtent()==0, so the empty case reports a zeroed box, while the
    // non-empty case keeps origin/extent authentically derived from the real min/max.
    const bool bEmptyBounds = (VertexCount == 0) || (MeshBounds.IsValid == 0);
    const FBox SafeBounds = bEmptyBounds ? FBox(FVector::ZeroVector, FVector::ZeroVector) : MeshBounds;

    TSharedPtr<FJsonObject> BBoxObj = MakeShared<FJsonObject>();
    BBoxObj->SetObjectField(TEXT("min"), JsonBuilders::BuildVectorJson(SafeBounds.Min));
    BBoxObj->SetObjectField(TEXT("max"), JsonBuilders::BuildVectorJson(SafeBounds.Max));
    BBoxObj->SetObjectField(TEXT("origin"), JsonBuilders::BuildVectorJson(SafeBounds.GetCenter()));
    BBoxObj->SetObjectField(TEXT("extent"), JsonBuilders::BuildVectorJson(SafeBounds.GetExtent()));
    Result->SetObjectField(TEXT("boundingBox"), BBoxObj);

    Ctx.SendSuccess(TEXT("Mesh info retrieved"), Result);
    return true;
}

// ============================================================================
// get_vertex_position
// ============================================================================
REGISTER_RPC_HANDLER("geometry.get_vertex_position", "geometry", "Get the position of a vertex in a dynamic mesh",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_REQ("vertexIndex", "integer", "Index of the vertex")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    int32 VertexIndex = Ctx.GetInt(TEXT("vertexIndex"), -1);

    if (ActorName.IsEmpty() || VertexIndex < 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("actorName and vertexIndex required"));
        return true;
    }

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    bool bIsValidVertex = false;
    FVector Position = UGeometryScriptLibrary_MeshQueryFunctions::GetVertexPosition(Target.Mesh, VertexIndex, bIsValidVertex);

    if (!bIsValidVertex)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_VERTEX, FString::Printf(TEXT("Invalid vertex index: %d"), VertexIndex));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("vertexIndex"), VertexIndex);

    TSharedPtr<FJsonObject> PosObj = MakeShared<FJsonObject>();
    PosObj->SetNumberField(TEXT("x"), Position.X);
    PosObj->SetNumberField(TEXT("y"), Position.Y);
    PosObj->SetNumberField(TEXT("z"), Position.Z);
    Result->SetObjectField(TEXT("position"), PosObj);

    Ctx.SendSuccess(TEXT("Vertex position retrieved"), Result);
    return true;
}

// ============================================================================
// set_vertex_position
// ============================================================================
REGISTER_RPC_HANDLER("geometry.set_vertex_position", "geometry", "Set the position of a vertex in a dynamic mesh",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_REQ("vertexIndex", "integer", "Index of the vertex"),
        RPC_PARAM_REQ("position", "object", "New position {x, y, z}")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    int32 VertexIndex = Ctx.GetInt(TEXT("vertexIndex"), -1);
    FVector Position = GeometryUtils::ReadVectorFromPayload(Ctx.GetRawPayload(), TEXT("position"), FVector::ZeroVector);

    if (ActorName.IsEmpty() || VertexIndex < 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("actorName and vertexIndex required"));
        return true;
    }

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    GeometryOps::FSetVertexPositionParams Params;
    Params.VertexIndex = VertexIndex;
    Params.Position = Position;

    const GeometryOps::FOpResult Op = GeometryOps::SetVertexPosition(Target.Mesh, Params);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("vertexIndex"), VertexIndex);

    TSharedPtr<FJsonObject> PosObj = MakeShared<FJsonObject>();
    PosObj->SetNumberField(TEXT("x"), Position.X);
    PosObj->SetNumberField(TEXT("y"), Position.Y);
    PosObj->SetNumberField(TEXT("z"), Position.Z);
    Result->SetObjectField(TEXT("position"), PosObj);

    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Vertex position set"), Result);
    return true;
}

// ============================================================================
// append_vertex
// ============================================================================
REGISTER_RPC_HANDLER("geometry.append_vertex", "geometry", "Add a single vertex to a dynamic mesh",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("position", "object", "Vertex position {x, y, z} (default origin)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    if (ActorName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("actorName required"));
        return true;
    }

    FVector Position = GeometryUtils::ReadVectorFromPayload(Ctx.GetRawPayload(), TEXT("position"), FVector::ZeroVector);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    GeometryOps::FAppendVertexParams Params;
    Params.Position = Position;

    int32 VertexIndex = INDEX_NONE;
    const GeometryOps::FOpResult Op = GeometryOps::AppendVertex(Target.Mesh, Params, VertexIndex);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("vertexIndex"), VertexIndex);
    Result->SetNumberField(TEXT("vertexCount"), Op.VerticesAfter);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Vertex appended"), Result);
    return true;
}

// ============================================================================
// delete_vertex
// ============================================================================
REGISTER_RPC_HANDLER("geometry.delete_vertex", "geometry", "Remove a vertex from a dynamic mesh (also removes connected triangles)",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_REQ("vertexIndex", "integer", "Index of the vertex to delete")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    int32 VertexIndex = Ctx.GetInt(TEXT("vertexIndex"), -1);

    if (ActorName.IsEmpty() || VertexIndex < 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("actorName and vertexIndex required"));
        return true;
    }

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    GeometryOps::FDeleteVertexParams Params;
    Params.VertexIndex = VertexIndex;

    const GeometryOps::FOpResult Op = GeometryOps::DeleteVertex(Target.Mesh, Params);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("vertexIndex"), VertexIndex);
    // `success` has always reported whether the mesh actually removed the vertex, inside an
    // otherwise-successful response. FOpResult::bChanged is that same signal.
    Result->SetBoolField(TEXT("success"), Op.bChanged);
    Result->SetNumberField(TEXT("vertexCount"), Op.VerticesAfter);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Vertex deleted"), Result);
    return true;
}

// ============================================================================
// append_triangle
// ============================================================================
REGISTER_RPC_HANDLER("geometry.append_triangle", "geometry", "Add a triangle to a dynamic mesh by specifying three vertex positions",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("v0", "object", "First vertex {x, y, z} (default 0,0,0)"),
        RPC_PARAM_OPT("v1", "object", "Second vertex {x, y, z} (default 100,0,0)"),
        RPC_PARAM_OPT("v2", "object", "Third vertex {x, y, z} (default 50,100,0)"),
        RPC_PARAM_OPT("groupID", "integer", "Polygon group ID (default 0)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    if (ActorName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("actorName required"));
        return true;
    }

    FVector V0 = GeometryUtils::ReadVectorFromPayload(Ctx.GetRawPayload(), TEXT("v0"), FVector(0, 0, 0));
    FVector V1 = GeometryUtils::ReadVectorFromPayload(Ctx.GetRawPayload(), TEXT("v1"), FVector(100, 0, 0));
    FVector V2 = GeometryUtils::ReadVectorFromPayload(Ctx.GetRawPayload(), TEXT("v2"), FVector(50, 100, 0));
    int32 GroupID = Ctx.GetInt(TEXT("groupID"), 0);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    GeometryOps::FAppendTriangleParams Params;
    Params.V0 = V0;
    Params.V1 = V1;
    Params.V2 = V2;
    Params.GroupID = GroupID;

    GeometryOps::FAppendTriangleIndices Indices;
    const GeometryOps::FOpResult Op = GeometryOps::AppendTriangle(Target.Mesh, Params, Indices);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("triangleIndex"), Indices.TriangleIndex);
    Result->SetNumberField(TEXT("vertexIndex0"), Indices.VertexIndices[0]);
    Result->SetNumberField(TEXT("vertexIndex1"), Indices.VertexIndices[1]);
    Result->SetNumberField(TEXT("vertexIndex2"), Indices.VertexIndices[2]);
    Result->SetNumberField(TEXT("triangleCount"), Op.TrianglesAfter);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Triangle appended"), Result);
    return true;
}

// ============================================================================
// delete_triangle
// ============================================================================
REGISTER_RPC_HANDLER("geometry.delete_triangle", "geometry", "Remove a triangle from a dynamic mesh",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_REQ("triangleIndex", "integer", "Index of the triangle to delete")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    int32 TriangleIndex = Ctx.GetInt(TEXT("triangleIndex"), -1);

    if (ActorName.IsEmpty() || TriangleIndex < 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("actorName and triangleIndex required"));
        return true;
    }

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    GeometryOps::FDeleteTriangleParams Params;
    Params.TriangleIndex = TriangleIndex;

    const GeometryOps::FOpResult Op = GeometryOps::DeleteTriangle(Target.Mesh, Params);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("triangleIndex"), TriangleIndex);
    Result->SetBoolField(TEXT("success"), Op.bChanged);
    Result->SetNumberField(TEXT("triangleCount"), Op.TrianglesAfter);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Triangle deleted"), Result);
    return true;
}

// ============================================================================
// set_vertex_color
// ============================================================================
REGISTER_RPC_HANDLER("geometry.set_vertex_color", "geometry", "Set vertex colors on a dynamic mesh",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("vertexIndex", "integer", "Index of the vertex (-1 if setAll)"),
        RPC_PARAM_OPT("r", "number", "Red channel 0-1 (default 1)"),
        RPC_PARAM_OPT("g", "number", "Green channel 0-1 (default 1)"),
        RPC_PARAM_OPT("b", "number", "Blue channel 0-1 (default 1)"),
        RPC_PARAM_OPT("a", "number", "Alpha channel 0-1 (default 1)"),
        RPC_PARAM_OPT("setAll", "boolean", "If true, set color on all vertices"),
        RPC_PARAM_DEF("channels", "string", "Which channels this write touches: any combination of r, g, b, a (e.g. \"a\" writes only alpha and leaves an RGB tint intact)", "rgba")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    int32 VertexIndex = Ctx.GetInt(TEXT("vertexIndex"), -1);
    double R = Ctx.GetNumber(TEXT("r"), 1.0);
    double G = Ctx.GetNumber(TEXT("g"), 1.0);
    double B = Ctx.GetNumber(TEXT("b"), 1.0);
    double A = Ctx.GetNumber(TEXT("a"), 1.0);
    bool bSetAll = Ctx.GetBool(TEXT("setAll"), false);

    if (ActorName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("actorName required"));
        return true;
    }

    // Default "rgba", so a caller that names no channels writes all four exactly as this verb
    // always did. An unreadable spelling is refused rather than treated as "all": it is a caller
    // naming something specific, and guessing all four would overwrite the channels they meant
    // to keep - which is the whole reason this parameter exists.
    GeometryOps::EColorChannels Channels = GeometryOps::EColorChannels::All;
    {
        FString ChannelError;
        const FString ChannelSpec = Ctx.GetString(TEXT("channels"), TEXT("rgba"));
        if (!GeometryOps::ParseColorChannels(ChannelSpec, Channels, ChannelError))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, ChannelError);
            return true;
        }
    }

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    // The published contract spells the color as four separate numbers; the op's one
    // parameter vocabulary spells it as a color. The translation lives here so the JSON
    // contract is unchanged and .pwmodel's color=(r,g,b,a) maps onto the field directly.
    GeometryOps::FSetVertexColorParams Params;
    Params.VertexIndex = VertexIndex;
    Params.Color = FLinearColor(
        static_cast<float>(R), static_cast<float>(G), static_cast<float>(B), static_cast<float>(A));
    Params.bSetAll = bSetAll;
    Params.Channels = Channels;

    int32 VerticesModified = 0;
    int32 ColorElementsCreated = 0;
    const GeometryOps::FOpResult Op =
        GeometryOps::SetVertexColor(Target.Mesh, Params, VerticesModified, &ColorElementsCreated);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("verticesModified"), VerticesModified);
    // How many colour elements had to be created to cover triangles that carried none - non-zero
    // exactly when the mesh had grown since the last colour write. Reported because the caller
    // otherwise has no way to notice that an earlier set_all had left that geometry unpainted.
    Result->SetNumberField(TEXT("colorElementsCreated"), ColorElementsCreated);
    Result->SetNumberField(TEXT("r"), R);
    Result->SetNumberField(TEXT("g"), G);
    Result->SetNumberField(TEXT("b"), B);
    Result->SetNumberField(TEXT("a"), A);
    // Echoed canonically (always in r,g,b,a order) rather than as the caller spelled it, so the
    // response says which components actually moved - the r/g/b/a echo above reports what was
    // passed, not what was written, and under a mask those are different statements.
    Result->SetStringField(TEXT("channels"), GeometryOps::ColorChannelsToString(Channels));
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Vertex color set"), Result);
    return true;
}

// ============================================================================
// set_uvs
// ============================================================================
REGISTER_RPC_HANDLER("geometry.set_uvs", "geometry", "Set UV coordinates on a dynamic mesh vertex",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_REQ("vertexIndex", "integer", "Index of the vertex"),
        RPC_PARAM_OPT("u", "number", "U coordinate (default 0)"),
        RPC_PARAM_OPT("v", "number", "V coordinate (default 0)"),
        RPC_PARAM_OPT("uvChannel", "integer", "UV channel (default 0)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    int32 VertexIndex = Ctx.GetInt(TEXT("vertexIndex"), -1);
    double U = Ctx.GetNumber(TEXT("u"), 0.0);
    double V = Ctx.GetNumber(TEXT("v"), 0.0);
    int32 UVChannel = Ctx.GetInt(TEXT("uvChannel"), 0);

    if (ActorName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("actorName required"));
        return true;
    }

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    GeometryOps::FSetUVsParams Params;
    Params.VertexIndex = VertexIndex;
    Params.UV = FVector2D(U, V);
    Params.UVChannel = UVChannel;

    int32 ElementsModified = 0;
    const GeometryOps::FOpResult Op = GeometryOps::SetUVs(Target.Mesh, Params, ElementsModified);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("vertexIndex"), VertexIndex);
    Result->SetNumberField(TEXT("u"), U);
    Result->SetNumberField(TEXT("v"), V);
    Result->SetNumberField(TEXT("uvChannel"), UVChannel);
    Result->SetNumberField(TEXT("elementsModified"), ElementsModified);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("UV coordinates set"), Result);
    return true;
}

// ============================================================================
// translate_mesh
// ============================================================================
REGISTER_RPC_HANDLER("geometry.translate_mesh", "geometry", "Translate (move) an entire dynamic mesh",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("translation", "object", "Translation offset {x, y, z}")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    if (ActorName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("actorName required"));
        return true;
    }

    FVector Translation = GeometryUtils::ReadVectorFromPayload(Ctx.GetRawPayload(), TEXT("translation"), FVector::ZeroVector);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    GeometryOps::FTranslateMeshParams Params;
    Params.Translation = Translation;

    const GeometryOps::FOpResult Op = GeometryOps::TranslateMesh(Target.Mesh, Params);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);

    TSharedPtr<FJsonObject> TransObj = MakeShared<FJsonObject>();
    TransObj->SetNumberField(TEXT("x"), Translation.X);
    TransObj->SetNumberField(TEXT("y"), Translation.Y);
    TransObj->SetNumberField(TEXT("z"), Translation.Z);
    Result->SetObjectField(TEXT("translation"), TransObj);

    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Mesh translated"), Result);
    return true;
}

// ============================================================================
// unwrap_uv
// ============================================================================
REGISTER_RPC_HANDLER("geometry.unwrap_uv", "geometry", "Auto-generate UV unwrapping for a dynamic mesh using XAtlas (geometry.auto_uv is an alias of this verb)",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("uvChannel", "integer", "UV channel (default 0)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    int32 UVChannel = Ctx.GetInt(TEXT("uvChannel"), 0);

    if (ActorName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("actorName required"));
        return true;
    }

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    // Canonical XAtlas auto-unwrap. The geometry is the pure GeometryOps::UnwrapUVXAtlas
    // (GeometryOps_Modeling.h); ApplyXAtlasUnwrap is the Ctx-bound shell that runs that op,
    // refreshes the component and sends this verb's response. geometry.auto_uv calls the op
    // directly, so both unwraps execute one implementation. geometry.pack_uv_islands used to
    // come through here too - it does not any more, because a repack must not recompute the
    // layer it is repacking.
    GeometryUtils::ApplyXAtlasUnwrap(
        Ctx, Target.Mesh, Target.Component, ActorName, UVChannel, TEXT("UV unwrapping completed"));
    return true;
}

// ============================================================================
// pack_uv_islands
// ============================================================================
REGISTER_RPC_HANDLER("geometry.pack_uv_islands", "geometry", "Repack the existing UV islands of a dynamic mesh into the unit square, leaving the seams where they are",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("uvChannel", "integer", "UV channel (default 0)"),
        FParamSpec{TEXT("textureResolution"), TEXT("integer"),
            FString::Printf(
                TEXT("Positive texture resolution the packing gutter is sized for, in pixels (engine range %d-%d, default %d; positive values outside that range are clamped with a warning). Drives the spacing left between islands, not the size of anything written"),
                GeometryOps::LayoutUVTextureResolutionMin,
                GeometryOps::LayoutUVTextureResolutionMax,
                GeometryOps::LayoutUVTextureResolutionDefault),
            false, FString::FromInt(GeometryOps::LayoutUVTextureResolutionDefault)}
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    int32 UVChannel = Ctx.GetInt(TEXT("uvChannel"), 0);
    const int32 RequestedTextureResolution = Ctx.GetInt(
        TEXT("textureResolution"), GeometryOps::LayoutUVTextureResolutionDefault);

    if (ActorName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("actorName required"));
        return true;
    }

    // Preserve the verb's established positive-integer contract. FUVPacker clamps positive
    // values outside its narrower domain; do that explicitly so the response can report both
    // the adjustment and the value the engine actually used.
    if (RequestedTextureResolution <= 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
            TEXT("textureResolution must be positive; got %d"), RequestedTextureResolution));
        return true;
    }
    GeometryOps::FOpResult ClampResult = GeometryOps::FOpResult::Ok();
    const int32 TextureResolution = GeometryOps::ClampRangeWarn(RequestedTextureResolution,
        GeometryOps::LayoutUVTextureResolutionMin,
        GeometryOps::LayoutUVTextureResolutionMax,
        TEXT("textureResolution"), ClampResult);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    // This verb REPACKS what is already there. It used to run the same XAtlas auto-unwrap as
    // unwrap_uv and auto_uv, which recomputes the layer from scratch - so the documented
    // unwrap -> project -> transform -> pack order silently discarded its own middle two steps,
    // and `textureResolution` was echoed back having reached no engine call at all. Both halves
    // are gone: GeometryOps::LayoutUV in Repack mode moves the existing islands and carries the
    // resolution into FGeometryScriptLayoutUVsOptions, so the echo below now reports a value the
    // engine actually saw. unwrap_uv / auto_uv keep the XAtlas path, which is correct for them.
    //
    // Packing an EMPTY layer is refused rather than reported as a pack of nothing. LayoutUV's
    // EnsureMeshHasUVChannel creates the layer when it is missing, which would turn "you have no
    // UVs yet" into a success on zero islands - the silent-no-op shape this verb is being
    // repaired out of. The message names the verbs that produce islands, because a caller who
    // reaches for pack first has no other way to learn the order.
    if (!GeometryUtils::MeshHasUVElementsInChannel(Target.Mesh, UVChannel))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
            TEXT("uvChannel %d carries no UV islands to pack. Packing rearranges an existing layout; "
                 "create one first with geometry.unwrap_uv (or geometry.project_uv), then pack."),
            UVChannel));
        return true;
    }

    GeometryOps::FLayoutUVParams Params;
    Params.UVChannel = UVChannel;
    Params.LayoutType = GeometryOps::EUVLayoutType::Repack;
    Params.TextureResolution = TextureResolution;

    GeometryOps::FOpResult Op = GeometryOps::LayoutUV(Target.Mesh, Params);
    Op.Warnings.Append(ClampResult.Warnings);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("uvChannel"), UVChannel);
    Result->SetNumberField(TEXT("textureResolution"), TextureResolution);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("UV islands packed"), Result);
    return true;
}
