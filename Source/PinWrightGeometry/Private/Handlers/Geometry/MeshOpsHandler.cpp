// Copyright (c) 2026 Alexander Penkin. MIT License.

// MeshOpsHandler.cpp - Mesh operations: normals, simplify, subdivide, modeling, deformers, repair, collision, UV, topology (Phase 18)
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Geometry/GeometryOpCounts.h"
#include "Handlers/Geometry/GeometryOpWarnings.h"
#include "Handlers/Geometry/GeometryTarget.h"
#include "Handlers/Geometry/GeometryAssetCreate.h"
#include "Handlers/Geometry/GeometryOps_Modeling.h"
#include "Handlers/Geometry/GeometryUtils.h"
#include "Handlers/Geometry/CollisionHelpers.h"
#include "Dom/JsonObject.h"
#include "Compat/EngineVersionCompat.h"


#include "Components/DynamicMeshComponent.h"
#include "DynamicMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Utils/AssetUtils.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "UDynamicMesh.h"
#include "Editor.h"

#if __has_include("GeometryScript/GeometryScriptTypes.h")
#include "GeometryScript/GeometryScriptTypes.h"
#else
#include "GeometryScriptTypes.h"
#endif

#include "GeometryScript/MeshNormalsFunctions.h"
#include "GeometryScript/MeshSimplifyFunctions.h"
#include "GeometryScript/MeshSubdivideFunctions.h"
#include "GeometryScript/MeshUVFunctions.h"
#include "GeometryScript/MeshModelingFunctions.h"
#include "GeometryScript/MeshSelectionFunctions.h"
#include "GeometryScript/MeshDeformFunctions.h"
#include "GeometryScript/MeshRepairFunctions.h"
#include "GeometryScript/MeshQueryFunctions.h"
#include "GeometryScript/MeshBasicEditFunctions.h"
#include "GeometryScript/MeshTransformFunctions.h"
#include "GeometryScript/CollisionFunctions.h"
#include "GeometryScript/MeshRemeshFunctions.h"
// convert_to_static_mesh's overwrite path: CopyMeshToStaticMesh writes an EXISTING asset
// in place, which CreateNewStaticMeshAssetFromMesh cannot do.
#include "GeometryScript/MeshAssetFunctions.h"
#include "Misc/EngineVersionComparison.h"

// File-local helpers for this file's mesh-op verbs.
namespace
{
    // Vertex/triangle snapshot of a mesh, sampled identically before and after a
    // face op so the change can be reported. Defined once here (single null-guard
    // and accessor choice) instead of being open-coded at each call site.
    struct FMeshCounts
    {
        int32 Verts = 0;
        int32 Tris = 0;
    };

    FMeshCounts SnapshotMeshCounts(UDynamicMesh* Mesh)
    {
        FMeshCounts Counts;
        Counts.Verts = GeometryUtils::GetMeshVertexCount(Mesh);
        // UDynamicMesh::GetTriangleCount() — the MeshQueryFunctions library accessor
        // for triangle count is commented out (does not exist) in UE 5.7.
        Counts.Tris = Mesh ? Mesh->GetTriangleCount() : 0;
        return Counts;
    }

    // Reads the face-op selection rule out of the request.
    //
    // When the caller supplies a `faceDirection` {x,y,z}, the op selects only the
    // triangles whose normal points (within `faceAngleTolerance` degrees, default
    // 45) in that direction — so a single face of a CLOSED solid (e.g. the +Z top
    // of a box) can actually be targeted. Without that argument the spec carries no
    // direction, which the op turns into an empty selection == the whole mesh (the
    // pre-existing behavior).
    //
    // This is the payload half only: building the engine selection is the op's job,
    // so the operation no longer reads Ctx mid-run.
    GeometryOps::FFaceSelectionSpec ReadFaceSelectionSpec(const FHandlerContext& Ctx)
    {
        GeometryOps::FFaceSelectionSpec Spec;

        const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
        Spec.bHasDirection = Payload.IsValid() && Payload->HasField(TEXT("faceDirection"));
        if (!Spec.bHasDirection)
        {
            return Spec;
        }

        Spec.Direction = GeometryUtils::ReadVectorFromPayload(Payload, TEXT("faceDirection"), FVector::UpVector);
        Spec.AngleTolerance = Ctx.GetNumber(TEXT("faceAngleTolerance"), 45.0);
        return Spec;
    }

    // One enum-parameter reader for every widened verb in this file.
    //
    // Absent leaves InOutValue alone, so the op's params struct stays the single source of the
    // default and no default is re-typed at a call site. An unrecognized value is REJECTED with
    // INVALID_ARGUMENT listing what is legal, rather than silently falling back the way the axis
    // reader below does: these vocabularies are new, so there is no shipped behaviour that a
    // rejection could break, and a silently ignored `method=atribute_aware` would hand back the
    // default having reported success.
    //
    // The spellings are the snake_case of the engine ENUMERATOR, identical to the .pwmodel op
    // table's, so one vocabulary serves both front-ends.
    template <typename EnumT>
    bool MeshOpsHandler_ReadEnumParam(
        const FHandlerContext& Ctx, const TCHAR* ParamName,
        TArrayView<const TPair<const TCHAR*, EnumT>> Values, EnumT& InOutValue)
    {
        const FString Requested = Ctx.GetString(ParamName);
        if (Requested.IsEmpty())
        {
            return true;
        }

        for (const TPair<const TCHAR*, EnumT>& Entry : Values)
        {
            if (Requested.Equals(Entry.Key, ESearchCase::IgnoreCase))
            {
                InOutValue = Entry.Value;
                return true;
            }
        }

        TArray<FString> Legal;
        Legal.Reserve(Values.Num());
        for (const TPair<const TCHAR*, EnumT>& Entry : Values)
        {
            Legal.Add(Entry.Key);
        }
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Unknown %s: %s. Use: %s"),
                ParamName, *Requested, *FString::Join(Legal, TEXT(", "))));
        return false;
    }

    // The four GeometryOps::FFaceOpCommonSpec parameters, read once for extrude / inset /
    // outset / offset_faces / poke - the same consolidation the .pwmodel front-end makes with
    // AppendFaceOpCommonParams, so the two cannot publish different spellings or different
    // defaults for one engine field.
    //
    // Returns false when an enum value was rejected, on MeshOpsHandler_ReadEnumParam's contract:
    // the INVALID_ARGUMENT response has already been sent and the caller must return true
    // immediately without touching the mesh.
    bool ReadFaceOpCommonSpec(const FHandlerContext& Ctx, GeometryOps::FFaceOpCommonSpec& InOut)
    {
        static const TPair<const TCHAR*, GeometryOps::EPolyOperationArea> AreaValues[] = {
            { TEXT("entire_selection"), GeometryOps::EPolyOperationArea::EntireSelection },
            { TEXT("per_polygroup"),    GeometryOps::EPolyOperationArea::PerPolygroup },
            { TEXT("per_triangle"),     GeometryOps::EPolyOperationArea::PerTriangle },
        };
        static const TPair<const TCHAR*, GeometryOps::EEditPolygroupMode> GroupValues[] = {
            { TEXT("preserve_existing"), GeometryOps::EEditPolygroupMode::PreserveExisting },
            { TEXT("auto_generate_new"), GeometryOps::EEditPolygroupMode::AutoGenerateNew },
            { TEXT("set_constant"),      GeometryOps::EEditPolygroupMode::SetConstant },
        };

        if (!MeshOpsHandler_ReadEnumParam<GeometryOps::EPolyOperationArea>(
                Ctx, TEXT("areaMode"), AreaValues, InOut.AreaMode))
        {
            return false;
        }
        if (!MeshOpsHandler_ReadEnumParam<GeometryOps::EEditPolygroupMode>(
                Ctx, TEXT("groupMode"), GroupValues, InOut.Groups.GroupMode))
        {
            return false;
        }
        InOut.Groups.ConstantGroup = Ctx.GetInt(TEXT("groupId"), InOut.Groups.ConstantGroup);
        InOut.UVScale = Ctx.GetNumber(TEXT("uvScale"), InOut.UVScale);
        return true;
    }

    // The two warp-extent parameters bend / twist / taper share.
    void ReadWarpExtentSpec(const FHandlerContext& Ctx, GeometryOps::FWarpExtentSpec& InOut)
    {
        InOut.bSymmetricExtents = Ctx.GetBool(TEXT("symmetricExtents"), InOut.bSymmetricExtents);
        InOut.LowerExtent = Ctx.GetNumber(TEXT("lowerExtent"), InOut.LowerExtent);
    }

    // The offset-direction vocabulary offset_faces and poke share.
    bool ReadOffsetFacesType(const FHandlerContext& Ctx, GeometryOps::EOffsetFacesType& InOut)
    {
        static const TPair<const TCHAR*, GeometryOps::EOffsetFacesType> Values[] = {
            { TEXT("vertex_normal"),         GeometryOps::EOffsetFacesType::VertexNormal },
            { TEXT("face_normal"),           GeometryOps::EOffsetFacesType::FaceNormal },
            { TEXT("parallel_face_offset"),  GeometryOps::EOffsetFacesType::ParallelFaceOffset },
        };
        return MeshOpsHandler_ReadEnumParam<GeometryOps::EOffsetFacesType>(
            Ctx, TEXT("offsetType"), Values, InOut);
    }

    // The published axis vocabulary of geometry.stretch and geometry.cylindrify: an
    // already-uppercased "X"/"Y", anything else (including a typo or an empty string) Z.
    // That silent fallback stays here rather than moving into the op, which takes a
    // spellable enum and so has nothing to fall back FROM.
    GeometryOps::EMeshAxis ReadMeshAxis(const FString& UpperAxis)
    {
        if (UpperAxis == TEXT("X")) return GeometryOps::EMeshAxis::X;
        if (UpperAxis == TEXT("Y")) return GeometryOps::EMeshAxis::Y;
        return GeometryOps::EMeshAxis::Z;
    }

    // The two warp-FRAME parameters bend / twist / taper share: which axis the extent is measured
    // along, and where along it the extent is centred. Both default to the struct's own value
    // (Z and the origin), which is the FTransform::Identity the three ops used to hardcode, so a
    // request that omits them reaches GeometryOps with exactly the frame it reached it with
    // before these keys existed.
    //
    // `axis` goes through the same ReadMeshAxis the stretch / cylindrify verbs use, so the whole
    // geometry namespace spells an axis one way and falls back to Z the same way. It has to sit
    // below that function rather than beside its sibling ReadWarpExtentSpec, because this file is
    // one anonymous namespace read top to bottom.
    void ReadWarpFrameSpec(const FHandlerContext& Ctx, GeometryOps::FWarpFrameSpec& InOut)
    {
        const FString Axis = Ctx.GetString(TEXT("axis")).ToUpper();
        if (!Axis.IsEmpty())
        {
            InOut.Axis = ReadMeshAxis(Axis);
        }
        InOut.Center = Ctx.GetVector(TEXT("center"), InOut.Center);
    }

    // The doc text for those two keys, written once because all three verbs publish it verbatim
    // and a warp family that describes its own frame three different ways is a family an author
    // has to read three times.
#define PW_WARP_FRAME_RPC_PARAMS \
        RPC_PARAM_OPT("axis", "string", "Axis the extent is measured along: 'x', 'y' or 'z' (default 'z'). The deform is centred on `center` and spans the extent along THIS axis, so a horizontal form is warped along its own length by naming its own axis rather than by rotating the mesh into Z and back."), \
        RPC_PARAM_OPT("center", "object", "Point the extent is centred on, {x, y, z} in the mesh's own space (default {0,0,0}). This is the axis LINE, not a bounding-box centre: a form that does not sit on the origin needs its own centre here, and a box-derived one would move whenever an earlier op changed the mesh's extent.")

    // The mesh's post-op vertexCount/triangleCount echo moved out to
    // Handlers/Geometry/GeometryOpCounts.h. It was duplicated here and in
    // GeometryTransformHandler.cpp, the second copy prefixed with its file name because two
    // anonymous-namespace functions of the same name collide once Unity merges the TUs; the
    // named-namespace header is the fix CLAUDE.md prescribes for that, and it removes the
    // second body's freedom to drift.

    // The change-detection fields shared by every face op: post-op counts, the number of
    // faces the op ran on, and `changed` — true iff either count moved, so a
    // whole-mesh-duplicate or a no-op is detectable instead of being reported as bare
    // success. All values come from the op, which already sampled them.
    //
    // `facesSelected: 0` no longer means "the whole mesh" on its own. It means that when
    // `faceFilterMatchedNothing` is absent, and it means "an explicit faceDirection matched no
    // face and the op did not run" when that field is present and true — the case that used to
    // operate on the ENTIRE mesh silently (GeometryOps_Modeling.h's FFaceOpOutcome, and the
    // rationale on GeometryOpsModeling_BuildSelection in the .cpp).
    //
    // Emitted only when true, on the same non-empty-only convention as
    // GeometryOps::AddOpWarnings: every call that does not trip the new case keeps a
    // byte-identical response, which is what makes this additive for the ~146 dispatcher tests
    // that assert on response fields.
    void ReportMeshChange(
        const TSharedPtr<FJsonObject>& Result,
        const GeometryOps::FOpResult& Op,
        const GeometryOps::FFaceOpOutcome& Outcome)
    {
        GeometryOps::SetMeshCountFieldsFromOp(Result, Op);
        Result->SetNumberField(TEXT("facesSelected"), Outcome.FacesSelected);
        Result->SetBoolField(TEXT("changed"), Op.bChanged);
        if (Outcome.bFilterMatchedNothing)
        {
            Result->SetBoolField(TEXT("faceFilterMatchedNothing"), true);
        }
    }
}


