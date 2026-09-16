// Copyright (c) 2026 Alexander Penkin. MIT License.

// GeometryOps_Boolean.cpp - see GeometryOps_Boolean.h for the shape and for what stayed behind
// in BooleanHandler.cpp / GeometryTransformHandler.cpp.
//
// The null guard and the before/after count snapshot are GeometryOps.h's, shared with the other
// four families: this file no longer carries a private copy of either. Only the three helpers
// below are genuinely boolean-specific.
//
// Failure is IN-PLACE throughout (FOpResult::FailIn). FOpResult::Fail builds a fresh struct, so
// a `return FOpResult::Fail(...)` taken after the before-counts were recorded silently discards
// them - which is why five sites in this file used to work around the clobber by hand, one by
// saving and re-applying its two counts around the call and four by re-filling counts onto the
// failure object afterwards. Fail() is now used only where nothing has accumulated yet.
#include "Handlers/Geometry/GeometryOps_Boolean.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Geometry/GeometryScriptDebugSink.h"
#include "Handlers/Geometry/GeometrySkinWeightRepair.h"
#include "Handlers/Geometry/GeometryUtils.h"
#include "Compat/EngineVersionCompat.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "UDynamicMesh.h"

#if __has_include("GeometryScript/GeometryScriptTypes.h")
#include "GeometryScript/GeometryScriptTypes.h"
#else
#include "GeometryScriptTypes.h"
#endif

#include "GeometryScript/MeshBasicEditFunctions.h"
#include "GeometryScript/MeshBooleanFunctions.h"
#include "GeometryScript/MeshRepairFunctions.h"
#include "GeometryScript/MeshTransformFunctions.h"

namespace
{
    // Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace
    // helper with a common name would collide with a sibling TU once Unity merges them.

    // Depth of the test-only memory-pressure override - see FScopedBooleanMemoryPressure in the
    // header. A counter rather than a bool so nested scopes cannot lift each other's override.
    int32 GeometryOpsBoolean_ForcedMemoryPressureDepth = 0;

    // A unit scale on every axis is what an unrecognized axis string resolves to in
    // geometry.mirror, which is why EMeshAxis::None maps here rather than being rejected.
    FVector GeometryOpsBoolean_MirrorScaleForAxis(GeometryOps::EMeshAxis Axis)
    {
        FVector Scale = FVector::OneVector;
        switch (Axis)
        {
        case GeometryOps::EMeshAxis::X: Scale.X = -1.0; break;
        case GeometryOps::EMeshAxis::Y: Scale.Y = -1.0; break;
        case GeometryOps::EMeshAxis::Z: Scale.Z = -1.0; break;
        default: break;
        }
        return Scale;
    }

    // Z for None: geometry.array_radial's if/else chain leaves the axis at its UpVector default
    // for anything it does not recognize, including its own documented "Z".
    FVector GeometryOpsBoolean_RotationAxisFor(GeometryOps::EMeshAxis Axis)
    {
        switch (Axis)
        {
        case GeometryOps::EMeshAxis::X: return FVector::ForwardVector;
        case GeometryOps::EMeshAxis::Y: return FVector::RightVector;
        default: return FVector::UpVector;
        }
    }

    // The shared pre-flight for the two in-place array multiplies. Identical message text in
    // both verbs today, so it is one function rather than two copies.
    //
    // Fails IN PLACE on the caller's result rather than returning a fresh one: both call sites
    // run it after BeginOp has recorded the before-counts, and returning a fresh failure there
    // dropped them - the clobber both verbs used to patch up by re-filling the counts onto the
    // returned object.
    bool GeometryOpsBoolean_CheckArrayBudget(
        UDynamicMesh* Target, int32 Count, GeometryOps::FOpResult& Result)
    {
        if (!GeometryOps::BooleanMemoryPressureSafe())
        {
            GeometryOps::FOpResult::FailIn(Result, ErrorCodes::ERR_MEMORY_PRESSURE,
                FString::Printf(TEXT("Memory pressure too high (%.1f%% used). Array operation blocked to prevent OOM."),
                               GeometryUtils::GetMemoryUsagePercent()));
            return false;
        }

        const int32 TriCountBefore = Target->GetTriangleCount();
        const int64 EstimatedTriangles = static_cast<int64>(TriCountBefore) * Count;

        if (EstimatedTriangles > GEOM_MAX_TRIANGLES_PER_DYNAMIC_MESH)
        {
            GeometryOps::FOpResult::FailIn(Result, ErrorCodes::ERR_POLYGON_LIMIT_EXCEEDED,
                FString::Printf(TEXT("Array would exceed triangle limit. Current: %d, Estimated: %lld, Max: %d"),
                               TriCountBefore, EstimatedTriangles, GEOM_MAX_TRIANGLES_PER_DYNAMIC_MESH));
            return false;
        }

        return true;
    }

