// Copyright (c) 2026 Alexander Penkin. MIT License.

// BooleanHandler.cpp - Boolean operations: union, subtract, intersection, trim, self_union (Phase 19)
#include "Handlers/HandlerRegistration.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Geometry/GeometryOpWarnings.h"
#include "Handlers/Geometry/GeometryOps_Boolean.h"
#include "Handlers/Geometry/GeometryTarget.h"
#include "Handlers/Geometry/GeometryUtils.h"
#include "Compat/EngineVersionCompat.h"
#include "Dom/JsonObject.h"


#include "Components/DynamicMeshComponent.h"
// Required even though this file never spells ADynamicMeshActor: FGeometryTarget::Actor is one,
// and the boolean verbs call GetActorTransform()/Destroy() through it, which needs the complete
// type. EngineUtils.h is NOT required - the TActorIterator scan moved into GeometryTarget - and
// neither is the GeometryScriptTypes __has_include block, since no FGeometryScript* type is
// named here; the operation selector is the enum from MeshBooleanFunctions.h below.
#include "DynamicMeshActor.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "UDynamicMesh.h"
// GEditor, for the one verb below that resolves its two actors itself rather than through
// GeometryTarget::ResolveOrSendError.
#include "Editor.h"

#include "GeometryScript/MeshBooleanFunctions.h"