// ============================================================================
// recalculate_normals
// ============================================================================
REGISTER_RPC_HANDLER("geometry.recalculate_normals", "geometry", "Recalculate normals on a dynamic mesh",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("areaWeighted", "boolean", "Use area-weighted normals (default true)"),
        RPC_PARAM_OPT("angleWeighted", "boolean", "Weight each face's contribution by its corner angle (default true)")
    ))
{
    // There is no `splitAngle` here, and adding one back would be a lie. The engine call this
    // verb makes is RecomputeNormals, whose whole documented contract is that it PRESERVES the
    // existing hard edges - it re-averages within the seams it finds and never creates one, so
    // it has no angle to take (FGeometryScriptCalculateNormalsOptions carries exactly the two
    // weighting flags below). `splitAngle` was published here for a while, accepted, and
    // dropped on the floor with no warning; the .pwmodel op table copied it through to authors
    // on the strength of this declaration. Creating hard edges is ComputeSplitNormals, which is
    // the `split_normals` verb - already published, already taking `splitAngle`, with the same
    // spelling and the same units. Two names for one call is what the one-vocabulary rule
    // exists to prevent, so this verb keeps none.
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    bool bAreaWeighted = Ctx.GetBool(TEXT("areaWeighted"), true);
    bool bAngleWeighted = Ctx.GetBool(TEXT("angleWeighted"), true);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    GeometryOps::FRecalculateNormalsParams Params;
    Params.bAreaWeighted = bAreaWeighted;
    Params.bAngleWeighted = bAngleWeighted;

    const GeometryOps::FOpResult Op = GeometryOps::RecalculateNormals(Target.Mesh, Params);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetBoolField(TEXT("areaWeighted"), bAreaWeighted);
    Result->SetBoolField(TEXT("angleWeighted"), bAngleWeighted);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Normals recalculated"), Result);
    return true;
}

// ============================================================================
// flip_normals
// ============================================================================
REGISTER_RPC_HANDLER("geometry.flip_normals", "geometry", "Flip all normals on a dynamic mesh",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    const GeometryOps::FOpResult Op = GeometryOps::FlipNormals(Target.Mesh, GeometryOps::FFlipNormalsParams());
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Normals flipped"), Result);
    return true;
}

// ============================================================================
// simplify_mesh
// ============================================================================
REGISTER_RPC_HANDLER("geometry.simplify_mesh", "geometry", "Simplify a dynamic mesh to a target percentage of triangles",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("targetPercentage", "number", "Target percentage of triangles to keep (default 50)"),
        RPC_PARAM_OPT("method", "string", "Simplification metric: standard_qem, volume_preserving, attribute_aware (default), attribute_aware_v2. This verb used to force standard_qem with no way to ask for anything else"),
        RPC_PARAM_OPT("allowSeamCollapse", "boolean", "Let the simplifier collapse edges on a UV / normal / material seam (default true)"),
        RPC_PARAM_OPT("allowSeamSmoothing", "boolean", "Let seam vertices move along the seam (default true)"),
        RPC_PARAM_OPT("allowSeamSplits", "boolean", "Let the simplifier split a seam edge (default true)"),
        RPC_PARAM_OPT("preserveVertexPositions", "boolean", "Keep surviving vertices where they started (default false)"),
        RPC_PARAM_OPT("retainQuadricMemory", "boolean", "Trade memory for speed by keeping the quadric cache (default false)"),
        RPC_PARAM_OPT("regularizeWeight", "number", "Small non-zero values improve triangle quality in flat regions (default 0.000001)"),
        RPC_PARAM_OPT("autoCompact", "boolean", "Compact the index space afterwards (default true)"),
        RPC_PARAM_OPT("quadricVariant", "string", "plane_quadric (default) or triangle_quadric"),
        RPC_PARAM_OPT("normalAttributeWeight", "number", "Influence of normals; attribute-aware methods only (default 16)"),
        RPC_PARAM_OPT("tangentAttributeWeight", "number", "Influence of tangents; attribute-aware methods only (default 0.1)"),
        RPC_PARAM_OPT("colorAttributeWeight", "number", "Influence of vertex color; attribute-aware methods only (default 0.1)"),
        RPC_PARAM_OPT("texCoordAttributeWeight", "number", "Influence of UVs; attribute-aware methods only (default 0.5)"),
        RPC_PARAM_OPT("scaleCorrection", "number", "Rebalances geometry error against attribute error for a differently-scaled model; attribute-aware methods only (default 1)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    double TargetPercentage = Ctx.GetNumber(TEXT("targetPercentage"), 50.0);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    // Every default comes off the params struct - none is re-typed here - so this verb and the
    // .pwmodel `simplify_mesh` op cannot drift, and omitting a parameter reaches the engine with
    // exactly what GeometryOps::FSimplifyMeshParams carries.
    //
    // The three FGeometryScriptWeightMapDensity options the engine also offers are absent by
    // design: each needs a weight-map handle naming a map on the mesh, and no verb on this
    // surface creates one. See FSimplifyMeshParams.
    GeometryOps::FSimplifyMeshParams Params;
    Params.TargetPercentage = TargetPercentage;

    static const TPair<const TCHAR*, GeometryOps::ESimplifyMethod> SimplifyMethods[] = {
        { TEXT("standard_qem"),       GeometryOps::ESimplifyMethod::StandardQEM },
        { TEXT("volume_preserving"),  GeometryOps::ESimplifyMethod::VolumePreserving },
        { TEXT("attribute_aware"),    GeometryOps::ESimplifyMethod::AttributeAware },
        { TEXT("attribute_aware_v2"), GeometryOps::ESimplifyMethod::AttributeAwareV2 },
    };
    if (!MeshOpsHandler_ReadEnumParam(Ctx, TEXT("method"), MakeArrayView(SimplifyMethods), Params.Method))
        return true;

    static const TPair<const TCHAR*, GeometryOps::ESimplifyQuadricVariant> QuadricVariants[] = {
        { TEXT("plane_quadric"),    GeometryOps::ESimplifyQuadricVariant::PlaneQuadric },
        { TEXT("triangle_quadric"), GeometryOps::ESimplifyQuadricVariant::TriangleQuadric },
    };
    if (!MeshOpsHandler_ReadEnumParam(Ctx, TEXT("quadricVariant"), MakeArrayView(QuadricVariants), Params.QuadricVariant))
        return true;

    Params.bAllowSeamCollapse = Ctx.GetBool(TEXT("allowSeamCollapse"), Params.bAllowSeamCollapse);
    Params.bAllowSeamSmoothing = Ctx.GetBool(TEXT("allowSeamSmoothing"), Params.bAllowSeamSmoothing);
    Params.bAllowSeamSplits = Ctx.GetBool(TEXT("allowSeamSplits"), Params.bAllowSeamSplits);
    Params.bPreserveVertexPositions = Ctx.GetBool(TEXT("preserveVertexPositions"), Params.bPreserveVertexPositions);
    Params.bRetainQuadricMemory = Ctx.GetBool(TEXT("retainQuadricMemory"), Params.bRetainQuadricMemory);
    Params.RegularizeWeight = Ctx.GetNumber(TEXT("regularizeWeight"), Params.RegularizeWeight);
    Params.bAutoCompact = Ctx.GetBool(TEXT("autoCompact"), Params.bAutoCompact);
    Params.NormalAttributeWeight = Ctx.GetNumber(TEXT("normalAttributeWeight"), Params.NormalAttributeWeight);
    Params.TangentAttributeWeight = Ctx.GetNumber(TEXT("tangentAttributeWeight"), Params.TangentAttributeWeight);
    Params.ColorAttributeWeight = Ctx.GetNumber(TEXT("colorAttributeWeight"), Params.ColorAttributeWeight);
    Params.TexCoordAttributeWeight = Ctx.GetNumber(TEXT("texCoordAttributeWeight"), Params.TexCoordAttributeWeight);
    Params.ScaleCorrection = Ctx.GetNumber(TEXT("scaleCorrection"), Params.ScaleCorrection);

    const GeometryOps::FOpResult Op = GeometryOps::SimplifyMesh(Target.Mesh, Params);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("originalTriangles"), Op.TrianglesBefore);
    Result->SetNumberField(TEXT("simplifiedTriangles"), Op.TrianglesAfter);
    Result->SetNumberField(TEXT("reductionPercent"), (1.0 - ((double)Op.TrianglesAfter / (double)Op.TrianglesBefore)) * 100.0);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Mesh simplified"), Result);
    return true;
}

// ============================================================================
// subdivide
// ============================================================================
REGISTER_RPC_HANDLER("geometry.subdivide", "geometry", "Subdivide a dynamic mesh (PN tessellation)",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("iterations", "integer", "Number of subdivision iterations (default 1, max 6)"),
        RPC_PARAM_OPT("recomputeNormals", "boolean", "Recompute normals from the curved PN patch (default true). This is what makes the result read as smooth; off keeps the faceted normals of the coarse mesh.")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));

    GeometryOps::FSubdivideParams Params;
    Params.Iterations = Ctx.GetInt(TEXT("iterations"), 1);
    Params.bRecomputeNormals = Ctx.GetBool(TEXT("recomputeNormals"), Params.bRecomputeNormals);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    // The iteration clamp, the memory-pressure gate and the triangle-limit estimate all live in
    // the op, so the .pwmodel compiler cannot reach the engine call without them. Their relative
    // order is unchanged; they now run after target resolution rather than before it.
    GeometryOps::FSubdivideOutcome Outcome;
    const GeometryOps::FOpResult Op = GeometryOps::Subdivide(Target.Mesh, Params, &Outcome);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    // The CLAMPED count, as this verb has always echoed.
    Result->SetNumberField(TEXT("iterations"), Outcome.EffectiveIterations);
    Result->SetNumberField(TEXT("originalTriangles"), Op.TrianglesBefore);
    Result->SetNumberField(TEXT("subdividedTriangles"), Op.TrianglesAfter);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Mesh subdivided"), Result);
    return true;
}

// ============================================================================
// auto_uv
// ============================================================================
REGISTER_RPC_HANDLER("geometry.auto_uv", "geometry", "Alias of geometry.unwrap_uv (same XAtlas auto-unwrap); accepts uvChannel, defaults to channel 0",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("uvChannel", "integer", "UV channel (default 0). Same param as geometry.unwrap_uv; the two verbs are the same XAtlas op")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    int32 UVChannel = Ctx.GetInt(TEXT("uvChannel"), 0);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    // True alias of geometry.unwrap_uv: the same XAtlas unwrap, reached through the pure op
    // rather than through GeometryUtils::ApplyXAtlasUnwrap. unwrap_uv and pack_uv_islands still
    // go through that helper, which is now a thin Ctx-bound shell over this same op — so the
    // three verbs still cannot drift, and the response below is the one that shell builds.
    const GeometryOps::FOpResult Op = GeometryOps::UnwrapUVXAtlas(Target.Mesh, UVChannel);
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
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Auto UV generated"), Result);
    return true;
}