    // Runs the skin-weight repair over a mesh an engine boolean has just rewritten, and records
    // what it did on the result. A no-op costing one null check when the snapshot found no skin
    // weights, which is every unskinned call.
    //
    // Why every boolean in this file needs it and not just subtract: the defect is
    // FMeshBoolean deleting vertices and then re-using their IDs for appended geometry, which
    // hands the new vertices the deleted vertices' bone weights (see GeometrySkinWeightRepair.h).
    // Union, intersection and trim all reach the same ApplyMeshBoolean, and self-union reaches
    // ApplyMeshSelfUnion, which resolves a mesh against itself the same way. Fixing only the verb
    // the defect was reported through would leave four verbs corrupting skinning silently.
    void GeometryOpsBoolean_RepairSkinWeights(
        const GeometrySkinWeights::FPreEditSnapshot& Snapshot,
        UDynamicMesh* Target,
        GeometryOps::FOpResult& Result)
    {
        const GeometrySkinWeights::FRepairReport Report = Snapshot.Repair(Target);
        Result.bSkinned = Report.bSkinned;
        Result.SkinWeightsTransferred = Report.VerticesTransferred;
        Result.SkinWeightsUnresolved = Report.VerticesUnresolved;

        // A warning rather than a failure: the geometry is what the caller asked for and the
        // unresolved vertices keep whatever the engine left on them. What must not happen is the
        // op reporting a clean success over vertices it knows it could not account for.
        if (Report.VerticesUnresolved > 0)
        {
            Result.Warnings.Add(FString::Printf(TEXT(
                "%d vertices could not be re-weighted from the pre-operation surface and keep "
                "whatever influences the engine left on them. Audit the mesh with "
                "skeleton.audit_skin_weights before baking it to a SkeletalMesh."),
                Report.VerticesUnresolved));
        }
    }
}

namespace GeometryOps
{

bool BooleanMemoryPressureSafe()
{
    if (GeometryOpsBoolean_ForcedMemoryPressureDepth > 0)
    {
        return false;
    }
    return GeometryUtils::IsMemoryPressureSafe();
}

FScopedBooleanMemoryPressure::FScopedBooleanMemoryPressure()
{
    ++GeometryOpsBoolean_ForcedMemoryPressureDepth;
}

FScopedBooleanMemoryPressure::~FScopedBooleanMemoryPressure()
{
    --GeometryOpsBoolean_ForcedMemoryPressureDepth;
}

const TCHAR* BooleanOperationName(EGeometryScriptBooleanOperation Operation)
{
    switch (Operation)
    {
    case EGeometryScriptBooleanOperation::Union:                return TEXT("Union");
    case EGeometryScriptBooleanOperation::Intersection:         return TEXT("Intersection");
    case EGeometryScriptBooleanOperation::Subtract:             return TEXT("Subtract");
    // The four trim/polygroup operations joined EGeometryScriptBooleanOperation in UE 5.6.
    // Before that the enum has exactly three enumerators and naming the others is a hard error.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    case EGeometryScriptBooleanOperation::TrimInside:           return TEXT("TrimInside");
    case EGeometryScriptBooleanOperation::TrimOutside:          return TEXT("TrimOutside");
    case EGeometryScriptBooleanOperation::NewPolyGroupInside:   return TEXT("NewPolyGroupInside");
    case EGeometryScriptBooleanOperation::NewPolyGroupOutside:  return TEXT("NewPolyGroupOutside");
#endif
    }
    return TEXT("Unknown");
}

FOpResult Boolean(
    UDynamicMesh* Target,
    const FTransform& TargetTransform,
    UDynamicMesh* Tool,
    const FTransform& ToolTransform,
    EGeometryScriptBooleanOperation Operation,
    const FBooleanParams& Params)
{
    const TCHAR* OpName = BooleanOperationName(Operation);

    // Both handles are checked at once, which is why this is CaptureBefore rather than BeginOp:
    // BeginOp guards a single mesh. Fail() is correct here and only here - nothing has been
    // recorded on Result yet, so there is nothing for the fresh struct to discard.
    FOpResult Result;
    if (!Target || !Tool)
    {
        return NullMeshFailure();
    }

    // Captured before anything runs: ApplyMeshBoolean mutates Target in place, so the "before"
    // half of the result cannot be read back afterwards.
    CaptureBefore(Target, Result);

    const int32 TargetTriCount = Result.TrianglesBefore;
    const int32 ToolTriCount = Tool->GetTriangleCount();
    const int64 EstimatedMaxTriangles = static_cast<int64>(TargetTriCount) + static_cast<int64>(ToolTriCount);

    // Both guards run before either mesh is touched, so a caller that hits one is left with
    // exactly the geometry it had - and with the before-counts that prove it, which is what
    // failing in place preserves.
    if (!BooleanMemoryPressureSafe())
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_MEMORY_PRESSURE,
            FString::Printf(TEXT("Memory pressure too high (%.1f%% used). Boolean %s blocked to prevent OOM."),
                GeometryUtils::GetMemoryUsagePercent(), OpName));
    }

    const int64 EstimatedWithSafetyMargin = EstimatedMaxTriangles * 3;
    if (EstimatedWithSafetyMargin > GEOM_MAX_TRIANGLES_PER_DYNAMIC_MESH)
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_POLYGON_LIMIT_EXCEEDED,
            FString::Printf(TEXT("Boolean %s would exceed polygon limit. Target: %d, Tool: %d, Estimated max: %lld, Limit: %d"),
                OpName, TargetTriCount, ToolTriCount, EstimatedWithSafetyMargin, GEOM_MAX_TRIANGLES_PER_DYNAMIC_MESH));
    }

    FGeometryScriptMeshBooleanOptions BoolOptions;
    BoolOptions.bFillHoles = Params.bFillHoles;
    BoolOptions.bSimplifyOutput = Params.bSimplifyOutput;
    BoolOptions.SimplifyPlanarTolerance = static_cast<float>(Params.SimplifyPlanarTolerance);
    // FGeometryScriptMeshBooleanOptions::bAllowEmptyResult arrived in UE 5.4. On 5.3 the engine
    // unconditionally refuses an empty result, which is exactly the default (false). A caller who
    // asked for true is refused by name rather than served the opposite behaviour under a success.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    BoolOptions.bAllowEmptyResult = Params.bAllowEmptyResult;
