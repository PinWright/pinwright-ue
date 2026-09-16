// Copyright (c) 2026 Alexander Penkin. MIT License.

// LODCollisionHandler.cpp - LOD and collision operations: generate_complex_collision,
//   simplify_collision, set_lod_settings (Phase 19)
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Geometry/GeometryTarget.h"
#include "Handlers/Geometry/GeometryUtils.h"
#include "Handlers/Geometry/CollisionHelpers.h"
#include "PinWrightHelpers.h"
#include "Utils/MeshRebuildRenderGuard.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/EngineVersionComparison.h"


#include "Components/DynamicMeshComponent.h"
// FGeometryTarget::Actor is ADynamicMeshActor*: the complete type is required here even with no
// literal ADynamicMeshActor spelled in this file - Target.Actor is member-called / converted to
// AActor*, and a derived-to-base conversion needs the definition, not a forward declaration.
#include "DynamicMeshActor.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "UDynamicMesh.h"
#include "Engine/StaticMesh.h"
#include "Editor.h"

#if __has_include("GeometryScript/GeometryScriptTypes.h")
#include "GeometryScript/GeometryScriptTypes.h"
#else
#include "GeometryScriptTypes.h"
#endif

#include "GeometryScript/MeshSimplifyFunctions.h"
#include "GeometryScript/CreateNewAssetUtilityFunctions.h"

// Collision functions (UE 5.4+)
#include "GeometryScript/CollisionFunctions.h"


// ============================================================================
// generate_complex_collision
// ============================================================================
// `maxHullVerts` and `hullPrecision` were DECLARED here and read nowhere - no Ctx.GetInt, no
// Ctx.GetNumber, no local, no echo. They are gone rather than wired, and the reason is that there
// is nothing on this engine call to wire them to.
//
// Both names come from the pre-Geometry-Script static-mesh convex decomposition panel, whose
// DoDecomp took (MaxHullCount, MaxHullVerts) and whose "Hull Precision" was a VOXEL COUNT in the
// 10,000-1,000,000 range - which is why the declaration's "default 100.0" corresponds to nothing.
// The call this verb actually makes takes FGeometryScriptCollisionFromMeshOptions
// (CollisionFunctions.h:44-99), and that struct has no per-hull vertex budget and no precision
// field. The nearest candidates are decoys and mapping onto them would invent behaviour:
// ConvexHullTargetFaceCount is a post-simplification FACE count, not a vertex cap, and
// ConvexDecompositionErrorTolerance is an error in cm, on which "100.0" would mean a tolerance
// a metre wide rather than high precision.
//
// Contract-change audit before removal: `grep -rn maxHullVerts|hullPrecision` over the whole
// repo returned these two declaration lines and nothing else - no test, no doc, no wiki source,
// no Python. The one automation test that touches this verb
// (PinWright.geometry.generate_complex_collision.MissingRequiredParam) dispatches an EMPTY
// payload and asserts MISSING_REQUIRED_PARAM on actorName, so it neither sends nor reads either
// name. Callers that passed them were silently ignored before and now get UNKNOWN_PARAMS from
// FRpcDispatcher::ValidateHandlerParams, which is the honest answer and the point of the change.
REGISTER_RPC_HANDLER("geometry.generate_complex_collision", "geometry", "Generate complex collision using convex decomposition",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("maxHullCount", "integer", "Maximum number of convex hulls (default 8, clamped to 1-64)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    const int32 RequestedMaxHullCount = Ctx.GetInt(TEXT("maxHullCount"), 8);
    constexpr int32 MaxComplexCollisionHullCount = 64;
    const int32 EffectiveMaxHullCount = FMath::Clamp(RequestedMaxHullCount, 1, MaxComplexCollisionHullCount);
    const bool bClamped = RequestedMaxHullCount != EffectiveMaxHullCount;

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    FGeometryScriptCollisionFromMeshOptions CollisionOptions;
    CollisionOptions.Method = EGeometryScriptCollisionGenerationMethod::ConvexHulls;
    CollisionOptions.MaxConvexHullsPerMesh = EffectiveMaxHullCount;
    CollisionOptions.bEmitTransaction = false;

    int32 ShapeCount = GeometryUtils::GenerateAndApplyCollision(Target.Mesh, Target.Component, CollisionOptions);

    // `hullCount` is the measured number of convex elements the component now owns. It is not
    // the requested budget: the decomposition can emit fewer hulls, and the other simple shape
    // types are included in ShapeCount but are not convex hulls.
    const UBodySetup* BodySetup = Target.Component ? Target.Component->GetBodySetup() : nullptr;
    const int32 MeasuredHullCount = BodySetup ? BodySetup->AggGeom.ConvexElems.Num() : 0;

    // Collision-only edit — see geometry.generate_collision.
    GeometryUtils::MarkGeometryActorModified(Target.Component, /*bNotifyMesh=*/false);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    Result->SetNumberField(TEXT("hullCount"), MeasuredHullCount);
    Result->SetNumberField(TEXT("shapeCount"), ShapeCount);
    Result->SetNumberField(TEXT("requestedMaxHullCount"), RequestedMaxHullCount);
    Result->SetNumberField(TEXT("effectiveMaxHullCount"), EffectiveMaxHullCount);
    Result->SetBoolField(TEXT("clamped"), bClamped);
    if (bClamped)
    {
        Result->SetNumberField(TEXT("limit"), MaxComplexCollisionHullCount);
        TArray<TSharedPtr<FJsonValue>> Warnings;
        Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("maxHullCount clamped from %d to %d (valid range 1-%d)"),
            RequestedMaxHullCount, EffectiveMaxHullCount, MaxComplexCollisionHullCount)));
        Result->SetArrayField(TEXT("warnings"), Warnings);
    }
    Result->SetStringField(TEXT("collisionType"), TEXT("convex_decomposition"));

    AddActorVerification(Result, Target.Actor);
    Result->SetStringField(TEXT("actorName"), ActorName);

    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// simplify_collision