// ============================================================================
// convert_to_static_mesh
// ============================================================================
REGISTER_RPC_HANDLER("geometry.convert_to_static_mesh", "geometry", "Bake a dynamic mesh into a StaticMesh asset. Creates a new asset by default; overwrite:true instead rewrites the EXISTING asset's LOD0 (and its HiRes/Nanite source) in place, PRESERVING its material slots, section map, other LODs and collision - which the create path drops. The inverse is geometry.create_from_static_mesh.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("assetPath", "path", "Asset path for the new StaticMesh (default /Game/GeneratedMeshes/<actorName>)"),
        RPC_PARAM_DEF("overwrite", "boolean", "DESTRUCTIVE, opt-in: rewrite the geometry of the EXISTING asset at assetPath instead of creating a new one. Preserves that asset's materials/sections/other LODs/collision, so a create_from_static_mesh -> edit -> convert round trip keeps its materials. Required to bake onto an occupied path at all - the create path refuses with ASSET_ALREADY_EXISTS rather than replacing an asset it did not generate. /Engine/ assets are refused. Default false", "false"),
        RPC_PARAM_DEF("save", "boolean", "Write the .uasset to disk (default true). Pass false to leave the package dirty-in-memory when batching many bakes before one editor.save_all", "true")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    FString AssetPath = Ctx.GetString(TEXT("assetPath"));

    if (ActorName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("actorName required"));
        return true;
    }
    // Preserve explicit assetPath validation before resolving the actor. A default path is
    // deferred until the resolver has selected the actor, so a full object path can never be
    // copied into the asset-name slot.
    if (!AssetPath.IsEmpty())
    {
        const FString SanitizedAssetPath = SanitizeProjectRelativePath(AssetPath);
        if (SanitizedAssetPath.IsEmpty())
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ASSET_PATH, TEXT("Invalid assetPath - rejected due to security validation"));
            return true;
        }
        AssetPath = SanitizedAssetPath;
    }

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    if (AssetPath.IsEmpty())
    {
        AssetPath = GeometryUtils::MakeDefaultGeometryAssetPath(Target.Actor);
        const FString SanitizedAssetPath = SanitizeProjectRelativePath(AssetPath);
        if (SanitizedAssetPath.IsEmpty())
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ASSET_PATH, TEXT("Invalid default assetPath - rejected due to security validation"));
            return true;
        }
        AssetPath = SanitizedAssetPath;
    }

    // Crash guard: the StaticMesh render build recomputes normals/tangents through
    // MikkT (FStaticMeshOperations::ComputeMikktTangents), which REQUIRES a UV channel
    // and indexes [0] into the baked UV array. A mesh authored via append_buffers/
    // append_vertex without uvs has a size-0 UV array -> Array.h OOB assert -> hard
    // editor crash on the async build worker. The crash fires even with tangent
    // recompute off, because normal recompute alone still invokes MikkT — so we must
    // guarantee UVs exist BEFORE the bake, not just toggle the tangent option (which
    // maps to FMeshBuildSettings but does not stop the normal-driven MikkT pass).
    // EnsureMeshHasUVs applies a deterministic box projection when the mesh has none.
    // Hoisted above the create/overwrite fork: the in-place write runs the SAME
    // MikkT-driven build (CommitMeshDescription -> PostEditChange), so it needs the
    // identical guard.
    const bool bMeshHadUVsBeforeGuard = GeometryUtils::MeshHasUsableUVs(Target.Mesh);
    const bool bHasUVs = GeometryUtils::EnsureMeshHasUVs(Target.Mesh);

    // EnsureMeshHasUVs box-projects UVs onto the LIVE level actor's mesh when it had
    // none — a real, persistent edit to the actor that this handler never committed
    // (it has no NotifyMeshUpdated() anywhere, and the ASSET_CREATION_FAILED return
    // below fires after it). Commit only when the guard actually added the layer, so a
    // mesh that already had UVs does not get the level dirtied by an asset-only verb.
    if (!bMeshHadUVsBeforeGuard && bHasUVs)
    {
        GeometryUtils::MarkGeometryActorModified(Target.Component);
    }

    const bool bOverwrite = Ctx.GetBool(TEXT("overwrite"), false);
    const bool bSaveRequested = Ctx.GetBool(TEXT("save"), true);

    UStaticMesh* BakedMesh = nullptr;
    bool bUpdatedInPlace = false;
    bool bHiResWritten = false;
    // Echoed regardless of branch so the response shape does not change between them.
    const bool bRecomputeNormals = bHasUVs;
    const bool bRecomputeTangents = bHasUVs;

    // Populated by whichever branch ran. The two branches differ in WHEN they can write these:
    // the in-place branch rewrites an asset that is already built, so its collision transfer and
    // its save happen after the write; the create branch hands both to CreateStaticMesh as
    // inputs, which lands them before the new asset's single build.
    int32 CollisionElementsCarried = 0;
    FString BakedPackageName;
    int64 BakedSizeBytes = 0;
    bool bSavedToDisk = false;
    // Both branches set this alongside bSavedToDisk. The bool alone cannot tell a deferred
    // edit (flush it) from a failed write (do not) from an unmountable package (never), and
    // the response promises persistence, so the state goes on the wire too.
    EAssetSaveState BakedSaveState = EAssetSaveState::NotRequested;

    // Non-fatal notes about what the bake actually did to the caller's request, emitted as
    // `warnings` below on the same non-empty-only convention as GeometryOps::AddOpWarnings.
    //
    // This verb makes no GeometryOps call, so AddOpWarnings does not apply - but it had the
    // identical defect on a different struct: it read six fields off FStaticMeshCreateResult and
    // never read Warnings, whose only consumer anywhere was the .pwmodel compiler. The dropped
    // notes are load-bearing for a bake. `Mesh carries no usable UV channel 0; normal/tangent
    // recompute disabled for the bake` (GeometryAssetCreate.cpp) means the asset was built with
    // BOTH recomputes off, and a lightmap-channel clamp means the asset does not use the channel
    // that was asked for - either one silently, under a clean success.
    //
    // Both branches feed this, because both can produce a note and neither can see the other's:
    // the create branch gets whatever CreateStaticMesh collected, and the overwrite branch -
    // which never calls it - reports the UV condition itself from the hoisted guard.
    TArray<FString> BakeWarnings;

    if (bOverwrite)
    {
        EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
        // In-place write-back. CopyMeshToStaticMesh takes an EXISTING UStaticMesh and
        // rewrites one LOD's MeshDescription under its own transaction + Modify(), leaving
        // the asset's StaticMaterials array and both section-info maps alone whenever
        // bReplaceMaterials is false (MeshAssetFunctions.cpp:378-392). That is the whole
        // point of this branch: the create path bakes geometry only and resets the asset to
        // the default material (StaticMeshSetMaterialHandler.cpp:5-11), so a round trip
        // through create_from_static_mesh used to lose materials on every re-bake.
        BakedMesh = LoadObject<UStaticMesh>(nullptr, *AssetPath);
        if (!BakedMesh)
        {
            Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
                FString::Printf(TEXT("overwrite:true but no StaticMesh exists at %s - omit overwrite to create it"),
                    *AssetPath));
            return true;
        }
        // Pre-empt the engine's own refusal (MeshAssetFunctions.cpp:164-169) with a message
        // that names the fix. Without this the engine returns Failure with the reason only
        // in the discarded Debug object, and the caller sees a bare CONVERSION_FAILED.
        if (BakedMesh->GetPathName().StartsWith(TEXT("/Engine/")) &&
            !BakedMesh->GetPathName().StartsWith(TEXT("/Engine/Transient")))
        {
            Ctx.SendError(ErrorCodes::ERR_SECURITY_VIOLATION,
                TEXT("Refusing to overwrite a built-in /Engine asset - asset.duplicate it into /Game first"));
            return true;
        }

        FGeometryScriptCopyMeshToAssetOptions WriteOptions;
        WriteOptions.bEnableRecomputeNormals = bHasUVs;
        WriteOptions.bEnableRecomputeTangents = bHasUVs;
        // bReplaceMaterials stays false: materials/sections on the target are preserved.
        // Replacing them is static_mesh.set_material's job and is already covered.
        WriteOptions.bReplaceMaterials = false;
        WriteOptions.bEmitTransaction = true;

        // A Nanite/HiRes asset keeps its authoritative geometry in the HiRes source model;
        // writing only LOD0 would leave that stale and the mesh would REVERT on the next
        // Nanite rebuild. static_mesh.bake_transform hit the same trap and solves it the
        // same way (StaticMeshBakeTransformHandler.cpp:137-145): write both. The HiRes
        // write defers PostEditChange so the asset rebuilds exactly once, after LOD0.
        if (BakedMesh->IsHiResMeshDescriptionValid())
        {
            FGeometryScriptCopyMeshToAssetOptions HiResOptions = WriteOptions;
            HiResOptions.bDeferMeshPostEditChange = true;
            FGeometryScriptMeshWriteLOD HiResTarget;
            HiResTarget.bWriteHiResSource = true;
            EGeometryScriptOutcomePins HiResOutcome = EGeometryScriptOutcomePins::Failure;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
            UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh(
                Target.Mesh, BakedMesh, HiResOptions, HiResTarget, HiResOutcome,
                /*bUseSectionMaterials=*/true, nullptr);
#else
            UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh(
                Target.Mesh, BakedMesh, HiResOptions, HiResTarget, HiResOutcome, nullptr);
#endif
            bHiResWritten = (HiResOutcome == EGeometryScriptOutcomePins::Success);
        }

        FGeometryScriptMeshWriteLOD WriteTarget;
        WriteTarget.bWriteHiResSource = false;
        WriteTarget.LODIndex = 0;
        // The 7-arg form is required on 5.5+ (the 6-arg overload is UE_DEPRECATED(5.5));
        // the 6-arg form is the only one that exists on 5.3/5.4.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
        UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh(
            Target.Mesh, BakedMesh, WriteOptions, WriteTarget, Outcome,
            /*bUseSectionMaterials=*/true, nullptr);
#else
        UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh(
            Target.Mesh, BakedMesh, WriteOptions, WriteTarget, Outcome, nullptr);
#endif

        if (Outcome != EGeometryScriptOutcomePins::Success)
        {
            Ctx.SendError(ErrorCodes::ERR_CONVERSION_FAILED,
                FString::Printf(TEXT("Failed to write the dynamic mesh into the existing StaticMesh asset %s"),
                    *AssetPath));
            return true;
        }
        bUpdatedInPlace = true;

        // B-geometry-convert-static-mesh-drops-collision: the bake rewrites geometry only, so a
        // prior geometry.generate_collision — which applies the simple shapes to the source
        // DynamicMeshComponent's BodySetup, not to the mesh — was silently dropped
        // (static_mesh.describe reported box:0 despite a successful shapeCount:1). Carry the
        // source component's simple collision (AggGeom + trace flag) into the target mesh's own
        // BodySetup BEFORE the save below so it lands in the .uasset, and report the carried
        // element count in-band so success never implies collision that was not transferred.
        CollisionElementsCarried =
            GeometryUtils::TransferSimpleCollisionToStaticMesh(Target.Component, BakedMesh);

        // The create branch gets this exact note from CreateStaticMesh, which runs its own
        // EnsureMeshHasUVs; this branch never calls it, so without this line an in-place bake of
        // a UV-less mesh reported a clean success while building the asset with both recomputes
        // off. Text is copied verbatim from GeometryAssetCreate.cpp so a caller matching on it
        // cannot tell the branches apart.
        if (!bHasUVs)
        {
            BakeWarnings.Add(TEXT("Mesh carries no usable UV channel 0; normal/tangent recompute disabled for the bake"));
        }

        // B-geometry-convert-static-mesh-no-disk-write: CopyMeshToStaticMesh does not write the
        // .uasset, so the rewritten StaticMesh is dirty-in-memory only and is silently lost on
        // the next editor launch (a cold-load open returns ASSET_NOT_FOUND). This bake verb's
        // entire purpose is producing a reusable on-disk asset, so persist for real through the
        // shared real-save wrapper (forced SaveLoadedAsset + IFileManager::FileSize disk probe,
        // gated by ShouldTreatAssetSaveAsSuccess) and report persistence honestly. A StaticMesh
        // is not a Blueprint/widget, so the corruption history that drove the deferred
        // mark-dirty McpSafeAssetSave no-op does not apply — a real save is safe here.
        // `save:false` is the batching escape hatch: mark dirty now, one editor.save_all later.
        bSavedToDisk = bSaveRequested
            ? SaveAssetToDiskReportingPresence(BakedMesh, /*bForce=*/true, &BakedPackageName,
                                               &BakedSizeBytes, &BakedSaveState)
            : false;
        if (!bSaveRequested)
        {
            BakedMesh->MarkPackageDirty();
            BakedPackageName = BakedMesh->GetOutermost() ? BakedMesh->GetOutermost()->GetName() : FString();
        }
    }
    else
    {
        FStaticMeshCreateSpec CreateSpec;
        CreateSpec.AssetPath = AssetPath;
        // Belt-and-suspenders: on the pathological chance UVs still could not be produced
        // (degenerate/empty mesh), keep BOTH recomputes off so MikkT is never invoked
        // without a UV channel. In the normal path bHasUVs is true and both stay on.
        CreateSpec.bRecomputeNormals = bHasUVs;
        CreateSpec.bRecomputeTangents = bHasUVs;
        CreateSpec.bSave = bSaveRequested;
        // No provenance stamp: this verb bakes a live actor, not a durable .pwmodel source, so
        // there is nothing a later compile could re-derive the asset from.

        // Simple collision travels as an INPUT here rather than as a post-bake patch, so it is
        // written into a UBodySetup that has never been cooked. Same source and same reported
        // count as the in-place branch's TransferSimpleCollisionToStaticMesh above.
        const UBodySetup* SourceBodySetup = Target.Component ? Target.Component->GetBodySetup() : nullptr;
        if (SourceBodySetup && SourceBodySetup->AggGeom.GetElementCount() > 0)
        {
            CreateSpec.SimpleCollision = SourceBodySetup->AggGeom;
            CreateSpec.CollisionTrace = SourceBodySetup->CollisionTraceFlag;
        }

        const FStaticMeshCreateResult Created = CreateStaticMesh(Target.Mesh, CreateSpec);
        if (!Created.bSuccess)
        {
            Ctx.SendError(*Created.ErrorCode, Created.ErrorMessage);
            return true;
        }

        BakedMesh = Created.Asset;
        CollisionElementsCarried = Created.CollisionElements;
        bSavedToDisk = Created.bSavedToDisk;
        BakedSaveState = Created.SaveState;
        BakedPackageName = Created.PackageName;
        BakedSizeBytes = Created.SizeBytes;
        // The sixth field this branch used to ignore. Reading five and dropping Warnings is what
        // let a bake disable normal/tangent recompute, or clamp a lightmap index, under a clean
        // success - the same drop-the-result defect the GeometryOps wrappers had, on a different
        // struct.
        BakeWarnings.Append(Created.Warnings);
    }

    // Echo a machine-readable confirmation block so the bake is verifiable in one
    // call. `exists`/`created` track actual on-disk persistence (bSavedToDisk), not
    // the in-memory create (see the no-disk-write rationale above). The topology is
    // read off the same Target.Mesh the handler still holds (the figures
    // asset.get_metadata would report for the baked StaticMesh), and the recompute
    // flags of whichever branch ran are echoed so the caller can confirm the bake
    // settings without a metadata re-read. This collapses the
    // canonical "bake + confirm" intent from 3 RPCs (convert -> asset.exists ->
    // asset.get_metadata) to 1, mirroring how simplify_mesh/subdivide carry their
    // post-op topology inline.
    const FMeshCounts BakedCounts = SnapshotMeshCounts(Target.Mesh);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetBoolField(TEXT("exists"), bSavedToDisk);
    // created stays false on the overwrite branch - the asset already existed, so
    // reporting created:true there would be a lie a caller could act on.
    Result->SetBoolField(TEXT("created"), bSavedToDisk && !bUpdatedInPlace);
    Result->SetStringField(TEXT("package"), BakedPackageName);
    Result->SetNumberField(TEXT("sizeBytes"), static_cast<double>(BakedSizeBytes));
    // Read `class` straight off the just-baked object (the asset class's leaf name,
    // "StaticMesh") instead of a registry FindAssetData round-trip the handler no
    // longer needs now that it holds the typed BakedMesh — the registry can also lag
    // right after creation, whereas the live object is authoritative.
    Result->SetStringField(TEXT("class"), BakedMesh->GetClass()->GetName());
    Result->SetNumberField(TEXT("triangleCount"), BakedCounts.Tris);
    Result->SetNumberField(TEXT("vertexCount"), BakedCounts.Verts);
    // Number of simple-collision primitives carried from the source DynamicMeshActor's
    // BodySetup into the baked StaticMesh (see the drops-collision rationale above). A
    // prior geometry.generate_collision now survives the bake; 0 means the source had no
    // simple collision to transfer, so the caller knows to add collision separately.
    Result->SetNumberField(TEXT("collisionElements"), CollisionElementsCarried);
    Result->SetBoolField(TEXT("nanite"), false);
    Result->SetBoolField(TEXT("recomputeNormals"), bRecomputeNormals);
    Result->SetBoolField(TEXT("recomputeTangents"), bRecomputeTangents);
    // Which branch actually ran, and what the in-place branch guaranteed. `updated` and
    // `created` are mutually exclusive, so a caller never has to infer the branch from the
    // request it sent. materialsPreserved is only meaningful (and only true) in-place -
    // the create path resets the asset to the default material, which is why
    // static_mesh.set_material exists.
    Result->SetBoolField(TEXT("overwrite"), bOverwrite);
    Result->SetBoolField(TEXT("updated"), bUpdatedInPlace && bSavedToDisk);
    Result->SetBoolField(TEXT("hiResWritten"), bHiResWritten);
    Result->SetBoolField(TEXT("materialsPreserved"), bUpdatedInPlace);
    Result->SetNumberField(TEXT("materialSlots"), BakedMesh->GetStaticMaterials().Num());
    // Honest persistence verdict: saveRequested/saved/pendingFlush. The convert verb
    // force-saves unless the caller passed save:false, so a saved:false carries
    // pendingFlush:true to tell the caller an editor.save_all is still required — never
    // letting created:true/updated:true imply durable state for a dirty-only package.
    AddAssetSaveReport(Result, bSaveRequested, bSavedToDisk, BakedSaveState);
    // Last, next to the SendSuccess, on the placement rule GeometryOpWarnings.h states: a
    // wrapper that later grows an early return cannot skip the warnings while still reporting
    // success. Emitted only when non-empty, so every bake that trips nothing is byte-identical.
    if (BakeWarnings.Num() > 0)
    {
        Result->SetArrayField(TEXT("warnings"), EmitStringArray(BakeWarnings));
    }
    Ctx.SendSuccess(bUpdatedInPlace
            ? TEXT("Existing StaticMesh updated in place from DynamicMesh")
            : TEXT("StaticMesh created from DynamicMesh"),
        Result);
    return true;
}