#else
    if (Params.bAllowEmptyResult)
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_UNSUPPORTED_ENGINE_VERSION,
            TEXT("allowEmptyResult needs FGeometryScriptMeshBooleanOptions::bAllowEmptyResult, "
                 "added in UE 5.4. This engine always fails a boolean whose result is empty; "
                 "omit the parameter or pass false."));
    }
#endif
    // 5.6+ only - see GeometryOps::FBooleanParams::OutputTransformSpace.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    BoolOptions.OutputTransformSpace = Params.OutputTransformSpace;
#endif

    // The failure channel, not decoration. ApplyMeshBoolean returns TargetMesh from every path
    // it has - including the empty-result refusal that is the only reachable failure of a
    // well-formed call - and reports the refusal ONLY into this argument. Passing nullptr here
    // is what made the `if (!ResultMesh)` arm below dead code and let all four boolean verbs
    // answer success over a boolean the engine had declined to perform. See
    // GeometryScriptDebugSink.h for the per-call-site evidence.
    FGeometryScriptDebugSink Debug;

    // Taken after every guard and immediately before the engine call, so a refused boolean pays
    // nothing for it. It is a full copy of the target mesh, which is the price of being able to
    // tell a surviving vertex from one the boolean invented - see GeometrySkinWeightRepair.h.
    const GeometrySkinWeights::FPreEditSnapshot SkinSnapshot(Target);

    UDynamicMesh* ResultMesh = UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean(
        Target,
        TargetTransform,
        Tool,
        ToolTransform,
        Operation,
        BoolOptions,
        Debug.Get()
    );

    // Forwarded before the failure check so a warning survives a failing call - the same reason
    // FailIn exists rather than Fail.
    Debug.DrainWarningsInto(Result);

    // Checked BEFORE the null test, because on UE 5.8 this is the arm that actually fires. The
    // engine's text is appended verbatim: on the empty-result refusal it names its own remedy
    // ("enable Allow Empty Result if empty results should be accepted"), which is the sentence
    // the caller needs and the one a paraphrase would lose.
    //
    // The target is untouched on this path (ApplyMeshBoolean returns before its SetMesh), so the
    // caller keeps exactly the geometry it had - and FailIn keeps the before-counts that prove it.
    if (Debug.HasError())
    {
        const FString EngineText = Debug.ErrorText();

        UE_LOG(LogMcpGeometryHandlersNew, Warning,
            TEXT("Boolean %s failed: %s"), OpName, *EngineText);

        return FOpResult::FailIn(Result, ErrorCodes::ERR_BOOLEAN_FAILED,
            FString::Printf(TEXT("Boolean %s failed - %s"), OpName, *EngineText));
    }

    // Retained as a guard against a future engine that DOES return null, not as live detection:
    // on 5.8 it can only fire for a null Target, which the guard at the top of this function
    // already rejected. It is deliberately not the primary check - that is Debug.HasError()
    // above.
    if (!ResultMesh)
    {
        UE_LOG(LogMcpGeometryHandlersNew, Warning,
            TEXT("Boolean %s returned null result - operation may have produced empty geometry"), OpName);

        return FOpResult::FailIn(Result, ErrorCodes::ERR_BOOLEAN_FAILED,
            FString::Printf(TEXT("Boolean %s failed - operation produced empty geometry"), OpName));
    }

    const int32 ResultTriCount = ResultMesh->GetTriangleCount();

    if (ResultTriCount > GEOM_MAX_TRIANGLES_PER_DYNAMIC_MESH)
    {
        UE_LOG(LogMcpGeometryHandlersNew, Warning,
            TEXT("Boolean %s result has %d triangles (exceeds limit of %d)"),
            OpName, ResultTriCount, GEOM_MAX_TRIANGLES_PER_DYNAMIC_MESH);
    }

    if (ResultTriCount > GEOM_WARNING_TRIANGLE_THRESHOLD)
    {
        UE_LOG(LogMcpGeometryHandlersNew, Warning,
            TEXT("Boolean %s result has %d triangles (warning threshold: %d)"),
            OpName, ResultTriCount, GEOM_WARNING_TRIANGLE_THRESHOLD);
    }

    GeometryOpsBoolean_RepairSkinWeights(SkinSnapshot, Target, Result);

    FinishOp(ResultMesh, Result);

    // Deliberate divergence from FinishOp, and the only one in this family. FinishOp defines
    // bChanged as triangles-OR-vertices moved; the four boolean verbs echo bChanged as their
    // `changed` field, and that field has always been the TRIANGLE delta alone. A boolean that
    // retriangulates without changing the triangle count would flip the published value from
    // false to true - an observable wire change, which ~146 dispatcher tests exist to catch.
    // Restated here rather than hidden behind a flag so the divergence is impossible to miss.
    Result.bChanged = (Result.TrianglesAfter != Result.TrianglesBefore);
    return Result;
}