// The shared body behind union / subtract / intersection.
namespace
{
    // The four FGeometryScriptMeshBooleanOptions parameters these verbs publish, read once for
    // every verb that publishes them - the struct's fifth, SimplifyPlanarTolerance, is not one
    // of them; see below. Uniquely prefixed for the Unity-merge reason; templated because
    // FBooleanParams and FTrimParams are deliberately separate types - they carried DIFFERENT
    // bSimplifyOutput defaults (false vs true) until the boolean side was flipped to the
    // engine's true, and staying templated is what keeps the next divergence from silently
    // imposing one surface's default on the other.
    //
    // Absent leaves the field alone, so every default stays in the params struct and none is
    // re-typed here. An unrecognized outputTransformSpace is rejected rather than ignored; the
    // vocabulary is new, so no shipped behaviour depends on it being silently dropped.
    //
    // Returns false having already sent the error, matching the Require* idiom.
    template <typename ParamsT>
    bool BooleanHandler_ReadBooleanOptions(const FHandlerContext& Ctx, ParamsT& Params)
    {
        Params.bFillHoles = Ctx.GetBool(TEXT("fillHoles"), Params.bFillHoles);
        Params.bSimplifyOutput = Ctx.GetBool(TEXT("simplifyOutput"), Params.bSimplifyOutput);
        // simplifyPlanarTolerance is deliberately NOT read here: UE 5.8's ApplyMeshBoolean
        // never forwards it to the operation (MeshBooleanFunctions.cpp:87 assigns
        // bSimplifyOutput and nothing else), so accepting it would publish a knob that does
        // nothing. geometry.self_union DOES publish its own, because ApplyMeshSelfUnion
        // forwards it (:161-162). See GeometryOps::FBooleanParams.
        Params.bAllowEmptyResult = Ctx.GetBool(TEXT("allowEmptyResult"), Params.bAllowEmptyResult);

        const FString Space = Ctx.GetString(TEXT("outputTransformSpace"));
        if (Space.IsEmpty())
        {
            return true;
        }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        if (Space.Equals(TEXT("target"), ESearchCase::IgnoreCase))
        {
            Params.OutputTransformSpace = EGeometryScriptBooleanOutputSpace::TargetTransformSpace;
            return true;
        }
        if (Space.Equals(TEXT("tool"), ESearchCase::IgnoreCase))
        {
            Params.OutputTransformSpace = EGeometryScriptBooleanOutputSpace::ToolTransformSpace;
            return true;
        }
        if (Space.Equals(TEXT("shared"), ESearchCase::IgnoreCase))
        {
            Params.OutputTransformSpace = EGeometryScriptBooleanOutputSpace::SharedTransformSpace;
            return true;
        }
#else
        // Pre-5.6 ApplyMeshBoolean has no output-space selector: it always inverts the TARGET
        // transform out of the result, so `target` is exactly what the engine already does and
        // is accepted as a no-op. The other two are REFUSED rather than served as `target`,
        // because silently writing the result into the wrong frame is the teleporting-mesh
        // failure this parameter exists to prevent.
        if (Space.Equals(TEXT("target"), ESearchCase::IgnoreCase))
        {
            return true;
        }
        if (Space.Equals(TEXT("tool"), ESearchCase::IgnoreCase)
            || Space.Equals(TEXT("shared"), ESearchCase::IgnoreCase))
        {
            Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_ENGINE_VERSION, FString::Printf(TEXT(
                "outputTransformSpace '%s' needs FGeometryScriptMeshBooleanOptions::"
                "OutputTransformSpace, added in UE 5.6. This engine always writes the boolean "
                "result into the target's transform space; pass 'target' or omit the parameter."),
                *Space));
            return false;
        }
#endif

        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Unknown outputTransformSpace: %s. Use: target, tool, shared"), *Space));
        return false;
    }

    bool HandleBooleanOperationImpl(const FHandlerContext& Ctx, EGeometryScriptBooleanOperation BoolOp)
    {
        FString TargetActorName = Ctx.GetString(TEXT("targetActor"));
        FString ToolActorName = Ctx.GetString(TEXT("toolActor"));
        bool bKeepTool = Ctx.GetBool(TEXT("keepTool"), true);

        if (TargetActorName.IsEmpty() || ToolActorName.IsEmpty())
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("targetActor and toolActor required"));
            return true;
        }

        // Role puts "Target"/"Tool" in the ACTOR_NOT_FOUND message, and this pair is the one
        // call site that reports a missing component under its own code rather than folding it
        // into MESH_NOT_FOUND.
        GeometryTarget::FResolveOptions ResolveOptions;
        ResolveOptions.bReportMissingComponent = true;

        FGeometryTarget Target, Tool;
        ResolveOptions.Role = TEXT("Target");
        if (!GeometryTarget::ResolveOrSendError(Ctx, TargetActorName, Target, ResolveOptions))
            return true;
        ResolveOptions.Role = TEXT("Tool");
        if (!GeometryTarget::ResolveOrSendError(Ctx, ToolActorName, Tool, ResolveOptions))
            return true;

        // The tool mesh is read, never written, so its count is the wrapper's own echo rather
        // than half of the op's before/after pair. Read before the op for the same reason the
        // op captures its own: the boolean rewrites the target in place.
        const int32 ToolTriCount = Tool.Mesh->GetTriangleCount();

        // Defaults are the struct's. The reader only overwrites a field the request actually
        // names, so omitting all of them gives the engine's own settings - including
        // bSimplifyOutput=true, which is a CHANGE: these verbs used to pin it false with no way
        // to reach it. Pass simplifyOutput=false to reproduce the old triangulation.
        GeometryOps::FBooleanParams Params;
        if (!BooleanHandler_ReadBooleanOptions(Ctx, Params))
        {
            return true;
        }

        const GeometryOps::FOpResult Op = GeometryOps::Boolean(
            Target.Mesh,
            Target.Actor->GetActorTransform(),
            Tool.Mesh,
            Tool.Actor->GetActorTransform(),
            BoolOp,
            Params);

        // BOOLEAN_FAILED is the only failure that reaches past this point, because it is the
        // only one with a response body to fill in below; the two pre-flight guards have always
        // returned here, leaving the tool actor alone.
        if (!Op.bSuccess && Op.ErrorCode != ErrorCodes::ERR_BOOLEAN_FAILED)
        {
            Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
            return true;
        }

        // The four boolean verbs previously had NO commit step at all — not even
        // NotifyMeshUpdated() — so the target's mesh was mutated in place while both the
        // renderer and the dirty flag were left stale. This is the commit for all four.
        if (Op.bSuccess)
        {
            GeometryUtils::MarkGeometryActorModified(Target.Component);
        }

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("targetActor"), TargetActorName);
        GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor, TEXT("target"));
        GeometryUtils::AddResolvedActorIdentity(Result, Tool.Actor, TEXT("tool"));

        // Optionally delete tool actor. UWorld::DestroyActor already calls
        // ThisActor->MarkPackageDirty() unconditionally (LevelActor.cpp:1056), so the
        // tool's removal needs no ceremony added here.
        //
        // GATED ON SUCCESS, and that gate is load-bearing now in a way it was not when this
        // line was written. Destroying the tool is a COMMIT: it is only correct once the tool's
        // volume is actually in the target. This ran unconditionally because BOOLEAN_FAILED was
        // structurally unreachable - GeometryOps::Boolean gated it on a null return that
        // ApplyMeshBoolean never produces - so the "failed but past the guard" case did not
        // exist. It exists now that the op reads the engine's UGeometryScriptDebug channel
        // (Handlers/Geometry/GeometryScriptDebugSink.h), and ungated this would delete the tool
        // actor for a boolean that changed nothing and then report BOOLEAN_FAILED: the caller
        // loses the cutter, gains no cut, and cannot retry without rebuilding it.
        if (Op.bSuccess && !bKeepTool)
        {
            Tool.Actor->Destroy();
        }

        Result->SetStringField(TEXT("operation"), GeometryOps::BooleanOperationName(BoolOp));
        Result->SetBoolField(TEXT("success"), Op.bSuccess);
        Result->SetNumberField(TEXT("targetTriangles"), Op.TrianglesBefore);
        Result->SetNumberField(TEXT("toolTriangles"), ToolTriCount);
        if (Op.bSuccess)
        {
            Result->SetNumberField(TEXT("resultTriangles"), Op.TrianglesAfter);

            // No-op detection (secondary hardening): the engine's ApplyMeshBoolean
            // returns the target mesh UNCHANGED (same pointer, same triangle count)
            // when the two meshes do not overlap in their resolved common space, so
            // success==true alone cannot tell a real cut from a no-effect pass.
            // Surface changed=false so a caller is not misled into running downstream
            // bevel/collision/convert on geometry that was never actually modified.
            // (The primary positioning bug that made overlapping tools miss is fixed
            // in PrimitiveHandler.cpp's identity-mesh spawn convention; this flag
            // covers the genuinely-disjoint case.)
            Result->SetBoolField(TEXT("changed"), Op.bChanged);

            GeometryOps::AddOpSkinWeights(Result, Op);
            GeometryOps::AddOpWarnings(Result, Op);
            Ctx.SendSuccess(Result);
        }
        else
        {
            Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        }

        return true;
    }
}