// ============================================================================
// extrude
// ============================================================================
REGISTER_RPC_HANDLER("geometry.extrude", "geometry", "Extrude faces of a dynamic mesh",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("distance", "number", "Extrude distance (default 10)"),
        RPC_PARAM_OPT("direction", "object", "Extrude direction {x, y, z} (default {0,0,1}). Ignored when directionMode is average_face_normal."),
        RPC_PARAM_OPT("directionMode", "string", "fixed_direction (default) pushes every face along `direction`; average_face_normal ignores `direction` and pushes each region along its OWN averaged normal"),
        RPC_PARAM_OPT("areaMode", "string", "How the affected faces are partitioned: entire_selection (default) moves them as ONE slab; per_polygroup / per_triangle run the op once per region so each moves along its own normal"),
        RPC_PARAM_OPT("groupMode", "string", "Polygroup assignment for the new faces: preserve_existing (default), auto_generate_new, set_constant"),
        RPC_PARAM_OPT("groupId", "integer", "Polygroup ID for the new faces; read only when groupMode is set_constant (default 0)"),
        RPC_PARAM_OPT("uvScale", "number", "UV scale for the NEW faces only (default 1); UVs already on the mesh are untouched"),
        RPC_PARAM_OPT("solidsToShells", "boolean", "Turn a closed solid into a shell when the extrusion would otherwise fold it inside out (default true)"),
        RPC_PARAM_OPT("faceDirection", "object", "Target only faces whose normal points {x,y,z} (e.g. {0,0,1}=top). Omit to extrude the whole mesh; on a closed solid that DUPLICATES it. If given and NO face matches, extrude does NOTHING and warns (facesSelected 0, changed false, faceFilterMatchedNothing true) - it does not fall back to the whole mesh. On an OPEN mesh a matching filter also warns: only the selected faces are extruded, so the result is less closed than the unfiltered call."),
        RPC_PARAM_OPT("faceAngleTolerance", "number", "Max normal-angle deviation in degrees for faceDirection selection (default 45)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    double Distance = Ctx.GetNumber(TEXT("distance"), 10.0);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    static const TPair<const TCHAR*, GeometryOps::ELinearExtrudeDirection> ExtrudeDirectionModes[] = {
        { TEXT("fixed_direction"),     GeometryOps::ELinearExtrudeDirection::FixedDirection },
        { TEXT("average_face_normal"), GeometryOps::ELinearExtrudeDirection::AverageFaceNormal },
    };

    GeometryOps::FExtrudeParams Params;
    Params.Distance = Distance;
    Params.Direction = GeometryUtils::ReadVectorFromPayload(Ctx.GetRawPayload(), TEXT("direction"), FVector(0, 0, 1));
    if (!MeshOpsHandler_ReadEnumParam<GeometryOps::ELinearExtrudeDirection>(
            Ctx, TEXT("directionMode"), ExtrudeDirectionModes, Params.DirectionMode))
    {
        return true;
    }
    if (!ReadFaceOpCommonSpec(Ctx, Params.Common))
    {
        return true;
    }
    Params.bSolidsToShells = Ctx.GetBool(TEXT("solidsToShells"), Params.bSolidsToShells);
    Params.Faces = ReadFaceSelectionSpec(Ctx);

    GeometryOps::FFaceOpOutcome Outcome;
    const GeometryOps::FOpResult Op = GeometryOps::Extrude(Target.Mesh, Params, &Outcome);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("distance"), Distance);
    ReportMeshChange(Result, Op, Outcome);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Extrude applied"), Result);
    return true;
}