FOpResult Trim(
    UDynamicMesh* Target,
    const FTransform& TargetTransform,
    UDynamicMesh* Tool,
    const FTransform& ToolTransform,
    const FTrimParams& Params)
{
    // Two handles, so CaptureBefore rather than BeginOp - see Boolean() above.
    FOpResult Result;
    if (!Target || !Tool)
    {
        return NullMeshFailure();
    }
    CaptureBefore(Target, Result);

    const EGeometryScriptBooleanOperation Operation = Params.bKeepInside
        ? EGeometryScriptBooleanOperation::Intersection
        : EGeometryScriptBooleanOperation::Subtract;

    // The same pre-flight the three symmetric booleans run, and for the same reason: trim
    // dispatches the SAME engine call (ApplyMeshBoolean) on the same two meshes, so it is the
    // same class of allocation - FMeshBoolean builds two AABB trees, a cut mesh per input and
    // the winding-number structures over them. It shipped without one, which meant the one op
    // an author reaches for to carve a heavy mesh down was the one op that would attempt the
    // carve with the machine already at 90% of physical memory.
    //
    // No POLYGON_LIMIT_EXCEEDED companion: that guard estimates target+tool*3 as an upper bound
    // on a union's output, and a trim's output is bounded by the TARGET alone. Adding it here
    // would refuse trims that cannot exceed the budget. The memory guard has no such asymmetry.
    if (!BooleanMemoryPressureSafe())
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_MEMORY_PRESSURE,
            FString::Printf(TEXT("Memory pressure too high (%.1f%% used). Boolean %s blocked to prevent OOM."),
                GeometryUtils::GetMemoryUsagePercent(), BooleanOperationName(Operation)));
    }

    // bSimplifyOutput is set from the params like the rest, and FTrimParams defaults it to the
    // engine's true - which is the value this call reached by NOT setting it at all. Spelling it
    // out changes nothing and removes the standing trap that a later edit to the default-
    // constructed struct would silently alter trim while leaving the four boolean verbs alone.
    FGeometryScriptMeshBooleanOptions BoolOptions;
    BoolOptions.bFillHoles = Params.bFillHoles;
    BoolOptions.bSimplifyOutput = Params.bSimplifyOutput;
    BoolOptions.SimplifyPlanarTolerance = static_cast<float>(Params.SimplifyPlanarTolerance);
    // FGeometryScriptMeshBooleanOptions::bAllowEmptyResult arrived in UE 5.4. On 5.3 the engine
    // unconditionally refuses an empty result, which is exactly the default (false). A caller who
    // asked for true is refused by name rather than served the opposite behaviour under a success.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    BoolOptions.bAllowEmptyResult = Params.bAllowEmptyResult;