// ============================================================================
// boolean_union
// ============================================================================
REGISTER_RPC_HANDLER("geometry.boolean_union", "geometry", "Perform boolean union of two dynamic mesh actors",
    RPC_PARAMS(
        RPC_PARAM_REQ("targetActor", "string", "Name of the target DynamicMeshActor"),
        RPC_PARAM_REQ("toolActor", "string", "Name of the tool DynamicMeshActor"),
        RPC_PARAM_OPT("keepTool", "boolean", "Keep the tool actor after operation (default true)"),
        RPC_PARAM_OPT("fillHoles", "boolean", "Fill the holes the cut opens (default true)"),
        RPC_PARAM_OPT("simplifyOutput", "boolean", "Collapse the small coplanar triangles the boolean generates along the new cut edges. Defaults TRUE - the engine default. It only touches triangles the CUT created, at 0.1 degrees from coplanar, and cannot distort polygroups, UVs or normals. Pass false to keep every triangle the cut produced"),
        RPC_PARAM_OPT("allowEmptyResult", "boolean", "Let the boolean produce an empty mesh instead of failing with BOOLEAN_FAILED (default false)"),
        RPC_PARAM_OPT("outputTransformSpace", "string", "Which operand's local space the result lands in: target (default), tool, or shared. Anything but target leaves the mesh in a frame the target actor's own transform does not correct for")
    ))
{
    return HandleBooleanOperationImpl(Ctx, EGeometryScriptBooleanOperation::Union);
}