// ============================================================================
// inset
// ============================================================================
REGISTER_RPC_HANDLER("geometry.inset", "geometry", "Inset faces of a dynamic mesh (shrink inward)",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("distance", "number", "Inset distance (default 5)"),
        RPC_PARAM_OPT("reproject", "boolean", "Reproject the inset ring back onto the original surface so it follows curvature (default true); off leaves it on the plane of the region"),
        RPC_PARAM_OPT("boundaryOnly", "boolean", "Inset only the boundary loop of the region rather than every face in it (default false)"),
        RPC_PARAM_OPT("softness", "number", "Blends the inset ring toward its neighbours; 0 (default) is a hard inset"),
        RPC_PARAM_OPT("areaScale", "number", "Scales the distance by the region's area, so large and small faces inset proportionally (default 1)"),
        RPC_PARAM_OPT("areaMode", "string", "entire_selection (default), per_polygroup, per_triangle"),
        RPC_PARAM_OPT("groupMode", "string", "Polygroup assignment for the new faces: preserve_existing (default), auto_generate_new, set_constant"),
        RPC_PARAM_OPT("groupId", "integer", "Polygroup ID for the new faces; read only when groupMode is set_constant (default 0)"),
        RPC_PARAM_OPT("uvScale", "number", "UV scale for the NEW faces only (default 1)"),
        RPC_PARAM_OPT("faceDirection", "object", "Target only faces whose normal points {x,y,z} (e.g. {0,0,1}=top). Omit to inset the whole mesh; on a closed solid that is a NO-OP. If given and NO face matches, inset does NOTHING and warns (facesSelected 0, changed false, faceFilterMatchedNothing true) - it does not fall back to the whole mesh."),
        RPC_PARAM_OPT("faceAngleTolerance", "number", "Max normal-angle deviation in degrees for faceDirection selection (default 45)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    double Distance = Ctx.GetNumber(TEXT("distance"), 5.0);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    GeometryOps::FInsetParams Params;
    Params.Distance = Distance;
    Params.bReproject = Ctx.GetBool(TEXT("reproject"), Params.bReproject);
    Params.bBoundaryOnly = Ctx.GetBool(TEXT("boundaryOnly"), Params.bBoundaryOnly);
    Params.Softness = Ctx.GetNumber(TEXT("softness"), Params.Softness);
    Params.AreaScale = Ctx.GetNumber(TEXT("areaScale"), Params.AreaScale);
    if (!ReadFaceOpCommonSpec(Ctx, Params.Common))
    {
        return true;
    }
    Params.Faces = ReadFaceSelectionSpec(Ctx);

    GeometryOps::FFaceOpOutcome Outcome;
    const GeometryOps::FOpResult Op = GeometryOps::Inset(Target.Mesh, Params, &Outcome);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetStringField(TEXT("operation"), TEXT("inset"));
    Result->SetNumberField(TEXT("distance"), Distance);
    ReportMeshChange(Result, Op, Outcome);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Inset applied"), Result);
    return true;
}

// ============================================================================
// outset
// ============================================================================
REGISTER_RPC_HANDLER("geometry.outset", "geometry", "Outset faces of a dynamic mesh (expand outward)",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("distance", "number", "Outset distance (default 5)"),
        RPC_PARAM_OPT("boundaryOnly", "boolean", "Outset only the boundary loop of the region rather than every face in it (default false)"),
        RPC_PARAM_OPT("softness", "number", "Blends the outset ring toward its neighbours; 0 (default) is a hard outset"),
        RPC_PARAM_OPT("areaScale", "number", "Scales the distance by the region's area (default 1)"),
        RPC_PARAM_OPT("areaMode", "string", "entire_selection (default), per_polygroup, per_triangle"),
        RPC_PARAM_OPT("groupMode", "string", "Polygroup assignment for the new faces: preserve_existing (default), auto_generate_new, set_constant"),
        RPC_PARAM_OPT("groupId", "integer", "Polygroup ID for the new faces; read only when groupMode is set_constant (default 0)"),
        RPC_PARAM_OPT("uvScale", "number", "UV scale for the NEW faces only (default 1)"),
        RPC_PARAM_OPT("faceDirection", "object", "Target only faces whose normal points {x,y,z} (e.g. {0,0,1}=top). Omit to outset the whole mesh. If given and NO face matches, outset does NOTHING and warns (facesSelected 0, changed false, faceFilterMatchedNothing true) - it does not fall back to the whole mesh."),
        RPC_PARAM_OPT("faceAngleTolerance", "number", "Max normal-angle deviation in degrees for faceDirection selection (default 45)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    double Distance = Ctx.GetNumber(TEXT("distance"), 5.0);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    // No `reproject` here, and its absence is deliberate: the engine honours reprojection only
    // for a positive inset distance and outset always passes a negative one. geometry.inset
    // publishes it. See FOutsetParams.
    GeometryOps::FOutsetParams Params;
    Params.Distance = Distance;
    Params.bBoundaryOnly = Ctx.GetBool(TEXT("boundaryOnly"), Params.bBoundaryOnly);
    Params.Softness = Ctx.GetNumber(TEXT("softness"), Params.Softness);
    Params.AreaScale = Ctx.GetNumber(TEXT("areaScale"), Params.AreaScale);
    if (!ReadFaceOpCommonSpec(Ctx, Params.Common))
    {
        return true;
    }
    Params.Faces = ReadFaceSelectionSpec(Ctx);

    GeometryOps::FFaceOpOutcome Outcome;
    const GeometryOps::FOpResult Op = GeometryOps::Outset(Target.Mesh, Params, &Outcome);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetStringField(TEXT("operation"), TEXT("outset"));
    Result->SetNumberField(TEXT("distance"), Distance);
    ReportMeshChange(Result, Op, Outcome);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Outset applied"), Result);
    return true;
}

// ============================================================================
// bevel
// ============================================================================
REGISTER_RPC_HANDLER("geometry.bevel", "geometry", "Apply bevel to safe polygroup edges of a dynamic mesh. The mesh's polygroups are the edge selection: a mesh with none is returned unchanged, and a mesh with one polygroup per quad gets every interior quad boundary chamfered. The revolved primitives - create_torus, create_arch, revolve - are the second case: they group per quad and cannot be told not to, so bevelling one produces a grid of notches that reads as surface damage at roughly 3x the triangle cost. Both cases warn. Before the engine runs, each selected topology edge is validated by exact ID and ordered mesh-edge span; invalid spans and spans containing an actual mesh-boundary edge are skipped, while internal open spans remain eligible even when they end at a boundary vertex. A measured bevel that adds self-intersections is discarded and the input mesh restored.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("distance", "number", "Bevel distance (default 5). Applied to every safe polygroup edge the filter box leaves in; unsafe edges are skipped with a warning."),
        RPC_PARAM_OPT("subdivisions", "integer", "Number of subdivisions across the bevel (default 0). Multiplies the triangle cost of an already-dense polygroup layout."),
        RPC_PARAM_OPT("roundWeight", "number", "Roundness of the bevel profile (default 1). IGNORED at subdivisions 0 - there is no interior loop to place - and the response warns if set without it."),
        RPC_PARAM_OPT("inferMaterialID", "boolean", "Take the new faces' material from the two faces either side of the bevelled edge when they agree (default false); materialID is used otherwise"),
        RPC_PARAM_OPT("materialID", "integer", "Material ID for the new bevel faces (default 0)"),
        RPC_PARAM_OPT("filterBoxMin", "object", "Lower corner {x,y,z} of a mesh-space box restricting which polygroup edges are bevelled. Both corners must be given for the filter to apply; this is the only edge filter the engine offers and the way out of the every-quad-boundary case above."),
        RPC_PARAM_OPT("filterBoxMax", "object", "Upper corner {x,y,z} of the filter box"),
        RPC_PARAM_OPT("fullyContained", "boolean", "True (default) bevels only edges entirely inside the filter box; false bevels any edge with a vertex in it")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    double BevelDistance = Ctx.GetNumber(TEXT("distance"), 5.0);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    GeometryOps::FBevelParams Params;
    Params.Distance = BevelDistance;
    Params.Subdivisions = Ctx.GetInt(TEXT("subdivisions"), 0);
    Params.RoundWeight = Ctx.GetNumber(TEXT("roundWeight"), Params.RoundWeight);
    Params.bInferMaterialID = Ctx.GetBool(TEXT("inferMaterialID"), Params.bInferMaterialID);
    Params.SetMaterialID = Ctx.GetInt(TEXT("materialID"), Params.SetMaterialID);
    Params.bFullyContained = Ctx.GetBool(TEXT("fullyContained"), Params.bFullyContained);

    // Derived from the two corners being present rather than from a separate flag: a flag with
    // no box and a box with the flag off are two spellings of a request that silently does
    // nothing, and this shape has neither.
    {
        const TSharedPtr<FJsonObject>& BevelPayload = Ctx.GetRawPayload();
        if (BevelPayload.IsValid()
            && BevelPayload->HasField(TEXT("filterBoxMin"))
            && BevelPayload->HasField(TEXT("filterBoxMax")))
        {
            Params.bApplyFilterBox = true;
            Params.FilterBoxMin = GeometryUtils::ReadVectorFromPayload(
                BevelPayload, TEXT("filterBoxMin"), FVector::ZeroVector);
            Params.FilterBoxMax = GeometryUtils::ReadVectorFromPayload(
                BevelPayload, TEXT("filterBoxMax"), FVector::ZeroVector);
        }
    }

    // The pre-5.4 subdivisions rejection lives in the op (it is a property of the engine call,
    // not of the request), so the compiler path cannot silently drop it. It now runs after
    // target resolution rather than before it.
    const GeometryOps::FOpResult Op = GeometryOps::Bevel(Target.Mesh, Params);
    if (!Op.bSuccess)
    {
        // Keep the standardized UNSUPPORTED_ENGINE_VERSION response this verb has always sent,
        // rather than a plain SendError carrying the same code. Deliberately NOT wrapped in a
        // UE_VERSION_OLDER_THAN guard: the version fork moved into GeometryOps::Bevel, so this
        // branch tests only what the op returned. A #if here would have to be kept in step with
        // the op's own fork, and getting that wrong drops the standardized response silently -
        // whereas unguarded it costs one string comparison on an already-failed bevel.
        if (Op.ErrorCode == ErrorCodes::ERR_UNSUPPORTED_ENGINE_VERSION)
        {
            Ctx.SendUnsupportedEngineVersion(TEXT("5.4"), TEXT("Bevel subdivisions"));
            return true;
        }
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("distance"), BevelDistance);
    GeometryOps::SetMeshCountFieldsFromOp(Result, Op);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Bevel applied"), Result);
    return true;
}

// ============================================================================
// offset_faces
// ============================================================================
REGISTER_RPC_HANDLER("geometry.offset_faces", "geometry", "Offset faces of a dynamic mesh along normals",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("distance", "number", "Offset distance (default 5)"),
        RPC_PARAM_OPT("offsetType", "string", "How the per-vertex offset direction is derived: parallel_face_offset (default) moves each face along its own normal by exactly distance so a box stays a box; vertex_normal averages at the corners and rounds them off; face_normal"),
        RPC_PARAM_OPT("solidsToShells", "boolean", "Turn a closed solid into a shell when the offset would otherwise fold it inside out (default true)"),
        RPC_PARAM_OPT("areaMode", "string", "entire_selection (default), per_polygroup, per_triangle"),
        RPC_PARAM_OPT("groupMode", "string", "Polygroup assignment for the new faces: preserve_existing (default), auto_generate_new, set_constant"),
        RPC_PARAM_OPT("groupId", "integer", "Polygroup ID for the new faces; read only when groupMode is set_constant (default 0)"),
        RPC_PARAM_OPT("uvScale", "number", "UV scale for the NEW faces only (default 1)"),
        RPC_PARAM_OPT("faceDirection", "object", "Target only faces whose normal points {x,y,z} (e.g. {0,0,1}=top). Omit to offset the whole mesh. If given and NO face matches, offset_faces does NOTHING and warns (facesSelected 0, changed false, faceFilterMatchedNothing true) - it does not fall back to the whole mesh. On an OPEN mesh a matching filter also warns: only the selected faces are offset, so the result is less closed than the unfiltered call."),
        RPC_PARAM_OPT("faceAngleTolerance", "number", "Max normal-angle deviation in degrees for faceDirection selection (default 45)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    double Distance = Ctx.GetNumber(TEXT("distance"), 5.0);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    GeometryOps::FOffsetFacesParams Params;
    Params.Distance = Distance;
    if (!ReadOffsetFacesType(Ctx, Params.OffsetType))
    {
        return true;
    }
    if (!ReadFaceOpCommonSpec(Ctx, Params.Common))
    {
        return true;
    }
    Params.bSolidsToShells = Ctx.GetBool(TEXT("solidsToShells"), Params.bSolidsToShells);
    Params.Faces = ReadFaceSelectionSpec(Ctx);

    GeometryOps::FFaceOpOutcome Outcome;
    const GeometryOps::FOpResult Op = GeometryOps::OffsetFaces(Target.Mesh, Params, &Outcome);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("distance"), Distance);
    ReportMeshChange(Result, Op, Outcome);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Offset faces applied"), Result);
    return true;
}

// ============================================================================
// shell
// ============================================================================
REGISTER_RPC_HANDLER("geometry.shell", "geometry", "Apply shell/solidify to a dynamic mesh (creates inner wall)",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("thickness", "number", "Shell thickness (default 5)"),
        RPC_PARAM_OPT("fixedBoundary", "boolean", "Pin the boundary loops in place instead of offsetting them (default false)"),
        RPC_PARAM_OPT("solveSteps", "integer", "Iterations of the offset solve (default 5). The offset is a SOLVE, not a rigid translation, so a wall that self-intersects on a concave mesh is usually fixed here rather than by changing thickness."),
        RPC_PARAM_OPT("smoothAlpha", "number", "Smoothing weight per solve step (default 0.1)"),
        RPC_PARAM_OPT("reprojectDuringSmoothing", "boolean", "Re-project onto the source surface between smoothing steps (default false)"),
        RPC_PARAM_OPT("boundaryAlpha", "number", "Smoothing weight at the boundary (default 0.2). The engine's own note is 'should not be > 0.9'.")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    double Thickness = Ctx.GetNumber(TEXT("thickness"), 5.0);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    GeometryOps::FShellParams Params;
    Params.Thickness = Thickness;
    Params.bFixedBoundary = Ctx.GetBool(TEXT("fixedBoundary"), Params.bFixedBoundary);
    Params.SolveSteps = Ctx.GetInt(TEXT("solveSteps"), Params.SolveSteps);
    Params.SmoothAlpha = Ctx.GetNumber(TEXT("smoothAlpha"), Params.SmoothAlpha);
    Params.bReprojectDuringSmoothing =
        Ctx.GetBool(TEXT("reprojectDuringSmoothing"), Params.bReprojectDuringSmoothing);
    Params.BoundaryAlpha = Ctx.GetNumber(TEXT("boundaryAlpha"), Params.BoundaryAlpha);

    const GeometryOps::FOpResult Op = GeometryOps::Shell(Target.Mesh, Params);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("thickness"), Thickness);
    GeometryOps::SetMeshCountFieldsFromOp(Result, Op);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Shell/solidify applied"), Result);
    return true;
}

// ============================================================================
// bend
// ============================================================================
REGISTER_RPC_HANDLER("geometry.bend", "geometry", "Apply bend deformer to a dynamic mesh",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("angle", "number", "Bend angle in degrees (default 45)"),
        RPC_PARAM_OPT("extent", "number", "Bend extent (default 50). SYMMETRIC half-extent measured along `axis` from `center` unless symmetricExtents is false."),
        RPC_PARAM_OPT("symmetricExtents", "boolean", "True (default) spans [-extent, +extent]; false spans [-lowerExtent, +extent], which is what a mesh that does not straddle the origin needs"),
        RPC_PARAM_OPT("lowerExtent", "number", "Lower half-extent (default 10); read only when symmetricExtents is false"),
        RPC_PARAM_OPT("bidirectional", "boolean", "True (default) centres the bend on the origin and rigidly transforms both regions outside the extents; false starts the bend at the lower extent and leaves everything below it untouched"),
        PW_WARP_FRAME_RPC_PARAMS
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    double BendAngle = Ctx.GetNumber(TEXT("angle"), 45.0);
    double BendExtent = Ctx.GetNumber(TEXT("extent"), 50.0);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    GeometryOps::FBendParams Params;
    Params.Angle = BendAngle;
    Params.Extent = BendExtent;
    ReadWarpExtentSpec(Ctx, Params.Extents);
    ReadWarpFrameSpec(Ctx, Params.Frame);
    Params.bBidirectional = Ctx.GetBool(TEXT("bidirectional"), Params.bBidirectional);

    const GeometryOps::FOpResult Op = GeometryOps::Bend(Target.Mesh, Params);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("angle"), BendAngle);
    GeometryOps::SetMeshCountFieldsFromOp(Result, Op);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Bend deformer applied"), Result);
    return true;
}

// ============================================================================
// twist
// ============================================================================
REGISTER_RPC_HANDLER("geometry.twist", "geometry", "Apply twist deformer to a dynamic mesh",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("angle", "number", "Twist angle in degrees (default 45)"),
        RPC_PARAM_OPT("extent", "number", "Twist extent (default 50). SYMMETRIC half-extent measured along `axis` from `center` unless symmetricExtents is false."),
        RPC_PARAM_OPT("symmetricExtents", "boolean", "True (default) spans [-extent, +extent]; false spans [-lowerExtent, +extent]"),
        RPC_PARAM_OPT("lowerExtent", "number", "Lower half-extent (default 10); read only when symmetricExtents is false"),
        RPC_PARAM_OPT("bidirectional", "boolean", "False starts the twist at the lower extent and leaves everything below it untouched (default true)"),
        PW_WARP_FRAME_RPC_PARAMS
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    double TwistAngle = Ctx.GetNumber(TEXT("angle"), 45.0);
    double TwistExtent = Ctx.GetNumber(TEXT("extent"), 50.0);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    GeometryOps::FTwistParams Params;
    Params.Angle = TwistAngle;
    Params.Extent = TwistExtent;
    ReadWarpExtentSpec(Ctx, Params.Extents);
    ReadWarpFrameSpec(Ctx, Params.Frame);
    Params.bBidirectional = Ctx.GetBool(TEXT("bidirectional"), Params.bBidirectional);

    const GeometryOps::FOpResult Op = GeometryOps::Twist(Target.Mesh, Params);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("angle"), TwistAngle);
    GeometryOps::SetMeshCountFieldsFromOp(Result, Op);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Twist deformer applied"), Result);
    return true;
}