#else
    if (Params.bAllowEmptyResult)
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_UNSUPPORTED_ENGINE_VERSION,
            TEXT("allowEmptyResult needs FGeometryScriptMeshBooleanOptions::bAllowEmptyResult, "
                 "added in UE 5.4. This engine always fails a boolean whose result is empty; "
                 "omit the parameter or pass false."));
    }
#endif
    // 5.6+ only - see GeometryOps::FBooleanParams::OutputTransformSpace.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    BoolOptions.OutputTransformSpace = Params.OutputTransformSpace;
#endif

    // See Boolean() above and GeometryScriptDebugSink.h: this argument is the only channel
    // ApplyMeshBoolean reports a refusal through.
    FGeometryScriptDebugSink Debug;

    // Same snapshot, same reason as Boolean() above: trim dispatches the same engine call, so it
    // creates the same stale-slot skin weights.
    const GeometrySkinWeights::FPreEditSnapshot SkinSnapshot(Target);

    UDynamicMesh* ResultMesh = UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean(
        Target,
        TargetTransform,
        Tool,
        ToolTransform,
        Operation,
        BoolOptions,
        Debug.Get());

    Debug.DrainWarningsInto(Result);

    // Capturing the RETURN VALUE was the first half of this fix and was not enough on its own:
    // ApplyMeshBoolean never returns null, so `if (!ResultMesh)` alone left the op still
    // structurally incapable of failing - the engine could refuse the trim, leave the target
    // untrimmed, and the caller was told it succeeded and built on top of geometry that still
    // had the tool volume in it. The refusal travels through Debug. Same code and same message
    // shape as Boolean(), so the two cannot describe one engine failure differently.
    //
    // The refusal this reaches most often is a trim whose result would be EMPTY, which with
    // FTrimParams::bAllowEmptyResult at its default false the engine declines to produce. That
    // default is deliberate - see the field - and the engine's own text names the option a
    // caller would have to turn ON to accept an empty trim.
    if (Debug.HasError())
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_BOOLEAN_FAILED,
            FString::Printf(TEXT("Boolean %s failed - %s"),
                BooleanOperationName(Operation), *Debug.ErrorText()));
    }

    // Future-engine guard only; unreachable on 5.8 for the same reason as in Boolean().
    if (!ResultMesh)
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_BOOLEAN_FAILED,
            FString::Printf(TEXT("Boolean %s failed - operation produced empty geometry"),
                BooleanOperationName(Operation)));
    }

    GeometryOpsBoolean_RepairSkinWeights(SkinSnapshot, Target, Result);

    FinishOp(Target, Result);
    return Result;
}

FOpResult SelfUnion(UDynamicMesh* Target, const FSelfUnionParams& Params)
{
    FOpResult Result;
    if (!BeginOp(Target, Result)) return Result;

    // Same pre-flight, same reason as Trim() above. ApplyMeshSelfUnion resolves the mesh
    // against ITSELF, so the allocation is a boolean's over a single input rather than a
    // smaller one - it is the op a caller reaches for precisely when a mesh has become large
    // and self-intersecting. Fails in place: nothing has touched the mesh yet, and the
    // before-counts BeginOp recorded are what prove it.
    if (!BooleanMemoryPressureSafe())
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_MEMORY_PRESSURE,
            FString::Printf(TEXT("Memory pressure too high (%.1f%% used). Self-union blocked to prevent OOM."),
                GeometryUtils::GetMemoryUsagePercent()));
    }

    FGeometryScriptMeshSelfUnionOptions SelfUnionOptions;
    SelfUnionOptions.bFillHoles = Params.bFillHoles;
    SelfUnionOptions.bTrimFlaps = Params.bTrimFlaps;
    SelfUnionOptions.bSimplifyOutput = Params.bSimplifyOutput;
    SelfUnionOptions.SimplifyPlanarTolerance = Params.SimplifyPlanarTolerance;
    SelfUnionOptions.WindingThreshold = Params.WindingThreshold;

    // ApplyMeshSelfUnion runs FMeshSelfUnion, which deletes the triangles it resolves away and
    // then re-triangulates over the freed vertex slots - the same stale-skin-weight exposure the
    // two-mesh booleans have. See GeometrySkinWeightRepair.h.
    const GeometrySkinWeights::FPreEditSnapshot SkinSnapshot(Target);

    UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshSelfUnion(Target, SelfUnionOptions, nullptr);

    GeometryOpsBoolean_RepairSkinWeights(SkinSnapshot, Target, Result);

    FinishOp(Target, Result);
    return Result;
}