// ============================================================================
// boolean_subtract
// ============================================================================
REGISTER_RPC_HANDLER("geometry.boolean_subtract", "geometry", "Perform boolean subtraction of two dynamic mesh actors",
    RPC_PARAMS(
        RPC_PARAM_REQ("targetActor", "string", "Name of the target DynamicMeshActor"),
        RPC_PARAM_REQ("toolActor", "string", "Name of the tool DynamicMeshActor"),
        RPC_PARAM_OPT("keepTool", "boolean", "Keep the tool actor after operation (default true)"),
        RPC_PARAM_OPT("fillHoles", "boolean", "Fill the holes the cut opens (default true)"),
        RPC_PARAM_OPT("simplifyOutput", "boolean", "Collapse the small coplanar triangles the boolean generates along the new cut edges. Defaults TRUE - the engine default. It only touches triangles the CUT created, at 0.1 degrees from coplanar, and cannot distort polygroups, UVs or normals. Pass false to keep every triangle the cut produced"),
        RPC_PARAM_OPT("allowEmptyResult", "boolean", "Let the boolean produce an empty mesh instead of failing with BOOLEAN_FAILED (default false)"),
        RPC_PARAM_OPT("outputTransformSpace", "string", "Which operand's local space the result lands in: target (default), tool, or shared. Anything but target leaves the mesh in a frame the target actor's own transform does not correct for")
    ))
{
    return HandleBooleanOperationImpl(Ctx, EGeometryScriptBooleanOperation::Subtract);
}

// ============================================================================
// boolean_intersection
// ============================================================================
REGISTER_RPC_HANDLER("geometry.boolean_intersection", "geometry", "Perform boolean intersection of two dynamic mesh actors",
    RPC_PARAMS(
        RPC_PARAM_REQ("targetActor", "string", "Name of the target DynamicMeshActor"),
        RPC_PARAM_REQ("toolActor", "string", "Name of the tool DynamicMeshActor"),
        RPC_PARAM_OPT("keepTool", "boolean", "Keep the tool actor after operation (default true)"),
        RPC_PARAM_OPT("fillHoles", "boolean", "Fill the holes the cut opens (default true)"),
        RPC_PARAM_OPT("simplifyOutput", "boolean", "Collapse the small coplanar triangles the boolean generates along the new cut edges. Defaults TRUE - the engine default. It only touches triangles the CUT created, at 0.1 degrees from coplanar, and cannot distort polygroups, UVs or normals. Pass false to keep every triangle the cut produced"),
        RPC_PARAM_OPT("allowEmptyResult", "boolean", "Let the boolean produce an empty mesh instead of failing with BOOLEAN_FAILED (default false)"),
        RPC_PARAM_OPT("outputTransformSpace", "string", "Which operand's local space the result lands in: target (default), tool, or shared. Anything but target leaves the mesh in a frame the target actor's own transform does not correct for")
    ))
{
    return HandleBooleanOperationImpl(Ctx, EGeometryScriptBooleanOperation::Intersection);
}

// ============================================================================
// difference (alias for boolean_subtract)
// ============================================================================
REGISTER_RPC_HANDLER("geometry.difference", "geometry", "Perform boolean subtraction (alias for boolean_subtract)",
    RPC_PARAMS(
        RPC_PARAM_REQ("targetActor", "string", "Name of the target DynamicMeshActor"),
        RPC_PARAM_REQ("toolActor", "string", "Name of the tool DynamicMeshActor"),
        RPC_PARAM_OPT("keepTool", "boolean", "Keep the tool actor after operation (default true)"),
        RPC_PARAM_OPT("fillHoles", "boolean", "Fill the holes the cut opens (default true)"),
        RPC_PARAM_OPT("simplifyOutput", "boolean", "Collapse the small coplanar triangles the boolean generates along the new cut edges. Defaults TRUE - the engine default. It only touches triangles the CUT created, at 0.1 degrees from coplanar, and cannot distort polygroups, UVs or normals. Pass false to keep every triangle the cut produced"),
        RPC_PARAM_OPT("allowEmptyResult", "boolean", "Let the boolean produce an empty mesh instead of failing with BOOLEAN_FAILED (default false)"),
        RPC_PARAM_OPT("outputTransformSpace", "string", "Which operand's local space the result lands in: target (default), tool, or shared. Anything but target leaves the mesh in a frame the target actor's own transform does not correct for")
    ))
{
    return HandleBooleanOperationImpl(Ctx, EGeometryScriptBooleanOperation::Subtract);
}