// ============================================================================
// taper
// ============================================================================
REGISTER_RPC_HANDLER("geometry.taper", "geometry", "Apply taper/flare deformer to a dynamic mesh",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("flareX", "number", "Flare percentage X (default 50)"),
        RPC_PARAM_OPT("flareY", "number", "Flare percentage Y (default 50)"),
        RPC_PARAM_OPT("extent", "number", "Flare extent (default 50). SYMMETRIC half-extent measured along `axis` from `center` unless symmetricExtents is false."),
        RPC_PARAM_OPT("symmetricExtents", "boolean", "True (default) spans [-extent, +extent]; false spans [-lowerExtent, +extent]"),
        RPC_PARAM_OPT("lowerExtent", "number", "Lower half-extent (default 10); read only when symmetricExtents is false"),
        RPC_PARAM_OPT("flareType", "string", "Displacement profile: sin_mode (default), sin_squared_mode, triangle_mode. sin_squared_mode is the one to reach for when the flare blends into unwarped geometry - its normal derivative is continuous at both ends, so a visible crease at the extent boundary is this parameter rather than the mesh."),
        PW_WARP_FRAME_RPC_PARAMS
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    double FlarePercentX = Ctx.GetNumber(TEXT("flareX"), 50.0);
    double FlarePercentY = Ctx.GetNumber(TEXT("flareY"), 50.0);
    double FlareExtent = Ctx.GetNumber(TEXT("extent"), 50.0);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    static const TPair<const TCHAR*, GeometryOps::EFlareType> FlareTypes[] = {
        { TEXT("sin_mode"),         GeometryOps::EFlareType::SinMode },
        { TEXT("sin_squared_mode"), GeometryOps::EFlareType::SinSquaredMode },
        { TEXT("triangle_mode"),    GeometryOps::EFlareType::TriangleMode },
    };

    GeometryOps::FTaperParams Params;
    Params.FlareX = FlarePercentX;
    Params.FlareY = FlarePercentY;
    Params.Extent = FlareExtent;
    ReadWarpExtentSpec(Ctx, Params.Extents);
    ReadWarpFrameSpec(Ctx, Params.Frame);
    if (!MeshOpsHandler_ReadEnumParam<GeometryOps::EFlareType>(
            Ctx, TEXT("flareType"), FlareTypes, Params.FlareType))
    {
        return true;
    }

    const GeometryOps::FOpResult Op = GeometryOps::Taper(Target.Mesh, Params);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("flareX"), FlarePercentX);
    Result->SetNumberField(TEXT("flareY"), FlarePercentY);
    Result->SetNumberField(TEXT("extent"), FlareExtent);
    GeometryOps::SetMeshCountFieldsFromOp(Result, Op);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Taper/flare deformer applied"), Result);
    return true;
}

// The three warp verbs are the only consumers; dropped here so a Unity-merged sibling TU cannot
// inherit it.
#undef PW_WARP_FRAME_RPC_PARAMS

// ============================================================================
// noise_deform
// ============================================================================
REGISTER_RPC_HANDLER("geometry.noise_deform", "geometry", "Apply Perlin noise deformation to a dynamic mesh",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("magnitude", "number", "Noise magnitude (default 5)"),
        RPC_PARAM_OPT("frequency", "number", "Noise frequency (default 0.25)"),
        RPC_PARAM_OPT("seed", "integer", "Noise seed (default 0). Perlin is a spatial field, so the same seed always reproduces the same displacement at the same position - two meshes differ by noise only if their seeds differ"),
        RPC_PARAM_OPT("frequencyShift", "object", "Per-axis offset {x, y, z} added to position before the noise lookup (default {0,0,0}); slides the sampling window continuously where seed jumps"),
        RPC_PARAM_OPT("applyAlongNormal", "boolean", "Displace along the vertex normal (default true); false displaces by the 3D noise vector"),
        RPC_PARAM_OPT("normalSource", "string", "Normals the displacement rides when applyAlongNormal: 'computed' (default) or 'averageFromOverlay'"),
        RPC_PARAM_OPT("magnitudeMode", "string", "What `magnitude` measures: 'absolute' (default) is a displacement in world units, identical everywhere; 'relative' is a FRACTION OF EACH VERTEX'S MEAN ONE-RING EDGE LENGTH, so one pass follows local feature size across a mesh whose parts differ in scale. Absolute is the engine call unchanged, so existing results are reproduced exactly. Relative needs applyAlongNormal true, samples the same noise field, and is a proxy for feature size that holds only while the tessellation tracks the form - on a uniformly remeshed mesh every vertex reports the same edge length and relative degenerates into absolute.")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    double Magnitude = Ctx.GetNumber(TEXT("magnitude"), 5.0);
    double Frequency = Ctx.GetNumber(TEXT("frequency"), 0.25);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    GeometryOps::FNoiseDeformParams Params;
    Params.Magnitude = Magnitude;
    Params.Frequency = Frequency;
    // Every new key defaults to the params struct's own value, so a request that omits them all
    // reaches GeometryOps with exactly the struct today's two-key call reached it with.
    Params.FrequencyShift = Ctx.GetVector(TEXT("frequencyShift"), Params.FrequencyShift);
    Params.Seed = Ctx.GetInt(TEXT("seed"), Params.Seed);
    Params.bApplyAlongNormal = Ctx.GetBool(TEXT("applyAlongNormal"), Params.bApplyAlongNormal);
    Params.NormalSource = Ctx.GetString(TEXT("normalSource")).Equals(TEXT("averageFromOverlay"), ESearchCase::IgnoreCase)
        ? GeometryOps::ENoiseNormalSource::AverageFromOverlay
        : GeometryOps::ENoiseNormalSource::Computed;
    // Same shape as normalSource above: anything that is not the non-default spelling reads as
    // the default, so an omitted or misspelled key keeps today's absolute behaviour rather than
    // silently rescaling every vertex of an existing recipe.
    Params.MagnitudeMode = Ctx.GetString(TEXT("magnitudeMode")).Equals(TEXT("relative"), ESearchCase::IgnoreCase)
        ? GeometryOps::ENoiseMagnitudeMode::Relative
        : GeometryOps::ENoiseMagnitudeMode::Absolute;

    const GeometryOps::FOpResult Op = GeometryOps::NoiseDeform(Target.Mesh, Params);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("magnitude"), Magnitude);
    // Echoed because `magnitude` alone no longer says what it MEANS: the same number is a world
    // distance under one mode and a fraction of a local edge length under the other.
    Result->SetStringField(TEXT("magnitudeMode"),
        Params.MagnitudeMode == GeometryOps::ENoiseMagnitudeMode::Relative
            ? TEXT("relative") : TEXT("absolute"));
    GeometryOps::SetMeshCountFieldsFromOp(Result, Op);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Noise deformer applied"), Result);
    return true;
}

// ============================================================================
// smooth
// ============================================================================
REGISTER_RPC_HANDLER("geometry.smooth", "geometry", "Apply iterative smoothing to a dynamic mesh",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("iterations", "integer", "Number of smoothing iterations (default 10)"),
        RPC_PARAM_OPT("alpha", "number", "Smoothing strength 0-1 (default 0.2)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    int32 Iterations = Ctx.GetInt(TEXT("iterations"), 10);
    double Alpha = Ctx.GetNumber(TEXT("alpha"), 0.2);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    GeometryOps::FSmoothParams Params;
    Params.Iterations = Iterations;
    Params.Alpha = Alpha;

    const GeometryOps::FOpResult Op = GeometryOps::Smooth(Target.Mesh, Params);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("iterations"), Iterations);
    GeometryOps::SetMeshCountFieldsFromOp(Result, Op);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Smooth applied"), Result);
    return true;
}

// ============================================================================
// relax
// ============================================================================
REGISTER_RPC_HANDLER("geometry.relax", "geometry", "Apply relax (Laplacian smoothing) to a dynamic mesh",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("iterations", "integer", "Number of iterations (default 3)"),
        RPC_PARAM_OPT("strength", "number", "Relaxation strength 0-1 (default 0.5)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    int32 Iterations = Ctx.GetInt(TEXT("iterations"), 3);
    double Strength = Ctx.GetNumber(TEXT("strength"), 0.5);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    GeometryOps::FRelaxParams Params;
    Params.Iterations = Iterations;
    Params.Strength = Strength;

    const GeometryOps::FOpResult Op = GeometryOps::Relax(Target.Mesh, Params);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("iterations"), Iterations);
    Result->SetNumberField(TEXT("strength"), Strength);
    GeometryOps::SetMeshCountFieldsFromOp(Result, Op);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Relax applied"), Result);
    return true;
}