FOpResult Mirror(UDynamicMesh* Target, const FMirrorParams& Params)
{
    FOpResult Result;
    if (!BeginOp(Target, Result)) return Result;

    // One sink for all three engine calls below. Each of them returns its TargetMesh on every
    // path, error paths included, so the three `if (!X)` guards this op used to rely on were
    // dead code and its ERR_OPERATION_FAILED was unreachable. Messages accumulate, so each call
    // is followed by its own HasError() check and the first failure wins - which is also what
    // keeps the message naming the step that actually failed. See GeometryScriptDebugSink.h.
    FGeometryScriptDebugSink Debug;

    UDynamicMesh* MirroredMesh = NewObject<UDynamicMesh>(GetTransientPackage());
    MirroredMesh->SetMesh(Target->GetMeshRef());

    // Mirror by scaling with a negative value on the axis.
    const FVector MirrorScale = GeometryOpsBoolean_MirrorScaleForAxis(Params.Axis);

    // A reflection has determinant -1, so negating one axis turns every triangle inside out.
    // The scale therefore has to reverse the winding as well as move the vertices; the branch
    // that did not is what made every mirrored half a black, back-faced shell, and made
    // `mirror` inside a boolean tool subtract an anti-solid instead of a solid.
    //
    // The upper bound this guard used to carry (< 5.5) was wrong: ScaleMesh still takes
    // bFixOrientationForNegativeScale in 5.8 (MeshTransformFunctions.h:68-75), so the engine's
    // own correct path was being skipped on every version this plugin actually targets. The
    // `#else` below is now DEAD on every version this plugin builds against and is kept only so
    // a 5.3-and-earlier backport has something to restore - see docs/engine-version-support.md
    // (## Dead version guards). Its sibling is Stretch's in GeometryOps_Modeling.cpp.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    UGeometryScriptLibrary_MeshTransformFunctions::ScaleMesh(
        MirroredMesh, MirrorScale, FVector::ZeroVector, /*bFixOrientationForNegativeScale=*/ true,
        Debug.Get());
#else
    // DEAD BRANCH - not reached on 5.4+, and this plugin targets 5.8 only. Fallback: scale the
    // mesh through the low-level API, then undo the inversion by hand. Positions only, so it
    // leaves stale normals and tangents behind where MeshTransforms::Scale applies the inverse
    // scale to both (MeshTransforms.cpp:150-170) - the same gap Stretch's dead branch carries,
    // and one more reason not to promote either back to a live path.
    {
        UE::Geometry::FDynamicMesh3& EditMesh = MirroredMesh->GetMeshRef();
        for (int32 VID : EditMesh.VertexIndicesItr())
        {
            FVector3d Pos = EditMesh.GetVertex(VID);
            Pos.X *= MirrorScale.X;
            Pos.Y *= MirrorScale.Y;
            Pos.Z *= MirrorScale.Z;
            EditMesh.SetVertex(VID, Pos);
        }
        // CONDITIONAL, matching Stretch's dead branch and MeshTransforms::Scale itself, which
        // reverses only when Scale.X * Scale.Y * Scale.Z < 0 (MeshTransforms.cpp:179). This
        // used to reverse unconditionally, which is wrong for the one axis value that produces
        // no reflection at all: EMeshAxis::None resolves to a UNIT scale (see
        // GeometryOpsBoolean_MirrorScaleForAxis), so an unrecognized `axis` string appended a
        // clone that was not mirrored and then flipped its winding anyway.
        if (MirrorScale.X * MirrorScale.Y * MirrorScale.Z < 0.0)
        {
            EditMesh.ReverseOrientation(/*bFlipNormals=*/ true);
        }
    }