// ============================================================================
REGISTER_RPC_HANDLER("geometry.simplify_collision", "geometry", "Simplify mesh and regenerate collision with fewer hulls",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("simplificationFactor", "number", "Simplification factor 0.0-1.0 (default 0.5)"),
        RPC_PARAM_OPT("targetHullCount", "integer", "Target number of convex hulls (default 4)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    double SimplificationFactor = Ctx.GetNumber(TEXT("simplificationFactor"), 0.5);
    int32 TargetHullCount = Ctx.GetInt(TEXT("targetHullCount"), 4);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    // Simplify the mesh first, then generate simpler collision.
    //
    // Left default-constructed on purpose. The two lines this replaced pinned Method to
    // StandardQEM - the engine's FIRST enumerator, not its default, which is AttributeAware
    // (MeshSimplifyFunctions.h) - and set bAllowSeamCollapse to the value it already had. The
    // same undocumented override sat in GeometryOps::SimplifyMesh and is gone from there too;
    // no comment, doc, test or board entry anywhere named a reason for either copy. This verb
    // publishes no simplification options, so a default-constructed struct IS its whole
    // vocabulary; if it ever grows one, take the fields from GeometryOps::FSimplifyMeshParams
    // rather than starting a third spelling.
    FGeometryScriptSimplifyMeshOptions SimplifyOptions;

    int32 CurrentTris = Target.Mesh->GetTriangleCount();
    int32 TargetTris = FMath::Max(4, static_cast<int32>(CurrentTris * SimplificationFactor));

    UGeometryScriptLibrary_MeshSimplifyFunctions::ApplySimplifyToTriangleCount(
        Target.Mesh, TargetTris, SimplifyOptions, nullptr);

    // Generate simplified collision
    FGeometryScriptCollisionFromMeshOptions CollisionOptions;
    CollisionOptions.Method = EGeometryScriptCollisionGenerationMethod::ConvexHulls;
    CollisionOptions.MaxConvexHullsPerMesh = FMath::Clamp(TargetHullCount, 1, 16);
    CollisionOptions.bEmitTransaction = false;

    int32 ShapeCount = GeometryUtils::GenerateAndApplyCollision(Target.Mesh, Target.Component, CollisionOptions);

    // Unlike the two collision-only verbs, this one also ran ApplySimplifyToTriangleCount
    // on the mesh above and never refreshed the renderer, so take the mesh notify too.
    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    Result->SetNumberField(TEXT("trianglesBefore"), CurrentTris);
    Result->SetNumberField(TEXT("trianglesAfter"), Target.Mesh->GetTriangleCount());
    Result->SetNumberField(TEXT("shapeCount"), ShapeCount);

    AddActorVerification(Result, Target.Actor);
    Result->SetStringField(TEXT("actorName"), ActorName);

    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// set_lod_settings
// ============================================================================
REGISTER_RPC_HANDLER("geometry.set_lod_settings", "geometry", "Configure LOD reduction settings for a specific LOD level",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the StaticMesh asset"),
        RPC_PARAM_OPT("lodIndex", "integer", "LOD index to configure (default 1)"),
        RPC_PARAM_OPT("trianglePercent", "number", "Triangle reduction percentage (default 50.0)"),
        RPC_PARAM_OPT("recomputeNormals", "boolean", "Recompute normals for this LOD (default false)"),
        RPC_PARAM_OPT("recomputeTangents", "boolean", "Recompute tangents for this LOD (default false)")
    ))
{
    FString AssetPath = Ctx.GetString(TEXT("assetPath"));
    int32 LODIndex = Ctx.GetInt(TEXT("lodIndex"), 1);
    double TrianglePercent = Ctx.GetNumber(TEXT("trianglePercent"), 50.0);
    bool bRecomputeNormals = Ctx.GetBool(TEXT("recomputeNormals"), false);
    bool bRecomputeTangents = Ctx.GetBool(TEXT("recomputeTangents"), false);

    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("assetPath required"));
        return true;
    }

    FString SafePath = SanitizeProjectRelativePath(AssetPath);
    if (SafePath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ASSET_PATH"), FString::Printf(TEXT("Invalid asset path: %s"), *AssetPath));
        return true;
    }

    UStaticMesh* StaticMesh = LoadObject<UStaticMesh>(nullptr, *SafePath);
    if (!StaticMesh)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), FString::Printf(TEXT("StaticMesh not found: %s"), *SafePath));
        return true;
    }

    if (LODIndex < 0 || LODIndex >= StaticMesh->GetNumSourceModels())
    {
        Ctx.SendError(TEXT("INVALID_LOD_INDEX"), FString::Printf(TEXT("Invalid LOD index: %d (mesh has %d LODs)"), LODIndex, StaticMesh->GetNumSourceModels()));
        return true;
    }

    TArray<FString> RebuildPaths;
    RebuildPaths.Add(SafePath);
    return PinWrightMeshRebuild::RunGuardedStaticMeshRebuild(Ctx,
        TEXT("geometry.set_lod_settings"), RebuildPaths,
        [SafePath, LODIndex, TrianglePercent, bRecomputeNormals, bRecomputeTangents](
            const PinWrightSafePoint::FSafePointResponder& Responder,
            const TArray<UStaticMesh*>& Meshes)
    {
        if (Meshes.Num() == 0)
        {
            Responder.SendError(TEXT("ASSET_NOT_FOUND"),
                FString::Printf(TEXT("StaticMesh not found: %s"), *SafePath));
            return;
        }

        UStaticMesh* StaticMesh = Meshes[0];
        StaticMesh->Modify();

        FStaticMeshSourceModel& SourceModel = StaticMesh->GetSourceModel(LODIndex);

        SourceModel.ReductionSettings.PercentTriangles = TrianglePercent / 100.0f;
        SourceModel.ReductionSettings.PercentVertices = TrianglePercent / 100.0f;

        SourceModel.BuildSettings.bRecomputeNormals = bRecomputeNormals;
        SourceModel.BuildSettings.bRecomputeTangents = bRecomputeTangents;

        StaticMesh->Build();
        StaticMesh->PostEditChange();
        McpSafeAssetSave(StaticMesh);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("assetPath"), SafePath);
        Result->SetNumberField(TEXT("lodIndex"), LODIndex);
        Result->SetNumberField(TEXT("trianglePercent"), TrianglePercent);

        AddAssetVerification(Result, StaticMesh);

        Responder.SendSuccess(Result);
    });
}