// ============================================================================
// stretch
// ============================================================================
REGISTER_RPC_HANDLER("geometry.stretch", "geometry", "Stretch a dynamic mesh along an axis",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("axis", "string", "Stretch axis: X, Y, or Z (default Z)"),
        RPC_PARAM_OPT("factor", "number", "Stretch factor (default 1.5)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    FString Axis = Ctx.GetString(TEXT("axis"), TEXT("Z")).ToUpper();
    double Factor = Ctx.GetNumber(TEXT("factor"), 1.5);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    GeometryOps::FStretchParams Params;
    Params.Axis = ReadMeshAxis(Axis);
    Params.Factor = Factor;

    const GeometryOps::FOpResult Op = GeometryOps::Stretch(Target.Mesh, Params);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetStringField(TEXT("axis"), Axis);
    Result->SetNumberField(TEXT("factor"), Factor);
    GeometryOps::SetMeshCountFieldsFromOp(Result, Op);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Stretch applied"), Result);
    return true;
}

// ============================================================================
// spherify
// ============================================================================
REGISTER_RPC_HANDLER("geometry.spherify", "geometry", "Project vertices of a dynamic mesh toward a sphere",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("factor", "number", "Spherify factor 0-1 (default 1)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    double Factor = Ctx.GetNumber(TEXT("factor"), 1.0);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    GeometryOps::FSpherifyParams Params;
    Params.Factor = Factor;

    const GeometryOps::FOpResult Op = GeometryOps::Spherify(Target.Mesh, Params);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("factor"), Factor);
    GeometryOps::SetMeshCountFieldsFromOp(Result, Op);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Spherify applied"), Result);
    return true;
}

// ============================================================================
// cylindrify
// ============================================================================
REGISTER_RPC_HANDLER("geometry.cylindrify", "geometry", "Project vertices of a dynamic mesh toward a cylinder",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("axis", "string", "Cylinder axis: X, Y, or Z (default Z)"),
        RPC_PARAM_OPT("factor", "number", "Cylindrify factor 0-1 (default 1)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    FString Axis = Ctx.GetString(TEXT("axis"), TEXT("Z")).ToUpper();
    double Factor = Ctx.GetNumber(TEXT("factor"), 1.0);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    GeometryOps::FCylindrifyParams Params;
    Params.Axis = ReadMeshAxis(Axis);
    Params.Factor = Factor;

    GeometryOps::FCylindrifyOutcome Outcome;
    const GeometryOps::FOpResult Op = GeometryOps::Cylindrify(Target.Mesh, Params, &Outcome);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetStringField(TEXT("axis"), Axis);
    Result->SetNumberField(TEXT("factor"), Factor);
    Result->SetNumberField(TEXT("avgRadius"), Outcome.AverageRadius);
    Result->SetNumberField(TEXT("verticesModified"), Outcome.VerticesModified);
    GeometryOps::SetMeshCountFieldsFromOp(Result, Op);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Cylindrify applied"), Result);
    return true;
}

// ============================================================================
// weld_vertices
// ============================================================================
REGISTER_RPC_HANDLER("geometry.weld_vertices", "geometry", "Weld edges with matching vertices on a dynamic mesh",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("tolerance", "number", "Weld tolerance (default 0.0001). Not the engine's own 1e-06, and geometry.mirror welds its seam at a looser 0.001 again - three tolerances for three jobs, all now settable."),
        RPC_PARAM_OPT("onlyUniquePairs", "boolean", "Merge only unambiguous pairs - edges with exactly one duplicate match (default true). Off lets the weld resolve ambiguous matches, which closes a seam where several edges coincide per vertex and can leave a non-manifold edge when it guesses wrong.")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    double Tolerance = Ctx.GetNumber(TEXT("tolerance"), 0.0001);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    GeometryOps::FWeldVerticesParams Params;
    Params.Tolerance = Tolerance;
    Params.bOnlyUniquePairs = Ctx.GetBool(TEXT("onlyUniquePairs"), Params.bOnlyUniquePairs);

    const GeometryOps::FOpResult Op = GeometryOps::WeldVertices(Target.Mesh, Params);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Vertices welded"), Result);
    return true;
}

// ============================================================================
// fill_holes
// ============================================================================
REGISTER_RPC_HANDLER("geometry.fill_holes", "geometry", "Fill all holes in a dynamic mesh",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("method", "string", "How each hole is triangulated: automatic (default), minimal_fill, polygon_triangulation, triangle_fan, planar_projection. A large planar hole comes back as a fan of slivers under the automatic minimal fill; polygon_triangulation or planar_projection produce a usable triangulation."),
        RPC_PARAM_OPT("deleteIsolatedTriangles", "boolean", "Floating disconnected triangles bound a hole that cannot be filled, so the engine deletes them first (default true). Off keeps them and reports them as failed fills instead.")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    static const TPair<const TCHAR*, GeometryOps::EFillHolesMethod> FillMethods[] = {
        { TEXT("automatic"),             GeometryOps::EFillHolesMethod::Automatic },
        { TEXT("minimal_fill"),          GeometryOps::EFillHolesMethod::MinimalFill },
        { TEXT("polygon_triangulation"), GeometryOps::EFillHolesMethod::PolygonTriangulation },
        { TEXT("triangle_fan"),          GeometryOps::EFillHolesMethod::TriangleFan },
        { TEXT("planar_projection"),     GeometryOps::EFillHolesMethod::PlanarProjection },
    };

    GeometryOps::FFillHolesParams Params;
    if (!MeshOpsHandler_ReadEnumParam<GeometryOps::EFillHolesMethod>(
            Ctx, TEXT("method"), FillMethods, Params.FillMethod))
    {
        return true;
    }
    Params.bDeleteIsolatedTriangles =
        Ctx.GetBool(TEXT("deleteIsolatedTriangles"), Params.bDeleteIsolatedTriangles);

    GeometryOps::FFillHolesOutcome Outcome;
    const GeometryOps::FOpResult Op = GeometryOps::FillHoles(Target.Mesh, Params, &Outcome);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("filledHoles"), Outcome.FilledHoles);
    Result->SetNumberField(TEXT("failedHoles"), Outcome.FailedHoles);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Holes filled"), Result);
    return true;
}

// ============================================================================
// remove_degenerates
// ============================================================================
REGISTER_RPC_HANDLER("geometry.remove_degenerates", "geometry", "Remove degenerate geometry from a dynamic mesh. Both thresholds are ABSOLUTE world units, so on a mesh authored in metres rather than centimetres the defaults are effectively zero and the op looks like it does nothing.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("mode", "string", "What happens to a triangle found degenerate: repair_or_delete (default) collapses it if it can and deletes it otherwise; delete_only skips the collapse; repair_or_skip never deletes, so the triangle count cannot drop"),
        RPC_PARAM_OPT("minTriangleArea", "number", "A triangle below this area is degenerate (default 0.001)"),
        RPC_PARAM_OPT("minEdgeLength", "number", "An edge below this length is degenerate (default 0.0001)"),
        RPC_PARAM_OPT("compactOnCompletion", "boolean", "Compact the vertex and triangle ID lists afterwards (default true). Off preserves existing IDs, which matters when a later call addresses elements by index.")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    static const TPair<const TCHAR*, GeometryOps::ERepairMeshMode> RepairModes[] = {
        { TEXT("delete_only"),      GeometryOps::ERepairMeshMode::DeleteOnly },
        { TEXT("repair_or_delete"), GeometryOps::ERepairMeshMode::RepairOrDelete },
        { TEXT("repair_or_skip"),   GeometryOps::ERepairMeshMode::RepairOrSkip },
    };

    GeometryOps::FRemoveDegeneratesParams Params;
    if (!MeshOpsHandler_ReadEnumParam<GeometryOps::ERepairMeshMode>(
            Ctx, TEXT("mode"), RepairModes, Params.Mode))
    {
        return true;
    }
    Params.MinTriangleArea = Ctx.GetNumber(TEXT("minTriangleArea"), Params.MinTriangleArea);
    Params.MinEdgeLength = Ctx.GetNumber(TEXT("minEdgeLength"), Params.MinEdgeLength);
    Params.bCompactOnCompletion =
        Ctx.GetBool(TEXT("compactOnCompletion"), Params.bCompactOnCompletion);

    const GeometryOps::FOpResult Op = GeometryOps::RemoveDegenerates(Target.Mesh, Params);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Degenerate geometry removed"), Result);
    return true;
}

// ============================================================================
// remesh_uniform
// ============================================================================
REGISTER_RPC_HANDLER("geometry.remesh_uniform", "geometry", "Apply uniform remeshing to a dynamic mesh",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("targetTriangleCount", "integer", "Target triangle count (default 5000). Read only when targetType is triangle_count, and approximate either way"),
        RPC_PARAM_OPT("targetType", "string", "triangle_count (default) or target_edge_length"),
        RPC_PARAM_OPT("targetEdgeLength", "number", "Desired edge length in Unreal units; read only when targetType is target_edge_length (default 1)"),
        RPC_PARAM_OPT("discardAttributes", "boolean", "Drop every mesh attribute first, losing UV and normal seams (default false)"),
        RPC_PARAM_OPT("reprojectToInputMesh", "boolean", "Project vertices back onto the input surface, preserving shape (default true)"),
        RPC_PARAM_OPT("smoothingType", "string", "uniform, uv_preserving, or mixed (default)"),
        RPC_PARAM_OPT("smoothingRate", "number", "Smoothing speed 0-1; 0 disables smoothing (default 0.25)"),
        RPC_PARAM_OPT("meshBoundaryConstraint", "string", "fixed, refine, free (default), or ignore"),
        RPC_PARAM_OPT("groupBoundaryConstraint", "string", "fixed, refine, free (default), or ignore"),
        RPC_PARAM_OPT("materialBoundaryConstraint", "string", "fixed, refine, free (default), or ignore"),
        RPC_PARAM_OPT("allowFlips", "boolean", "Allow edge flips; off markedly lowers quality (default true)"),
        RPC_PARAM_OPT("allowSplits", "boolean", "Allow edge splits, i.e. let density rise (default true)"),
        RPC_PARAM_OPT("allowCollapses", "boolean", "Allow edge collapses, i.e. let density fall (default true)"),
        RPC_PARAM_OPT("preventNormalFlips", "boolean", "Skip any flip or collapse that would flip a face normal (default true)"),
        RPC_PARAM_OPT("preventTinyTriangles", "boolean", "Skip any flip or collapse that would create a degenerate triangle (default true)"),
        RPC_PARAM_OPT("useFullRemeshPasses", "boolean", "Use the expensive full-pass strategy instead of the edge queue (default false)"),
        RPC_PARAM_OPT("iterations", "integer", "Remesh iterations (default 20)"),
        RPC_PARAM_OPT("autoCompact", "boolean", "Compact the index space afterwards (default true)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    int32 TargetTriangleCount = Ctx.GetInt(TEXT("targetTriangleCount"), 5000);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    // As with simplify_mesh: every default is the params struct's, so this verb and the
    // .pwmodel `remesh_uniform` op reach ApplyUniformRemesh with the same two option structs.
    GeometryOps::FRemeshUniformParams Params;
    Params.TargetTriangleCount = TargetTriangleCount;

    static const TPair<const TCHAR*, GeometryOps::ERemeshTargetType> RemeshTargetTypes[] = {
        { TEXT("triangle_count"),     GeometryOps::ERemeshTargetType::TriangleCount },
        { TEXT("target_edge_length"), GeometryOps::ERemeshTargetType::TargetEdgeLength },
    };
    if (!MeshOpsHandler_ReadEnumParam(Ctx, TEXT("targetType"), MakeArrayView(RemeshTargetTypes), Params.TargetType))
        return true;

    static const TPair<const TCHAR*, GeometryOps::ERemeshSmoothingType> RemeshSmoothingTypes[] = {
        { TEXT("uniform"),       GeometryOps::ERemeshSmoothingType::Uniform },
        { TEXT("uv_preserving"), GeometryOps::ERemeshSmoothingType::UVPreserving },
        { TEXT("mixed"),         GeometryOps::ERemeshSmoothingType::Mixed },
    };
    if (!MeshOpsHandler_ReadEnumParam(Ctx, TEXT("smoothingType"), MakeArrayView(RemeshSmoothingTypes), Params.SmoothingType))
        return true;

    static const TPair<const TCHAR*, GeometryOps::ERemeshEdgeConstraint> RemeshEdgeConstraints[] = {
        { TEXT("fixed"),  GeometryOps::ERemeshEdgeConstraint::Fixed },
        { TEXT("refine"), GeometryOps::ERemeshEdgeConstraint::Refine },
        { TEXT("free"),   GeometryOps::ERemeshEdgeConstraint::Free },
        { TEXT("ignore"), GeometryOps::ERemeshEdgeConstraint::Ignore },
    };
    if (!MeshOpsHandler_ReadEnumParam(Ctx, TEXT("meshBoundaryConstraint"), MakeArrayView(RemeshEdgeConstraints), Params.MeshBoundaryConstraint))
        return true;
    if (!MeshOpsHandler_ReadEnumParam(Ctx, TEXT("groupBoundaryConstraint"), MakeArrayView(RemeshEdgeConstraints), Params.GroupBoundaryConstraint))
        return true;
    if (!MeshOpsHandler_ReadEnumParam(Ctx, TEXT("materialBoundaryConstraint"), MakeArrayView(RemeshEdgeConstraints), Params.MaterialBoundaryConstraint))
        return true;

    Params.TargetEdgeLength = Ctx.GetNumber(TEXT("targetEdgeLength"), Params.TargetEdgeLength);
    Params.bDiscardAttributes = Ctx.GetBool(TEXT("discardAttributes"), Params.bDiscardAttributes);
    Params.bReprojectToInputMesh = Ctx.GetBool(TEXT("reprojectToInputMesh"), Params.bReprojectToInputMesh);
    Params.SmoothingRate = Ctx.GetNumber(TEXT("smoothingRate"), Params.SmoothingRate);
    Params.bAllowFlips = Ctx.GetBool(TEXT("allowFlips"), Params.bAllowFlips);
    Params.bAllowSplits = Ctx.GetBool(TEXT("allowSplits"), Params.bAllowSplits);
    Params.bAllowCollapses = Ctx.GetBool(TEXT("allowCollapses"), Params.bAllowCollapses);
    Params.bPreventNormalFlips = Ctx.GetBool(TEXT("preventNormalFlips"), Params.bPreventNormalFlips);
    Params.bPreventTinyTriangles = Ctx.GetBool(TEXT("preventTinyTriangles"), Params.bPreventTinyTriangles);
    Params.bUseFullRemeshPasses = Ctx.GetBool(TEXT("useFullRemeshPasses"), Params.bUseFullRemeshPasses);
    Params.RemeshIterations = Ctx.GetInt(TEXT("iterations"), Params.RemeshIterations);
    Params.bAutoCompact = Ctx.GetBool(TEXT("autoCompact"), Params.bAutoCompact);

    const GeometryOps::FOpResult Op = GeometryOps::RemeshUniform(Target.Mesh, Params);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("targetTriangleCount"), TargetTriangleCount);
    // targetTriangleCount above echoes the REQUESTED budget. Uniform remesh only
    // APPROXIMATES that budget (ApplyUniformRemesh targets an edge length, so the
    // achieved count can diverge substantially from the request), so also echo the
    // ACHIEVED vertexCount/triangleCount read off the just-remeshed Target.Mesh —
    // matching the keys geometry.get_mesh_info returns. The achieved triangleCount is
    // a distinct key from targetTriangleCount so it cannot be mistaken for the input
    // echo, and "did the retopo hit close to my budget?" is answerable without a
    // follow-up get_mesh_info readback.
    GeometryOps::SetMeshCountFieldsFromOp(Result, Op);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Uniform remesh applied"), Result);
    return true;
}

// ============================================================================
// merge_vertices
// ============================================================================
REGISTER_RPC_HANDLER("geometry.merge_vertices", "geometry", "Merge nearby vertices on a dynamic mesh",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("tolerance", "number", "Merge distance tolerance (default 0.001)"),
        RPC_PARAM_OPT("compact", "boolean", "Compact mesh after merge (default true)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    double Tolerance = Ctx.GetNumber(TEXT("tolerance"), 0.001);
    bool bCompactMesh = Ctx.GetBool(TEXT("compact"), true);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    GeometryOps::FMergeVerticesParams Params;
    Params.Tolerance = Tolerance;
    Params.bCompact = bCompactMesh;

    GeometryOps::FMergeVerticesOutcome Outcome;
    const GeometryOps::FOpResult Op = GeometryOps::MergeVertices(Target.Mesh, Params, &Outcome);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("tolerance"), Tolerance);
    Result->SetNumberField(TEXT("verticesBefore"), Op.VerticesBefore);
    Result->SetNumberField(TEXT("verticesAfter"), Op.VerticesAfter);
    // merged = vertices actually welded away (EMeshResult::Ok), which equals the
    // before/after delta when compaction runs but is reported directly so the count
    // is correct even with compact=false (deleted-but-not-compacted slots).
    Result->SetNumberField(TEXT("merged"), Outcome.Merged);
    // vertexCount echoes the same post-op count already in verticesAfter.
    GeometryOps::SetMeshCountFieldsFromOp(Result, Op);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Vertices merged"), Result);
    return true;
}

// ============================================================================
// generate_collision
// ============================================================================
REGISTER_RPC_HANDLER("geometry.generate_collision", "geometry", "Generate collision shapes for a dynamic mesh",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("collisionType", "string", "Collision type: box, sphere, capsule, convex, convex_decomposition, auto (default convex)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    FString CollisionType = Ctx.GetString(TEXT("collisionType"), TEXT("convex"));

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    FGeometryScriptCollisionFromMeshOptions CollisionOptions;
    CollisionOptions.bEmitTransaction = false;

    if (CollisionType == TEXT("box") || CollisionType == TEXT("boxes"))
    {
        CollisionOptions.Method = EGeometryScriptCollisionGenerationMethod::AlignedBoxes;
    }
    else if (CollisionType == TEXT("sphere") || CollisionType == TEXT("spheres"))
    {
        CollisionOptions.Method = EGeometryScriptCollisionGenerationMethod::MinimalSpheres;
    }
    else if (CollisionType == TEXT("capsule") || CollisionType == TEXT("capsules"))
    {
        CollisionOptions.Method = EGeometryScriptCollisionGenerationMethod::Capsules;
    }
    else if (CollisionType == TEXT("convex"))
    {
        CollisionOptions.Method = EGeometryScriptCollisionGenerationMethod::ConvexHulls;
        CollisionOptions.MaxConvexHullsPerMesh = 1;
    }
    else if (CollisionType == TEXT("convex_decomposition"))
    {
        CollisionOptions.Method = EGeometryScriptCollisionGenerationMethod::ConvexHulls;
        CollisionOptions.MaxConvexHullsPerMesh = 8;
    }
    else
    {
        CollisionOptions.Method = EGeometryScriptCollisionGenerationMethod::MinVolumeShapes;
    }

    int32 ShapeCount = GeometryUtils::GenerateAndApplyCollision(Target.Mesh, Target.Component, CollisionOptions);

    // Collision-only edit: the component's UBodySetup changed, the mesh did not, so skip
    // the render-proxy rebuild. bEmitTransaction is false above, so nothing else dirties.
    GeometryUtils::MarkGeometryActorModified(Target.Component, /*bNotifyMesh=*/false);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetStringField(TEXT("collisionType"), CollisionType);
    Result->SetNumberField(TEXT("shapeCount"), ShapeCount);
    Ctx.SendSuccess(TEXT("Collision generated"), Result);
    return true;
}

// ============================================================================
// poke
// ============================================================================
REGISTER_RPC_HANDLER("geometry.poke", "geometry", "Poke faces of a dynamic mesh (offset + subdivide)",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("offset", "number", "Poke offset distance (default 0)"),
        RPC_PARAM_OPT("offsetType", "string", "How the offset direction is derived: parallel_face_offset (default), vertex_normal, face_normal"),
        RPC_PARAM_OPT("solidsToShells", "boolean", "Turn a closed solid into a shell when the offset would otherwise fold it inside out (default true)"),
        RPC_PARAM_OPT("areaMode", "string", "entire_selection (default), per_polygroup, per_triangle"),
        RPC_PARAM_OPT("groupMode", "string", "Polygroup assignment for the new faces: preserve_existing (default), auto_generate_new, set_constant"),
        RPC_PARAM_OPT("groupId", "integer", "Polygroup ID for the new faces; read only when groupMode is set_constant (default 0)"),
        RPC_PARAM_OPT("uvScale", "number", "UV scale for the NEW faces only (default 1)"),
        RPC_PARAM_OPT("recomputeNormals", "boolean", "Recompute normals from the curved PN patch after the tessellation (default true)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    double PokeOffset = Ctx.GetNumber(TEXT("offset"), 0.0);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    GeometryOps::FPokeParams Params;
    Params.Offset = PokeOffset;
    if (!ReadOffsetFacesType(Ctx, Params.OffsetType))
    {
        return true;
    }
    if (!ReadFaceOpCommonSpec(Ctx, Params.Common))
    {
        return true;
    }
    Params.bSolidsToShells = Ctx.GetBool(TEXT("solidsToShells"), Params.bSolidsToShells);
    Params.bRecomputeNormals = Ctx.GetBool(TEXT("recomputeNormals"), Params.bRecomputeNormals);

    // Memory-pressure gate and triangle-limit estimate both live in the op now, in the same
    // order; they run after target resolution rather than before it.
    const GeometryOps::FOpResult Op = GeometryOps::Poke(Target.Mesh, Params);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("offset"), PokeOffset);
    Result->SetNumberField(TEXT("triangleCount"), Op.TrianglesAfter);
    Result->SetNumberField(TEXT("originalTriangles"), Op.TrianglesBefore);
    // poke explodes the vertex count (offset_faces + PN-tessellate); echo it alongside the
    // triangleCount it already reports so callers don't need a follow-up get_mesh_info purely
    // to read the verts.
    Result->SetNumberField(TEXT("vertexCount"), Op.VerticesAfter);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Poke applied"), Result);
    return true;
}

// ============================================================================
// project_uv
// ============================================================================
REGISTER_RPC_HANDLER("geometry.project_uv", "geometry", "Apply UV projection (box, planar, or cylindrical) to a dynamic mesh",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("projectionType", "string", "Projection type: box, planar, cylindrical (default box)"),
        RPC_PARAM_OPT("scale", "number", "Projection scale (default 1)"),
        RPC_PARAM_OPT("uvChannel", "integer", "UV channel (default 0)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    FString ProjectionType = Ctx.GetString(TEXT("projectionType"), TEXT("box")).ToLower();
    double Scale = Ctx.GetNumber(TEXT("scale"), 1.0);
    int32 UVChannel = Ctx.GetInt(TEXT("uvChannel"), 0);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    // Validate the projection type BEFORE calling the op so a rejected request leaves no side
    // effect — previously the UV-layer ensure ran first, so an unknown projectionType still
    // added an empty UV layer to the mesh before erroring out. The name-to-mode mapping (and
    // the "cube" alias) is this verb's published vocabulary, so it stays in the wrapper; the op
    // takes a spellable enum and has no unknown case to reject.
    GeometryOps::FProjectUVParams Params;
    // This verb publishes one `scale` number; the op's frame is 2D, so both axes take it and the
    // resulting projection frame is the uniform one this verb has always built.
    Params.Scale = FVector2D(Scale, Scale);
    Params.UVChannel = UVChannel;
    if (ProjectionType == TEXT("box") || ProjectionType == TEXT("cube"))
    {
        Params.Projection = GeometryOps::EUVProjectionMode::Box;
    }
    else if (ProjectionType == TEXT("planar"))
    {
        Params.Projection = GeometryOps::EUVProjectionMode::Planar;
    }
    else if (ProjectionType == TEXT("cylindrical"))
    {
        Params.Projection = GeometryOps::EUVProjectionMode::Cylindrical;
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(TEXT("Unknown projection type: %s. Use: box, planar, cylindrical"), *ProjectionType));
        return true;
    }

    const GeometryOps::FOpResult Op = GeometryOps::ProjectUV(Target.Mesh, Params);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetStringField(TEXT("projectionType"), ProjectionType);
    Result->SetNumberField(TEXT("scale"), Scale);
    Result->SetNumberField(TEXT("uvChannel"), UVChannel);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("UV projection applied"), Result);
    return true;
}

// ============================================================================
// transform_uvs
// ============================================================================
REGISTER_RPC_HANDLER("geometry.transform_uvs", "geometry", "Transform UV coordinates (translate, scale, rotate)",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("uvChannel", "integer", "UV channel (default 0)"),
        RPC_PARAM_OPT("translateU", "number", "UV translation U (default 0)"),
        RPC_PARAM_OPT("translateV", "number", "UV translation V (default 0)"),
        RPC_PARAM_OPT("scaleU", "number", "UV scale U (default 1)"),
        RPC_PARAM_OPT("scaleV", "number", "UV scale V (default 1)"),
        RPC_PARAM_OPT("rotation", "number", "UV rotation in degrees (default 0)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    int32 UVChannel = Ctx.GetInt(TEXT("uvChannel"), 0);
    double TranslateU = Ctx.GetNumber(TEXT("translateU"), 0.0);
    double TranslateV = Ctx.GetNumber(TEXT("translateV"), 0.0);
    double ScaleU = Ctx.GetNumber(TEXT("scaleU"), 1.0);
    double ScaleV = Ctx.GetNumber(TEXT("scaleV"), 1.0);
    double Rotation = Ctx.GetNumber(TEXT("rotation"), 0.0);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    GeometryOps::FTransformUVsParams Params;
    Params.UVChannel = UVChannel;
    Params.Translate = FVector2D(TranslateU, TranslateV);
    Params.Scale = FVector2D(ScaleU, ScaleV);
    Params.Rotation = Rotation;

    const GeometryOps::FOpResult Op = GeometryOps::TransformUVs(Target.Mesh, Params);
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
    Result->SetNumberField(TEXT("translateU"), TranslateU);
    Result->SetNumberField(TEXT("translateV"), TranslateV);
    Result->SetNumberField(TEXT("scaleU"), ScaleU);
    Result->SetNumberField(TEXT("scaleV"), ScaleV);
    Result->SetNumberField(TEXT("rotation"), Rotation);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("UVs transformed"), Result);
    return true;
}

// ============================================================================
// recompute_tangents
// ============================================================================
REGISTER_RPC_HANDLER("geometry.recompute_tangents", "geometry", "Recompute tangents on a dynamic mesh. A tangent basis only means anything relative to a UV unwrap, so uvLayer must name the channel the normal map was authored against - point it at the wrong one and the bake reads inverted along one axis.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("type", "string", "Tangent algorithm: fast_mikkt (default), per_triangle, standard_mikkt. standard_mikkt is the reference implementation and matches what most bakers assume."),
        RPC_PARAM_OPT("uvLayer", "integer", "UV channel the tangent basis is built from (default 0)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    static const TPair<const TCHAR*, GeometryOps::ETangentType> TangentTypes[] = {
        { TEXT("fast_mikkt"),     GeometryOps::ETangentType::FastMikkT },
        { TEXT("per_triangle"),   GeometryOps::ETangentType::PerTriangle },
        { TEXT("standard_mikkt"), GeometryOps::ETangentType::StandardMikkT },
    };

    GeometryOps::FRecomputeTangentsParams Params;
    if (!MeshOpsHandler_ReadEnumParam<GeometryOps::ETangentType>(
            Ctx, TEXT("type"), TangentTypes, Params.Type))
    {
        return true;
    }
    Params.UVLayer = Ctx.GetInt(TEXT("uvLayer"), Params.UVLayer);

    const GeometryOps::FOpResult Op = GeometryOps::RecomputeTangents(Target.Mesh, Params);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Tangents recomputed"), Result);
    return true;
}

// ============================================================================
// split_normals
// ============================================================================
REGISTER_RPC_HANDLER("geometry.split_normals", "geometry", "Split normals on a dynamic mesh based on angle threshold",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("splitAngle", "number", "Normal split angle in degrees (default 60). Not the engine's own 15: this surface has published 60 since it shipped."),
        RPC_PARAM_OPT("splitByOpeningAngle", "boolean", "Split where the dihedral angle exceeds splitAngle (default true). With this and splitByFaceGroup both off there is no split predicate left, so the op recomputes normals with NO hard edges - the mesh comes back fully smoothed rather than unchanged, and it warns."),
        RPC_PARAM_OPT("splitByFaceGroup", "boolean", "Also split along polygroup boundaries (default false). Independent of the angle test - the engine ORs the two."),
        RPC_PARAM_OPT("useDefaultGroupLayer", "boolean", "Use the standard polygroup layer (default true); read only when splitByFaceGroup"),
        RPC_PARAM_OPT("groupLayerIndex", "integer", "Index of an extended polygroup layer (default 0); read only when splitByFaceGroup and useDefaultGroupLayer is false")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    double SplitAngle = Ctx.GetNumber(TEXT("splitAngle"), 60.0);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    GeometryOps::FSplitNormalsParams Params;
    Params.SplitAngle = SplitAngle;
    Params.bSplitByOpeningAngle = Ctx.GetBool(TEXT("splitByOpeningAngle"), Params.bSplitByOpeningAngle);
    Params.bSplitByFaceGroup = Ctx.GetBool(TEXT("splitByFaceGroup"), Params.bSplitByFaceGroup);
    Params.bUseDefaultGroupLayer = Ctx.GetBool(TEXT("useDefaultGroupLayer"), Params.bUseDefaultGroupLayer);
    Params.ExtendedGroupLayerIndex = Ctx.GetInt(TEXT("groupLayerIndex"), Params.ExtendedGroupLayerIndex);

    const GeometryOps::FOpResult Op = GeometryOps::SplitNormals(Target.Mesh, Params);
    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("splitAngle"), SplitAngle);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(TEXT("Split normals applied"), Result);
    return true;
}