#endif

    FGeometryScriptAppendMeshOptions AppendOptions;
    UDynamicMesh* Appended = UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(
        Target, MirroredMesh, FTransform::Identity, false, AppendOptions, Debug.Get());

    // The clone is a full copy of the mesh and the append has already consumed it. Leaking one
    // per call was tolerable while mirror was an interactive verb; it is now the .pwmodel
    // compiler's inner loop, so a leaked clone per mirror per part accumulates for the whole
    // compile. Same release GeneratePipe does with its scratch meshes.
    MirroredMesh->MarkAsGarbage();

    Debug.DrainWarningsInto(Result);

    // Covers the scale AND the append: both wrote into the same sink, and neither reports
    // through its return value. On 5.8 the only errors either can raise are null-mesh ones
    // (MeshBasicEditFunctions.cpp:590-599, MeshTransformFunctions.cpp), so this fires only when
    // the NewObject above failed to allocate - but it is the first arm of this op that CAN fire
    // at all, and the engine's text names which of the two steps it was.
    if (Debug.HasError())
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_OPERATION_FAILED,
            FString::Printf(TEXT("Mirror failed - the mirrored half could not be appended: %s"),
                *Debug.ErrorText()));
    }

    // Future-engine guard only: AppendMesh returns TargetMesh on every path it has.
    if (!Appended)
    {
        return FOpResult::FailIn(Result, ErrorCodes::ERR_OPERATION_FAILED,
            TEXT("Mirror failed - the mirrored half could not be appended"));
    }

    if (Params.bWeld)
    {
        FGeometryScriptWeldEdgesOptions WeldOptions;
        // Was the literal 0.001f. FMirrorParams::WeldTolerance still DEFAULTS to 0.001, so this
        // is a widening and not a behaviour change - see the field for why that value is kept
        // rather than aligned to weld_vertices' 0.0001 or the engine's 1e-06.
        WeldOptions.Tolerance = Params.WeldTolerance;
        WeldOptions.bOnlyUniquePairs = Params.bOnlyUniquePairs;
        UDynamicMesh* Welded = UGeometryScriptLibrary_MeshRepairFunctions::WeldMeshEdges(
            Target, WeldOptions, Debug.Get());

        Debug.DrainWarningsInto(Result);

        // WeldMeshEdges appends "Weld Operation returned error flag" when
        // FMergeCoincidentMeshEdges::Apply() returns false (MeshRepairFunctions.cpp:147) and
        // then returns TargetMesh regardless, so this is the only arm of mirror that the engine
        // has any wording for.
        //
        // STILL UNREACHABLE ON UE 5.8, and for a different reason than before: Apply() has
        // exactly one return statement and it is `return true`
        // (GeometryCore/Private/DynamicMesh/Operations/MergeCoincidentMeshEdges.cpp:233), so the
        // flag it is supposed to raise is never raised. The arm is wired anyway rather than
        // deleted, because the ONE thing that keeps a failure detectable across an engine
        // upgrade is the channel being connected before the engine starts using it - which is
        // exactly the lesson the boolean family taught by shipping the opposite.
        if (Debug.HasError())
        {
            return FOpResult::FailIn(Result, ErrorCodes::ERR_OPERATION_FAILED,
                FString::Printf(TEXT("Mirror failed - the seam could not be welded after the append: %s"),
                    *Debug.ErrorText()));
        }

        // Future-engine guard only: WeldMeshEdges returns TargetMesh on every path it has.
        if (!Welded)
        {
            return FOpResult::FailIn(Result, ErrorCodes::ERR_OPERATION_FAILED,
                TEXT("Mirror failed - the seam could not be welded after the append"));
        }
    }

    FinishOp(Target, Result);
    return Result;
}

FOpResult ValidateArrayCount(int32 Count)
{
    if (Count < 1 || Count > 100)
    {
        return FOpResult::Fail(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("count must be between 1 and 100"));
    }
    return FOpResult::Ok();
}