// ============================================================================
// boolean_trim
// ============================================================================
REGISTER_RPC_HANDLER("geometry.boolean_trim", "geometry", "Trim a mesh using another mesh as a cutting tool. The response carries no vertex/triangle count by design; a trim the engine refused is reported as BOOLEAN_FAILED rather than as a success.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the target DynamicMeshActor"),
        RPC_PARAM_REQ("trimActorName", "string", "Name of the trim tool DynamicMeshActor"),
        RPC_PARAM_OPT("keepInside", "boolean", "Keep inside portion (intersection) vs outside (subtraction) (default false)"),
        RPC_PARAM_OPT("fillHoles", "boolean", "Fill the holes the trim opens (default true)"),
        RPC_PARAM_OPT("simplifyOutput", "boolean", "Collapse the small coplanar triangles the trim generates along the new cut edges. Defaults TRUE - the engine default, which this verb has always run and geometry.boolean_* now runs too"),
        RPC_PARAM_OPT("allowEmptyResult", "boolean", "Let the trim produce an empty mesh instead of failing with BOOLEAN_FAILED (default false)"),
        RPC_PARAM_OPT("outputTransformSpace", "string", "Which operand's local space the result lands in: target (default), tool, or shared")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    FString TrimActorName = Ctx.GetString(TEXT("trimActorName"));
    bool bKeepInside = Ctx.GetBool(TEXT("keepInside"), false);

    if (ActorName.IsEmpty() || TrimActorName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("actorName and trimActorName required"));
        return true;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Ctx.SendError(TEXT("NO_WORLD"), TEXT("No world available"));
        return true;
    }

    // Nullable probes rather than ResolveOrSendError: this verb reports both lookups in one
    // message, which the resolve-or-error form cannot express.
    FGeometryTarget Target, Trim;
    McpActorUtils::FActorResolution TargetResolution;
    McpActorUtils::FActorResolution TrimResolution;
    GeometryTarget::Find(World, ActorName, Target, &TargetResolution);
    GeometryTarget::Find(World, TrimActorName, Trim, &TrimResolution);

    if (TargetResolution.IsAmbiguous())
    {
        ActorNameParamUtils::SendAmbiguousActorError(Ctx, ActorName, TargetResolution);
        return true;
    }

    if (!Target.Actor)
    {
        Ctx.SendError(TEXT("ACTOR_NOT_FOUND"), TEXT("One or both actors not found"));
        return true;
    }

    if (TrimResolution.IsAmbiguous())
    {
        ActorNameParamUtils::SendAmbiguousActorError(Ctx, TrimActorName, TrimResolution);
        return true;
    }

    if (!Target.Actor || !Trim.Actor)
    {
        Ctx.SendError(TEXT("ACTOR_NOT_FOUND"), TEXT("One or both actors not found"));
        return true;
    }

    if (!Target.Mesh || !Trim.Mesh)
    {
        Ctx.SendError(TEXT("MESH_NOT_FOUND"), TEXT("DynamicMesh not available on one or both actors"));
        return true;
    }

    GeometryOps::FTrimParams Params;
    Params.bKeepInside = bKeepInside;
    // Same reader as the four boolean verbs. The two structs' defaults now agree on
    // bSimplifyOutput (both the engine's true); the reader stays templated because they are
    // still separate types and keep_inside exists on only one of them.
    if (!BooleanHandler_ReadBooleanOptions(Ctx, Params))
    {
        return true;
    }

    // The op's COUNTS are still deliberately dropped: this verb has never echoed a vertex or
    // triangle count, and adding one is a widening this change does not make. The op reports
    // them; the .pwmodel compiler is what consumes them.
    //
    // Its BOOLEAN_FAILED is no longer dropped. That used to be justified as "an observable wire
    // change rather than the extraction it is part of", which conflates two unlike silences: an
    // unpublished count withholds something the caller was never promised, while a success
    // response for a trim the engine refused asserts something FALSE - and the caller then keeps
    // building on geometry that still has the tool volume in it. ApplyMeshBoolean returns null
    // when the operation produces empty geometry, which GeometryOps::Trim reports as
    // ERR_BOOLEAN_FAILED with the same text geometry.boolean_* uses. Honouring it IS an
    // observable change on the failure path, which is the point; the success path is unchanged,
    // count-less response included.
    //
    // Its WARNINGS are not dropped either. Discarding the whole result is how a clamp ends up
    // invisible to every caller: the value the wrapper kept dropping is a note about what the
    // engine actually did, not a count this verb chose not to publish. Trim emits none today;
    // capturing the result is what stops the next warning added to it from being lost here.
    const GeometryOps::FOpResult Op = GeometryOps::Trim(
        Target.Mesh,
        Target.Actor->GetActorTransform(),
        Trim.Mesh,
        Trim.Actor->GetActorTransform(),
        Params);

    // Notify BEFORE the failure check. A boolean that fails partway can still have touched the
    // target mesh, and a component left un-notified would keep rendering geometry the mesh no
    // longer holds - a worse outcome than an extra notify on an unchanged mesh.
    GeometryUtils::MarkGeometryActorModified(Target.Component);

    if (!Op.bSuccess)
    {
        Ctx.SendError(Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    Result->SetStringField(TEXT("trimActorName"), TrimActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor, TEXT("target"));
    GeometryUtils::AddResolvedActorIdentity(Result, Trim.Actor, TEXT("trim"));
    Result->SetBoolField(TEXT("keepInside"), bKeepInside);
    GeometryOps::AddOpSkinWeights(Result, Op);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// self_union
// ============================================================================
REGISTER_RPC_HANDLER("geometry.self_union", "geometry", "Apply self-union to resolve self-intersections in a mesh",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("fillHoles", "boolean", "Fill holes during self-union (default true)"),
        RPC_PARAM_OPT("trimFlaps", "boolean", "Drop the open half-faces a self-intersection leaves bounding nothing (default true). Off keeps them, which is what a SURFACE model wants - there the flaps are the geometry, not debris."),
        RPC_PARAM_OPT("simplifyOutput", "boolean", "Collapse coplanar triangles in the result (default true). On here, unlike the four boolean_* verbs, which is the engine's own default and what this verb has always run."),
        RPC_PARAM_OPT("simplifyPlanarTolerance", "number", "Coplanarity tolerance for the simplification pass (default 0.01); ignored when simplifyOutput is false"),
        RPC_PARAM_OPT("windingThreshold", "number", "Winding-number isovalue separating inside from outside (default 0.5). 0.5 keeps anything inside at least one shell; raising it toward 1.5 keeps only what is covered twice, turning the resolve into an intersection.")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    bool bFillHoles = Ctx.GetBool(TEXT("fillHoles"), true);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    GeometryOps::FSelfUnionParams Params;
    Params.bFillHoles = bFillHoles;
    Params.bTrimFlaps = Ctx.GetBool(TEXT("trimFlaps"), Params.bTrimFlaps);
    Params.bSimplifyOutput = Ctx.GetBool(TEXT("simplifyOutput"), Params.bSimplifyOutput);
    Params.SimplifyPlanarTolerance =
        Ctx.GetNumber(TEXT("simplifyPlanarTolerance"), Params.SimplifyPlanarTolerance);
    Params.WindingThreshold = Ctx.GetNumber(TEXT("windingThreshold"), Params.WindingThreshold);

    const GeometryOps::FOpResult Op = GeometryOps::SelfUnion(Target.Mesh, Params);

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("trianglesBefore"), Op.TrianglesBefore);
    Result->SetNumberField(TEXT("trianglesAfter"), Op.TrianglesAfter);
    GeometryOps::AddOpSkinWeights(Result, Op);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(Result);
    return true;
}