FOpResult ArrayLinear(UDynamicMesh* Target, const FArrayLinearParams& Params)
{
    // Count first, and on a fresh result: the RPC wrapper runs this same check BEFORE it
    // resolves the actor, so a rejected count carries no counts there and must not start
    // carrying them here. Nothing has accumulated yet, so Fail()'s fresh struct is correct.
    FOpResult Invalid = ValidateArrayCount(Params.Count);
    if (!Invalid.bSuccess)
    {
        return Invalid;
    }

    FOpResult Result;
    if (!BeginOp(Target, Result)) return Result;

    // Runs after BeginOp so a rejected array still reports the pre-op counts that prove the mesh
    // was left alone; the budget check fails in place to keep them.
    if (!GeometryOpsBoolean_CheckArrayBudget(Target, Params.Count, Result))
    {
        return Result;
    }

    UDynamicMesh* SourceMesh = NewObject<UDynamicMesh>(GetTransientPackage());
    SourceMesh->SetMesh(Target->GetMeshRef());

    FTransform RepeatTransform;
    RepeatTransform.SetLocation(Params.Offset);

    FGeometryScriptAppendMeshOptions AppendOptions;
    // bApplyTransformToFirstInstance = true: apply the offset before the first append so the
    // Count-1 appended copies land at offset..(Count-1)*offset while the original stays at 0,
    // giving the documented evenly-spaced layout. With false the first appended copy gets the
    // identity transform, doubling the original at the origin and leaving the row one full
    // spacing short.
    UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMeshRepeated(
        Target, SourceMesh, RepeatTransform, Params.Count - 1, /*bApplyTransformToFirstInstance=*/ true, false, AppendOptions, nullptr);

    // Released for the same reason as Mirror's clone: one full-mesh copy per array call, and the
    // arrays are the compiler's inner loop.
    SourceMesh->MarkAsGarbage();

    FinishOp(Target, Result);
    return Result;
}

FOpResult ArrayRadial(UDynamicMesh* Target, const FArrayRadialParams& Params)
{
    // See ArrayLinear: count first and on a fresh result, then the mesh guard, then the budget.
    FOpResult Invalid = ValidateArrayCount(Params.Count);
    if (!Invalid.bSuccess)
    {
        return Invalid;
    }

    FOpResult Result;
    if (!BeginOp(Target, Result)) return Result;

    if (!GeometryOpsBoolean_CheckArrayBudget(Target, Params.Count, Result))
    {
        return Result;
    }

    UDynamicMesh* SourceMesh = NewObject<UDynamicMesh>(GetTransientPackage());
    SourceMesh->SetMesh(Target->GetMeshRef());

    const double AngleStep = Params.TotalAngleDegrees / Params.Count;
    const FVector RotationAxis = GeometryOpsBoolean_RotationAxisFor(Params.Axis);

    TArray<FTransform> Transforms;
    for (int32 i = 1; i < Params.Count; ++i)  // Start from 1: the original occupies step 0.
    {
        const double Angle = AngleStep * i;
        const FQuat Rotation = FQuat(RotationAxis, FMath::DegreesToRadians(Angle));
        FTransform Transform;
        Transform.SetRotation(Rotation);
        // Rotate around the center point.
        Transform.SetLocation(Params.Center + Rotation.RotateVector(-Params.Center));
        Transforms.Add(Transform);
    }

    FGeometryScriptAppendMeshOptions AppendOptions;
    UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMeshTransformed(
        Target, SourceMesh, Transforms, FTransform::Identity, true, false, AppendOptions, nullptr);

    SourceMesh->MarkAsGarbage();

    FinishOp(Target, Result);
    return Result;
}

FOpResult ArrayAlongPath(UDynamicMesh* Target, const FArrayAlongPathParams& Params)
{
    // The frame list is the count, so the same 1..100 bound the other two enforce applies to its
    // length - and for the same reason it runs first and on a fresh result.
    FOpResult Invalid = ValidateArrayCount(Params.Frames.Num());
    if (!Invalid.bSuccess)
    {
        return Invalid;
    }

    FOpResult Result;
    if (!BeginOp(Target, Result)) return Result;

    if (!GeometryOpsBoolean_CheckArrayBudget(Target, Params.Frames.Num(), Result))
    {
        return Result;
    }

    UDynamicMesh* SourceMesh = NewObject<UDynamicMesh>(GetTransientPackage());
    SourceMesh->SetMesh(Target->GetMeshRef());

    // Reset BEFORE the append, which is what makes N frames produce N copies rather than N+1.
    // Safe on these meshes because none of them carries a UDynamicMeshGenerator - Reset() re-runs
    // one when bEnableMeshGenerator is set, and would then repopulate the mesh being emptied.
    Target->Reset();

    FGeometryScriptAppendMeshOptions AppendOptions;
    UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMeshTransformed(
        Target, SourceMesh, Params.Frames, FTransform::Identity, true, false, AppendOptions, nullptr);

    SourceMesh->MarkAsGarbage();

    // bForceChanged: a single identity frame rebuilds the same mesh, so the triangle count is
    // unmoved and the delta test would report a real rebuild as a no-op. Every other frame list
    // moves geometry, and neither case left the mesh alone.
    FinishOp(Target, Result, /*bForceChanged=*/true);
    return Result;
}

} // namespace GeometryOps
