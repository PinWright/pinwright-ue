// Copyright (c) 2026 Alexander Penkin. MIT License.

// GeometryOps_Advanced.cpp - see GeometryOps_Advanced.h for why the actor-driven verbs take
// extracted data rather than actors.
//
// The null guard, the before/after count snapshot and the clamp-and-warn pair are
// GeometryOps.h's, shared with the other four families: this file no longer carries a private
// copy of any of them.
#include "Handlers/Geometry/GeometryOps_Advanced.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Geometry/GeometryScriptDebugSink.h"
#include "Handlers/Geometry/GeometryUtils.h"
#include "Compat/EngineVersionCompat.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicMeshOverlay.h"
#include "UDynamicMesh.h"

#if __has_include("GeometryScript/GeometryScriptTypes.h")
#include "GeometryScript/GeometryScriptTypes.h"
#else
#include "GeometryScriptTypes.h"
#endif

#include "MeshBoundaryLoops.h"

#include "GeometryScript/MeshBasicEditFunctions.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryScript/MeshQueryFunctions.h"
#include "GeometryScript/MeshRepairFunctions.h"

namespace
{
    // Uniquely prefixed: this module builds with bUseUnity = true, so a plainly-named
    // anonymous-namespace helper would collide with a sibling TU once Unity merges them - and
    // AppendSweepPolygonCompat / BuildCircularProfile are exactly the names the next family
    // reaches for. Every sibling file prefixes for the same reason (GeometryOpsBoolean_*,
    // GeometryOpsModeling_*).

    // Bounds of the spline path step count, in ONE place. SplinePathStepCount() is what the RPC
    // wrapper calls to size the sample list it hands back, and Sweep's fallback clamps the same
    // caller value through ClampRangeWarn; a second literal pair here would let the op warn
    // about a range the wrapper does not enforce.
    constexpr int32 GeometryOpsAdvanced_SplineStepMin = 2;
    constexpr int32 GeometryOpsAdvanced_SplineStepMax = 256;

    // Sweep a 2D profile polygon along PathFrames, appending the swept surface to Mesh.
    // Owns the single engine-version fork in one place: UE 5.5+ added a MiterLimit parameter
    // to AppendSweepPolygon that UE 5.4 lacks, so any future signature change is a one-line
    // edit here instead of an N-way lock-step update across the loft/sweep/extrude ops.
    //
    // Note on the profile: the FVector2D vertices are passed straight through. Earlier code
    // copied them into a default-constructed FGeometryScriptSimplePolygon whose Vertices
    // TSharedPtr is null until Reset() allocates it, so every guarded Add/copy was skipped and
    // the sweep received an EMPTY polygon -> the mesh stayed empty while the handler still
    // reported success.
    void GeometryOpsAdvanced_AppendSweepPolygonCompat(
        UDynamicMesh* Mesh,
        const FGeometryScriptPrimitiveOptions& PrimOptions,
        const FTransform& SweepTransform,
        const TArray<FVector2D>& PolygonVertices,
        const TArray<FTransform>& PathFrames,
        bool bLoop,
        bool bCapped)
    {
#if UE_VERSION_OLDER_THAN(5,5,0)
        // UE 5.4: AppendSweepPolygon does not have the MiterLimit parameter
        UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSweepPolygon(
            Mesh, PrimOptions, SweepTransform, PolygonVertices, PathFrames,
            bLoop, bCapped,
            1.0f,     // StartScale
            1.0f,     // EndScale
            0.0f,     // RotationAngleDeg
            nullptr);
#else
        // UE 5.5+: AppendSweepPolygon gains MiterLimit parameter
        UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSweepPolygon(
            Mesh, PrimOptions, SweepTransform, PolygonVertices, PathFrames,
            bLoop, bCapped,
            1.0f,     // StartScale
            1.0f,     // EndScale
            0.0f,     // RotationAngleDeg
            1.0f,     // MiterLimit
            nullptr);
#endif
    }

    // The circular cross-section both sweep verbs derive from a mesh's bounding box. Neither
    // traces the mesh's outline - a caller expecting its silhouette to be swept gets a circle
    // sized by the larger of the two horizontal half-extents.
    TArray<FVector2D> GeometryOpsAdvanced_BuildCircularProfile(int32 NumSides, double Radius)
    {
        TArray<FVector2D> Vertices;
        Vertices.Reserve(NumSides);
        for (int32 i = 0; i < NumSides; ++i)
        {
            const double Angle = 2.0 * PI * i / NumSides;
            Vertices.Add(FVector2D(FMath::Cos(Angle) * Radius, FMath::Sin(Angle) * Radius));
        }
        return Vertices;
    }

    // The cross-section either sweep verb actually sweeps: the caller's polygon when it has one,
    // otherwise the bounding-box circle. Two vertices cannot bound a face, so anything shorter
    // than a triangle is treated as "no profile" rather than swept into a degenerate ribbon -
    // and the .pwmodel front-end rejects a short `profile=` before it reaches here, so the
    // fallback is only ever reached by a caller that supplied none.
    TArray<FVector2D> GeometryOpsAdvanced_ResolveProfile(
        const GeometryOps::FSweepProfile& Profile, int32 NumSides, double Radius)
    {
        if (Profile.Vertices.Num() >= 3)
        {
            return Profile.Vertices;
        }
        return GeometryOpsAdvanced_BuildCircularProfile(NumSides, Radius);
    }

    // ------------------------------------------------------------------------------------
    // The two path diagnostics both sweep verbs publish.
    //
    // Neither is a clamp and neither changes what is built: a path a sweep cannot use is not a
    // path the op can guess a substitute for. They exist because the failure they name is
    // otherwise SILENT - a degenerate sweep reports bSuccess, isClosed true and zero boundary
    // edges, at a triangle count indistinguishable from a correct one. The only tells were a
    // degenerate-triangle count and a bounding box with no thickness, neither of which names a
    // parameter. Both messages therefore lead with `path`, which is what an author can act on.
    // ------------------------------------------------------------------------------------

    // Whether the path ever leaves the plane its cross-section sits in.
    //
    // AppendSweepPolygon lands the profile in each frame's (Rotation.AxisY(), Rotation.AxisZ())
    // plane and takes Rotation.AxisX() as that plane's NORMAL - it builds
    // FFrame3d(Location, Rotation.AxisY(), Rotation.AxisZ(), Rotation.AxisX()), whose third
    // argument is the frame's Z (MeshPrimitiveFunctions.cpp:1136-1138). A segment perpendicular
    // to that normal therefore slides the section along inside its own plane and sweeps no
    // volume at all: the span comes back as a flat slab, and the engine says nothing.
    //
    // Returns the index of the first frame whose OUTGOING segment is degenerate, else
    // INDEX_NONE. Zero-length segments are skipped: a repeated control point is legal and
    // already tolerated, and it has no direction to test.
    int32 GeometryOpsAdvanced_FirstInPlaneSegment(const TArray<FTransform>& PathFrames)
    {
        // cos(89.5 degrees). A span this close to edge-on has a thickness under 1% of its
        // length, which is the flat slab this exists to name rather than a shape anyone drew.
        constexpr double InPlaneTolerance = 0.0087;

        for (int32 Index = 0; Index + 1 < PathFrames.Num(); ++Index)
        {
            FVector Direction = PathFrames[Index + 1].GetLocation() - PathFrames[Index].GetLocation();
            if (!Direction.Normalize())
            {
                continue;
            }

            const FVector SectionNormal = PathFrames[Index].GetRotation().GetAxisX();
            if (FMath::Abs(FVector::DotProduct(Direction, SectionNormal)) < InPlaneTolerance)
            {
                return Index;
            }
        }
        return INDEX_NONE;
    }

    void GeometryOpsAdvanced_WarnInPlanePath(const TArray<FTransform>& PathFrames,
                                             GeometryOps::FOpResult& Result)
    {
        const int32 Index = GeometryOpsAdvanced_FirstInPlaneSegment(PathFrames);
        if (Index == INDEX_NONE)
        {
            return;
        }

        Result.Warnings.Add(FString::Printf(
            TEXT("path frame %d sweeps inside its own cross-section plane - the frame's local +X ")
            TEXT("is perpendicular to the segment leaving it, so that span comes back flat. Give ")
            TEXT("each frame the rotation of the path tangent at it."), Index));
    }

    // The result-side half: a swept mesh with no thickness on an axis.
    //
    // Read off the WHOLE mesh, so it is only meaningful when the sweep had the mesh to itself -
    // both verbs APPEND, and an existing box would hide a flat sweep behind its own extent.
    // TrianglesBefore is the discriminator. The frame check above is what covers the rest.
    void GeometryOpsAdvanced_WarnFlatResult(UDynamicMesh* Mesh, GeometryOps::FOpResult& Result)
    {
        if (!Mesh || Result.TrianglesBefore > 0 || Mesh->GetTriangleCount() == 0)
        {
            return;
        }

        const FVector Size =
            UGeometryScriptLibrary_MeshQueryFunctions::GetMeshBoundingBox(Mesh).GetSize();

        const TCHAR* FlatAxis = nullptr;
        if (Size.X <= KINDA_SMALL_NUMBER)      { FlatAxis = TEXT("X"); }
        else if (Size.Y <= KINDA_SMALL_NUMBER) { FlatAxis = TEXT("Y"); }
        else if (Size.Z <= KINDA_SMALL_NUMBER) { FlatAxis = TEXT("Z"); }

        if (FlatAxis)
        {
            Result.Warnings.Add(FString::Printf(
                TEXT("path swept a flat result - the mesh has zero extent on %s, so it encloses ")
                TEXT("no volume."), FlatAxis));
        }
    }

    // How near its own start a path has to return before it counts as closed. World units, and
    // deliberately loose: an author writing a ring repeats the first frame's coordinates by
    // hand or generates them from an angle sweep, and neither route lands on the bit pattern.
    constexpr double GeometryOpsAdvanced_PathCloseTolerance = 0.01;
}

namespace GeometryOps
{

// ---------------------------------------------------------------------------
// bridge
// ---------------------------------------------------------------------------

FOpResult Bridge(UDynamicMesh* Mesh, const FBridgeParams& Params, FBridgeOutputs& Out)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result)) return Result;
    bool bPreparedAttributes = false;

    // Get direct access to FDynamicMesh3 for low-level operations
    UE::Geometry::FDynamicMesh3& EditMesh = Mesh->GetMeshRef();

    // FMeshBoundaryLoops lives in GeometryCore (Public/MeshBoundaryLoops.h) on every supported
    // engine, 5.3 included, with the same constructor / bAborted / GetLoopCount / operator[]
    // surface. An earlier version gate here claimed it was 5.4+ and routed 5.3 to a hole-fill
    // "fallback"; that premise was false. The remaining one-loop fallback still needs the shared
    // overlay preflight below because FillAllMeshHoles writes normals and UV0 without checking
    // that either overlay exists.
    UE::Geometry::FMeshBoundaryLoops BoundaryLoops(&EditMesh, true);

    if (BoundaryLoops.bAborted)
    {
        Out.Status = TEXT("Boundary loop computation aborted (mesh topology issue)");
    }
    else if (BoundaryLoops.GetLoopCount() < 2)
    {
        // Not enough boundary loops for bridging - fall back to hole filling
        Out.Status = FString::Printf(TEXT("Only %d boundary loop(s) found, need at least 2 for bridging. Filling holes instead."), BoundaryLoops.GetLoopCount());
        if (!PrepareHoleFillAttributes(Mesh, TEXT("bridge"), Result, bPreparedAttributes))
        {
            return Result;
        }

        FGeometryScriptFillHolesOptions FillOptions;
        FillOptions.FillMethod = EGeometryScriptFillHolesMethod::MinimalFill;
        int32 NumFilledHoles = 0;
        int32 NumFailedHoleFills = 0;
        UGeometryScriptLibrary_MeshRepairFunctions::FillAllMeshHoles(Mesh, FillOptions, NumFilledHoles, NumFailedHoleFills, nullptr);
    }
    else
    {
        // Validate edge group indices. edgeGroupA reports its own clamp: the loop count is
        // discovered here and nowhere the caller can see, so an out-of-range index silently
        // bridged a different loop than the one asked for and the response named neither.
        const int32 LoopCount = BoundaryLoops.GetLoopCount();
        const int32 LoopIndexA = ClampRangeWarn(Params.EdgeGroupA, 0, LoopCount - 1, TEXT("edgeGroupA"), Result);
        int32 LoopIndexB = FMath::Clamp(Params.EdgeGroupB, 0, LoopCount - 1);

        if (LoopIndexA == LoopIndexB)
        {
            LoopIndexB = (LoopIndexA + 1) % LoopCount;
        }

        const UE::Geometry::FEdgeLoop& LoopA = BoundaryLoops[LoopIndexA];
        const UE::Geometry::FEdgeLoop& LoopB = BoundaryLoops[LoopIndexB];

        const TArray<int32>& VertsA = LoopA.Vertices;
        const TArray<int32>& VertsB = LoopB.Vertices;

        const int32 NumVertsA = VertsA.Num();
        const int32 NumVertsB = VertsB.Num();

        if (NumVertsA > 0 && NumVertsB > 0)
        {
            // Find the closest starting vertex on LoopB to LoopA's first vertex
            const FVector3d StartPosA = EditMesh.GetVertex(VertsA[0]);
            int32 BestStartB = 0;
            double BestDist = TNumericLimits<double>::Max();

            for (int32 i = 0; i < NumVertsB; ++i)
            {
                const double Dist = FVector3d::DistSquared(StartPosA, EditMesh.GetVertex(VertsB[i]));
                if (Dist < BestDist)
                {
                    BestDist = Dist;
                    BestStartB = i;
                }
            }

            // Create triangle strips between the two loops
            const int32 MaxVerts = FMath::Max(NumVertsA, NumVertsB);

            for (int32 i = 0; i < MaxVerts; ++i)
            {
                const int32 iA = i % NumVertsA;
                const int32 iA_Next = (i + 1) % NumVertsA;
                const int32 iB = (BestStartB + i) % NumVertsB;
                const int32 iB_Next = (BestStartB + i + 1) % NumVertsB;

                const int32 vA0 = VertsA[iA];
                const int32 vA1 = VertsA[iA_Next];
                const int32 vB0 = VertsB[iB];
                const int32 vB1 = VertsB[iB_Next];

                // Triangle 1: vA0 -> vA1 -> vB0
                if (vA0 != vA1 && vA1 != vB0 && vB0 != vA0)
                {
                    if (EditMesh.AppendTriangle(vA0, vA1, vB0) >= 0) Out.TrianglesCreated++;
                }

                // Triangle 2: vB0 -> vA1 -> vB1
                if (vB0 != vA1 && vA1 != vB1 && vB1 != vB0)
                {
                    if (EditMesh.AppendTriangle(vB0, vA1, vB1) >= 0) Out.TrianglesCreated++;
                }
            }

            Out.Status = FString::Printf(TEXT("Bridged loop %d (%d verts) to loop %d (%d verts), created %d triangles"),
                LoopIndexA, NumVertsA, LoopIndexB, NumVertsB, Out.TrianglesCreated);
        }
        else
        {
            Out.Status = TEXT("One or both boundary loops have no vertices");
        }
    }

    FinishOp(Mesh, Result, bPreparedAttributes);
    return Result;
}

// ---------------------------------------------------------------------------
// edge_split
// ---------------------------------------------------------------------------

FOpResult EdgeSplit(UDynamicMesh* Mesh, const FEdgeSplitParams& Params, FEdgeSplitOutputs& Out)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result)) return Result;

    // Use FDynamicMesh3 directly for edge splitting
    UE::Geometry::FDynamicMesh3& DynMesh = Mesh->GetMeshRef();

    for (int32 EdgeID : Params.EdgeIndices)
    {
        if (!DynMesh.IsEdge(EdgeID))
        {
            continue;
        }

        const UE::Geometry::FIndex2i EdgeV = DynMesh.GetEdgeV(EdgeID);

        const FVector3d V0 = DynMesh.GetVertex(EdgeV.A);
        const FVector3d V1 = DynMesh.GetVertex(EdgeV.B);
        const FVector3d Midpoint = V0 + (V1 - V0) * Params.SplitFactor;

        const int32 NewVertexID = DynMesh.AppendVertex(Midpoint);

        const UE::Geometry::FIndex2i EdgeT = DynMesh.GetEdgeT(EdgeID);
        TArray<int32> TrisToModify;
        if (EdgeT.A >= 0) TrisToModify.Add(EdgeT.A);
        if (EdgeT.B >= 0) TrisToModify.Add(EdgeT.B);

        for (int32 TriID : TrisToModify)
        {
            if (!DynMesh.IsTriangle(TriID))
            {
                continue;
            }

            const UE::Geometry::FIndex3i Tri = DynMesh.GetTriangle(TriID);

            int32 ReplaceV = -1;
            int32 KeepV1 = -1, KeepV2 = -1;

            if (Tri.A == EdgeV.A && Tri.B == EdgeV.B) { ReplaceV = Tri.B; KeepV1 = Tri.A; KeepV2 = Tri.C; }
            else if (Tri.B == EdgeV.A && Tri.C == EdgeV.B) { ReplaceV = Tri.C; KeepV1 = Tri.A; KeepV2 = Tri.B; }
            else if (Tri.C == EdgeV.A && Tri.A == EdgeV.B) { ReplaceV = Tri.A; KeepV1 = Tri.B; KeepV2 = Tri.C; }
            else if (Tri.A == EdgeV.B && Tri.B == EdgeV.A) { ReplaceV = Tri.B; KeepV1 = Tri.A; KeepV2 = Tri.C; }
            else if (Tri.B == EdgeV.B && Tri.C == EdgeV.A) { ReplaceV = Tri.C; KeepV1 = Tri.A; KeepV2 = Tri.B; }
            else if (Tri.C == EdgeV.B && Tri.A == EdgeV.A) { ReplaceV = Tri.A; KeepV1 = Tri.B; KeepV2 = Tri.C; }

            if (ReplaceV >= 0)
            {
                DynMesh.RemoveTriangle(TriID);
                DynMesh.AppendTriangle(KeepV1, NewVertexID, KeepV2);
                DynMesh.AppendTriangle(NewVertexID, ReplaceV, KeepV2);
                Out.EdgesSplit++;
            }
        }
    }

    // Weld vertices if requested
    if (Params.bWeldVertices && Params.WeldTolerance > 0)
    {
        FGeometryScriptWeldEdgesOptions WeldOptions;
        WeldOptions.Tolerance = Params.WeldTolerance;
        WeldOptions.bOnlyUniquePairs = true;
        UGeometryScriptLibrary_MeshRepairFunctions::WeldMeshEdges(Mesh, WeldOptions, nullptr);
    }

    FinishOp(Mesh, Result);
    return Result;
}

// ---------------------------------------------------------------------------
// loft
// ---------------------------------------------------------------------------

FOpResult Loft(UDynamicMesh* Mesh, const FLoftParams& Params,
               const TArray<FLoftProfileSample>& Profiles, FLoftOutputs& Out)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result)) return Result;

    if (Params.bUseProfiles)
    {
        // Profiles requested but fewer than two resolved, or an endpoint carries no mesh:
        // nothing is appended. Deliberately NOT the bounding-box branch below - falling through
        // to it would silently substitute a different shape for the one that was asked for.
        if (Profiles.Num() >= 2 && Profiles[0].bHasMesh && Profiles.Last().bHasMesh)
        {
            const FVector StartPos = Profiles[0].Location;
            const FVector EndPos = Profiles.Last().Location;
            FVector Direction = EndPos - StartPos;
            const double PathLength = Direction.Size();

            if (PathLength > KINDA_SMALL_NUMBER)
            {
                Direction.Normalize();

                const FVector ProfileExtent = Profiles[0].Extent;

                // Create a simple polygon approximating the first profile's cross-section.
                // Side count is derived from the RAW subdivisions (8 + it, clamped) - clamping
                // the caller value first would change the polygon this branch has always built.
                const int32 NumPolySides = FMath::Clamp(8 + Params.Subdivisions, 4, 64);
                const double ProfileRadius = FMath::Max(ProfileExtent.X, ProfileExtent.Y);
                const TArray<FVector2D> PolygonVertices =
                    GeometryOpsAdvanced_BuildCircularProfile(NumPolySides, ProfileRadius);

                // Build path frames for sweeping. The two loft branches clamp subdivisions to
                // DIFFERENT ceilings (64 here, 32 below), so which one a caller hit is not
                // derivable from the response - the warning is the only record of it.
                TArray<FTransform> PathFrames;
                const int32 NumPathSteps =
                    ClampRangeWarn(Params.Subdivisions, 2, 64, TEXT("subdivisions"), Result);

                for (int32 Step = 0; Step <= NumPathSteps; ++Step)
                {
                    const double T = (double)Step / NumPathSteps;
                    const FVector Pos = StartPos + Direction * PathLength * T;
                    const FQuat Rotation = FQuat::FindBetweenNormals(FVector::UpVector, Direction);
                    PathFrames.Add(FTransform(Rotation, Pos));
                }

                FGeometryScriptPrimitiveOptions PrimOptions;
                PrimOptions.PolygroupMode = EGeometryScriptPrimitivePolygroupMode::PerQuad;
                PrimOptions.bFlipOrientation = false;

                const FTransform SweepTransform(FRotator::ZeroRotator, StartPos);

                // Sweep the profile polygon along the loft path.
                GeometryOpsAdvanced_AppendSweepPolygonCompat(Mesh, PrimOptions, SweepTransform,
                    PolygonVertices, PathFrames, /*bLoop*/ false, /*bCapped*/ Params.bCap);

                Out.ProfilesUsed = Profiles.Num();
            }
        }
    }
    else
    {
        // No profile actors provided - perform simple Z-axis extrusion
        const FBox BBox = UGeometryScriptLibrary_MeshQueryFunctions::GetMeshBoundingBox(Mesh);
        const FVector Center = BBox.GetCenter();
        const FVector Extent = BBox.GetExtent();

        const double ExtrudeHeight = Extent.Z > KINDA_SMALL_NUMBER ? Extent.Z : 100.0;

        const int32 NumPolySides = FMath::Clamp(8 + Params.Subdivisions, 4, 64);
        const double Radius = FMath::Max(Extent.X, Extent.Y);
        const TArray<FVector2D> PolygonVertices =
            GeometryOpsAdvanced_BuildCircularProfile(NumPolySides, Radius);

        TArray<FTransform> PathFrames;
        const int32 NumPathSteps =
            ClampRangeWarn(Params.Subdivisions, 2, 32, TEXT("subdivisions"), Result);

        for (int32 Step = 0; Step <= NumPathSteps; ++Step)
        {
            const double T = (double)Step / NumPathSteps;
            const FVector Pos = Center + FVector(0, 0, -ExtrudeHeight/2 + ExtrudeHeight * T);
            PathFrames.Add(FTransform(FQuat::Identity, Pos));
        }

        FGeometryScriptPrimitiveOptions PrimOptions;
        PrimOptions.PolygroupMode = EGeometryScriptPrimitivePolygroupMode::PerQuad;
        PrimOptions.bFlipOrientation = false;

        const FTransform SweepTransform(FRotator::ZeroRotator, Center);

        // Sweep the profile along the vertical extrusion path. The earlier code built the
        // polygon and path frames but never called AppendSweepPolygon (it only logged), so
        // the mesh stayed empty while the handler still reported success. The frame-count log
        // that narrated the line above is gone: the .pwmodel compiler drives this op once per
        // part, so it was per-part log spam that restated NumPathSteps + 1.
        GeometryOpsAdvanced_AppendSweepPolygonCompat(Mesh, PrimOptions, SweepTransform,
            PolygonVertices, PathFrames, /*bLoop*/ false, /*bCapped*/ Params.bCap);
    }

    // Recompute normals for smooth shading if requested
    if (Params.bSmooth)
    {
        // Warning channel only - RecomputeNormals has no error a non-null mesh can reach. See
        // GeometryOps_Modeling.cpp's RecalculateNormals for what the one warning means.
        GeometryOps::FGeometryScriptDebugSink NormalsDebug;
        UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(Mesh, FGeometryScriptCalculateNormalsOptions(), false, NormalsDebug.Get());
        NormalsDebug.DrainWarningsInto(Result);
    }

    FinishOp(Mesh, Result);
    return Result;
}

// ---------------------------------------------------------------------------
// sweep / extrude_along_spline
// ---------------------------------------------------------------------------

int32 SplinePathStepCount(int32 Steps)
{
    return FMath::Clamp(Steps, GeometryOpsAdvanced_SplineStepMin, GeometryOpsAdvanced_SplineStepMax);
}

double FSweepScaleCurve::Evaluate(double Alpha) const
{
    if (!IsSet())
    {
        return 1.0;
    }

    // Held rather than extrapolated outside the outermost knots. Extrapolating a piecewise-linear
    // law off its own ends is where a curve that stops at alpha 0.9 turns into a negative scale at
    // 1.0 - a cross-section folded through its own axis, which reverses the tube's facing normals
    // and is invisible in any render. Holding cannot do that: every value it can return is a value
    // the author wrote.
    if (Alpha <= Knots[0].X)
    {
        return Knots[0].Y;
    }
    if (Alpha >= Knots.Last().X)
    {
        return Knots.Last().Y;
    }

    for (int32 i = 1; i < Knots.Num(); ++i)
    {
        if (Alpha <= Knots[i].X)
        {
            const double Span = Knots[i].X - Knots[i - 1].X;

            // Two knots at one alpha is a step, not a division by zero. The caller's validator
            // refuses it; this arm is what keeps a hand-built curve from producing a NaN that
            // then propagates silently into every vertex of the swept tube.
            if (Span <= UE_DOUBLE_SMALL_NUMBER)
            {
                return Knots[i].Y;
            }
            const double T = (Alpha - Knots[i - 1].X) / Span;
            return FMath::Lerp(Knots[i - 1].Y, Knots[i].Y, T);
        }
    }
    return Knots.Last().Y;
}

// The one place either op turns an alpha into a cross-section scale. Shared so the spline branch,
// the vertical fallback and extrude_along_spline cannot end up honouring the curve in two of the
// three places - which is exactly how `scale_start` and `scale_end` came to be dropped on a
// closed path without anything saying so.
static float GeometryOpsAdvanced_ScaleAt(const FSweepScaleCurve& Curve, double ScaleStart,
                                         double ScaleEnd, float Alpha)
{
    return Curve.IsSet()
        ? (float)Curve.Evaluate((double)Alpha)
        : FMath::Lerp((float)ScaleStart, (float)ScaleEnd, Alpha);
}

FOpResult Sweep(UDynamicMesh* Mesh, const FSweepParams& Params, const FSweepPath& Path,
                FSweepOutputs& Out)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result)) return Result;

    // Get mesh bounding box to derive profile shape
    const FBox MeshBBox = UGeometryScriptLibrary_MeshQueryFunctions::GetMeshBoundingBox(Mesh);
    const FVector MeshCenter = MeshBBox.GetCenter();
    const FVector MeshExtent = MeshBBox.GetExtent();

    const int32 NumPolySides = FMath::Clamp(Params.Steps / 2, 4, 32);
    double ProfileRadius = FMath::Max(MeshExtent.X, MeshExtent.Y);
    if (ProfileRadius < KINDA_SMALL_NUMBER)
    {
        ProfileRadius = 50.0;
    }
    const TArray<FVector2D> PolygonVertices =
        GeometryOpsAdvanced_ResolveProfile(Params.Profile, NumPolySides, ProfileRadius);
    Out.ProfileVertices = PolygonVertices.Num();

    // Build sweep path
    TArray<FTransform> PathFrames;

    if (Path.Samples.Num() >= 2)
    {
        Out.PathSteps = Path.Samples.Num() - 1;
        PathFrames.Reserve(Path.Samples.Num());

        for (int32 i = 0; i < Path.Samples.Num(); ++i)
        {
            const float Alpha = (float)i / Out.PathSteps;

            const float TwistAngle = FMath::DegreesToRadians(Params.Twist * Alpha);
            const FQuat TwistRotation = FQuat(FVector::ForwardVector, TwistAngle);
            const FQuat Rotation = Path.Samples[i].GetRotation() * TwistRotation;

            const float Scale = GeometryOpsAdvanced_ScaleAt(
                Params.ScaleCurve, Params.ScaleStart, Params.ScaleEnd, Alpha);

            PathFrames.Add(FTransform(Rotation, Path.Samples[i].GetLocation(), FVector(Scale)));
        }

        Out.Status = FString::Printf(TEXT("Swept along spline with %d steps, length %.1f"), Out.PathSteps, Path.SplineLength);
    }
    else if (Path.bActorHasNoSplineComponent)
    {
        Out.Status = TEXT("Spline actor found but no USplineComponent - using linear sweep");
    }

    // Fallback: If no spline or spline invalid, create a linear vertical sweep
    if (PathFrames.Num() < 2)
    {
        const double SweepHeight = MeshExtent.Z > KINDA_SMALL_NUMBER ? MeshExtent.Z * 2 : 100.0;

        // Same bounds SplinePathStepCount() applies for the wrapper, routed through the warn
        // form because this is the branch where the caller's `steps` is the value being
        // rewritten: pathSteps echoes the clamped number and nothing said it had moved.
        Out.PathSteps = ClampRangeWarn(Params.Steps, GeometryOpsAdvanced_SplineStepMin,
            GeometryOpsAdvanced_SplineStepMax, TEXT("steps"), Result);

        // The frame's local +X is the SWEEP DIRECTION and the cross-section lands in its local
        // Y-Z plane, so a fallback that runs up world +Z needs frames whose +X is world +Z.
        // They used to be `FQuat(FVector::UpVector, Twist)` - a rotation about world Z, leaving
        // local +X in the XY plane and therefore PERPENDICULAR to the path. That put the section
        // plane's own normal across the direction of travel, so the documented "vertical sweep
        // through the mesh's bounding box" swept a FLAT RIBBON with zero extent on X and no
        // enclosed volume, at exactly the triangle count a solid tube has. Nothing reported it;
        // the same warning that now names a caller's degenerate path would have named this one.
        //
        // Twist is unchanged in effect: it is applied about the frame's local +X, which on this
        // path IS world Z, and that is also the axis the old expression used. Applying it as a
        // local-X rotation makes the fallback and the spline branch spell twist the same way.
        const FQuat PathOrientation = FRotationMatrix::MakeFromX(FVector::UpVector).ToQuat();

        for (int32 i = 0; i <= Out.PathSteps; ++i)
        {
            const float Alpha = (float)i / Out.PathSteps;
            const FVector Location = MeshCenter + FVector(0, 0, -SweepHeight/2 + SweepHeight * Alpha);

            const float TwistAngle = FMath::DegreesToRadians(Params.Twist * Alpha);
            const FQuat Rotation = PathOrientation * FQuat(FVector::ForwardVector, TwistAngle);

            const float Scale = GeometryOpsAdvanced_ScaleAt(
                Params.ScaleCurve, Params.ScaleStart, Params.ScaleEnd, Alpha);

            PathFrames.Add(FTransform(Rotation, Location, FVector(Scale)));
        }

        if (Out.Status.IsEmpty())
        {
            Out.Status = FString::Printf(TEXT("Linear sweep with %d steps, height %.1f"), Out.PathSteps, SweepHeight);
        }
    }

    // Perform the sweep using Geometry Script
    if (PathFrames.Num() >= 2)
    {
        // Reported BEFORE the engine call, so the warning survives whatever the call does with
        // the path. Nothing is substituted - see the helper for why a degenerate path has no
        // sound replacement - the caller is simply told which frame produced the flat span.
        GeometryOpsAdvanced_WarnInPlanePath(PathFrames, Result);

        FGeometryScriptPrimitiveOptions PrimOptions;
        PrimOptions.PolygroupMode = EGeometryScriptPrimitivePolygroupMode::PerQuad;
        PrimOptions.bFlipOrientation = false;

        const FTransform SweepTransform = FTransform::Identity;

        // Sweep the profile along the path. The earlier code built the polygon and path
        // frames but never called AppendSweepPolygon, so the mesh stayed empty while
        // sweepStatus still reported a successful sweep. The frame-count log that narrated
        // the line above is gone - sweepStatus already carries the step count, and the
        // .pwmodel compiler turns one line per op into one line per part.
        GeometryOpsAdvanced_AppendSweepPolygonCompat(Mesh, PrimOptions, SweepTransform,
            PolygonVertices, PathFrames, /*bLoop*/ false, /*bCapped*/ Params.bCap);
    }

    FinishOp(Mesh, Result);

    // After the append, and after FinishOp has filled the after-counts: the check reads the
    // mesh the sweep produced and compares against the before-count FinishOp did not touch.
    GeometryOpsAdvanced_WarnFlatResult(Mesh, Result);
    return Result;
}

FOpResult ExtrudeAlongSpline(UDynamicMesh* Mesh, const FExtrudeAlongSplineParams& Params,
                             const TArray<FTransform>& PathSamples)
{
    FOpResult Result;
    if (!BeginOp(Mesh, Result)) return Result;

    // Get mesh bounding box to derive profile shape
    const FBox MeshBBox = UGeometryScriptLibrary_MeshQueryFunctions::GetMeshBoundingBox(Mesh);
    const FVector MeshExtent = MeshBBox.GetExtent();

    const int32 NumPolySides = FMath::Clamp(Params.Segments / 2, 4, 32);
    double ProfileRadius = FMath::Max(MeshExtent.X, MeshExtent.Y);
    if (ProfileRadius < KINDA_SMALL_NUMBER)
    {
        ProfileRadius = 50.0;
    }
    const TArray<FVector2D> PolygonVertices =
        GeometryOpsAdvanced_ResolveProfile(Params.Profile, NumPolySides, ProfileRadius);

    // Apply twist and scale onto the spline samples
    TArray<FTransform> PathFrames;
    PathFrames.Reserve(PathSamples.Num());
    const int32 PathSteps = FMath::Max(PathSamples.Num() - 1, 1);

    for (int32 i = 0; i < PathSamples.Num(); ++i)
    {
        const float Alpha = (float)i / PathSteps;

        const float TwistAngle = FMath::DegreesToRadians(Params.Twist * Alpha);
        const FQuat TwistRotation = FQuat(FVector::ForwardVector, TwistAngle);
        const FQuat Rotation = PathSamples[i].GetRotation() * TwistRotation;

        const float Scale = GeometryOpsAdvanced_ScaleAt(
            Params.ScaleCurve, Params.ScaleStart, Params.ScaleEnd, Alpha);

        PathFrames.Add(FTransform(Rotation, PathSamples[i].GetLocation(), FVector(Scale)));
    }

    // bLoop is derived from the PATH. It used to be hardcoded true, and that single argument is
    // the whole of this verb's `cap` defect:
    //
    //  - FGeneralizedCylinderGenerator builds caps under `if (bCapped && !bLoop)`
    //    (SweepGenerator.cpp:608 and :707) and asserts a loop has none (:129), so `cap` was
    //    unreachable at EVERY path length. Measured: `cap` omitted and `cap=true` produced
    //    byte-identical meshes.
    //  - The same generator gates path scaling on `bLoop == false` (:616), so `scale_start` and
    //    `scale_end` were silently dropped with it.
    //  - Worst of the three, it CLOSED PATHS THAT DO NOT CLOSE. An open 3-frame path came back
    //    as a tube running A->B->C->A - a wrap segment no author asked for - which read as
    //    "capped" only because a loop has no ends to leave open. At 2 frames the wrap collapses
    //    onto the outbound span: 20 triangles, 8 boundary edges, not closed.
    //
    // A path counts as closed when it RETURNS TO ITS START, which is the only signal either
    // front-end can carry (neither publishes a `loop` parameter, and the RPC verb's spline is
    // sampled into a plain frame list before it reaches here). The engine adds the wrap segment
    // itself - NumPathSegs = bLoop ? Path.Num() : Path.Num() - 1 (SweepGenerator.cpp:96) - so a
    // closing path has to hand it the ring WITHOUT the repeated final frame, or the wrap is a
    // zero-length segment. Four frames is the minimum that survives dropping one and still
    // clears the generator's own three-cross-section floor for a loop (:255).
    bool bLoop = false;
    if (PathFrames.Num() >= 4 && PathFrames[0].GetLocation().Equals(
            PathFrames.Last().GetLocation(), GeometryOpsAdvanced_PathCloseTolerance))
    {
        PathFrames.RemoveAt(PathFrames.Num() - 1);
        bLoop = true;

        // Only when the caller would otherwise be waiting for caps. A closed tube is watertight
        // without them, so this is "the parameter had nothing to do here", not a defect report.
        if (Params.bCap)
        {
            // Prose after the leading token, deliberately: the two scale parameters are spelled
            // differently on the two front-ends (scaleStart / scale_start) and only the leading
            // word is translated on re-emission, so naming them by role rather than by spelling
            // is the one wording that is correct on both.
            Result.Warnings.Add(TEXT("cap has no effect on a path that returns to its start: the ")
                TEXT("swept tube closes into a loop and has no ends. The start and end scale ")
                TEXT("parameters are dropped on a closed path for the same reason."));
        }

        // The scale law is dropped by the same `bLoop == false` gate, and the warning above does
        // not cover it: that one fires only when `cap` was ALSO set. An author who wrote a radius
        // law on a ring and no cap would otherwise get a uniform tube with nothing said - which
        // is the failure mode this op has already been through once, on `cap` itself.
        if (Params.ScaleCurve.IsSet())
        {
            Result.Warnings.Add(TEXT("scales is dropped on a path that returns to its start: the ")
                TEXT("engine's loop branch gates path scaling off entirely, so the cross-section ")
                TEXT("is swept at its authored size the whole way round. Move the last frame off ")
                TEXT("the first to sweep the path OPEN if the section has to change size along it."));
        }
    }

    GeometryOpsAdvanced_WarnInPlanePath(PathFrames, Result);

    // Unlike Sweep this runs unguarded on the frame count and takes the engine's DEFAULT
    // primitive options rather than PerQuad polygroups - both are the shipped behaviour, not
    // oversights corrected here.
    FGeometryScriptPrimitiveOptions PrimOptions;

    GeometryOpsAdvanced_AppendSweepPolygonCompat(Mesh, PrimOptions, FTransform::Identity,
        PolygonVertices, PathFrames, bLoop, /*bCapped*/ Params.bCap);

    FinishOp(Mesh, Result);
    GeometryOpsAdvanced_WarnFlatResult(Mesh, Result);
    return Result;
}

// ---------------------------------------------------------------------------
// append_buffers
// ---------------------------------------------------------------------------

FOpResult ValidateAppendBufferTriangle(int32 TriangleIndex, const FIntVector& Triangle,
                                       int32 VertexCount)
{
    const bool bIn0 = (Triangle.X >= 0 && Triangle.X < VertexCount);
    const bool bIn1 = (Triangle.Y >= 0 && Triangle.Y < VertexCount);
    const bool bIn2 = (Triangle.Z >= 0 && Triangle.Z < VertexCount);
    if (bIn0 && bIn1 && bIn2)
    {
        return FOpResult::Ok();
    }

    const int32 BadIdx = !bIn0 ? Triangle.X : (!bIn1 ? Triangle.Y : Triangle.Z);
    // Fail() rather than FailIn(): a validator takes no mesh and has accumulated no warnings or
    // before-counts, which is the one case where building a fresh result discards nothing.
    return FOpResult::Fail(ErrorCodes::ERR_INVALID_PARAMS,
        FString::Printf(TEXT("triangle %d references vertex index %d out of range [0, %d)"),
            TriangleIndex, BadIdx, VertexCount));
}

FOpResult ValidateAppendBufferVertexCount(int32 VertexCount)
{
    if (VertexCount > 0)
    {
        return FOpResult::Ok();
    }
    return FOpResult::Fail(ErrorCodes::ERR_INVALID_PARAMS,
        TEXT("vertices must be a non-empty array of [x,y,z] or {x,y,z}"));
}

FOpResult ValidateAppendBufferAttributeCount(const TCHAR* AttributeName, int32 AttributeCount,
                                             int32 VertexCount)
{
    if (AttributeCount == VertexCount)
    {
        return FOpResult::Ok();
    }
    return FOpResult::Fail(ErrorCodes::ERR_INVALID_PARAMS,
        FString::Printf(TEXT("%s count (%d) must match vertices count (%d)"),
            AttributeName, AttributeCount, VertexCount));
}

FOpResult ValidateAppendBuffers(const FAppendBuffersParams& Params)
{
    const int32 NumVerts = Params.Vertices.Num();
    const FOpResult VertexCheck = ValidateAppendBufferVertexCount(NumVerts);
    if (!VertexCheck.bSuccess)
    {
        return VertexCheck;
    }

    for (int32 t = 0; t < Params.Triangles.Num(); ++t)
    {
        const FOpResult TriResult = ValidateAppendBufferTriangle(t, Params.Triangles[t], NumVerts);
        if (!TriResult.bSuccess)
        {
            return TriResult;
        }
    }

    // An absent optional attribute is legal; a present one must be complete.
    if (Params.Normals.Num() > 0)
    {
        const FOpResult R = ValidateAppendBufferAttributeCount(TEXT("normals"), Params.Normals.Num(), NumVerts);
        if (!R.bSuccess) return R;
    }
    if (Params.UVs.Num() > 0)
    {
        const FOpResult R = ValidateAppendBufferAttributeCount(TEXT("uvs"), Params.UVs.Num(), NumVerts);
        if (!R.bSuccess) return R;
    }
    if (Params.Colors.Num() > 0)
    {
        const FOpResult R = ValidateAppendBufferAttributeCount(TEXT("colors"), Params.Colors.Num(), NumVerts);
        if (!R.bSuccess) return R;
    }

    return FOpResult::Ok();
}

// How many UV layers the TARGET mesh would lose real element data from if AppendBuffersToMesh
// were allowed to resize its layer set. Zero when nothing would be lost.
//
// The engine's append does not merge UV layers, it REPLACES the count: it counts the UV sets in
// the INCOMING buffers and then calls SetNumUVLayers(NumUVLayers) on the TARGET
// (MeshBasicEditFunctions.cpp:973-983), and SetNumUVLayers shrinks by RemoveAt
// (DynamicMeshAttributeSet.cpp:719-740), destroying the overlay objects and every element in
// them. So `box size=100` followed by an `append_buffers` with no `uvs=` does not merely fail to
// give the new triangles UVs - it deletes the BOX's UVs too, silently, on the whole mesh.
//
// The answer is the layer COUNT, not the count of layers carrying elements: preserving layer 2
// means asking the engine for 3 sets, which necessarily preserves 0 and 1 as well.
//
// Returns 0 when no layer carries elements. That case is deliberately not padded: layers with no
// elements hold no data to lose, and padding them would change what `procedural_mesh` +
// `append_buffers` produces - the very shape the shell and bevel crash guards in
// GeometryOps_Modeling.cpp are written against and tested on.
static int32 GeometryOpsAdvanced_UVLayersWorthPreserving(UDynamicMesh* Mesh)
{
    if (!Mesh)
    {
        return 0;
    }

    const UE::Geometry::FDynamicMeshAttributeSet* Attributes = Mesh->GetMeshRef().Attributes();
    if (!Attributes)
    {
        return 0;
    }

    const int32 NumLayers = Attributes->NumUVLayers();
    for (int32 LayerIndex = 0; LayerIndex < NumLayers; ++LayerIndex)
    {
        const UE::Geometry::FDynamicMeshUVOverlay* Layer = Attributes->GetUVLayer(LayerIndex);
        if (Layer && Layer->ElementCount() > 0)
        {
            return NumLayers;
        }
    }

    return 0;
}

FOpResult AppendBuffers(UDynamicMesh* Mesh, FAppendBuffersParams Params,
                        FAppendBuffersOutputs& Out)
{
    // Counts are taken before validation as well as before the append; nothing between the two
    // points touches the mesh, so the appended counts stay the honest delta they have always
    // been (correct even when the seed actor already carries geometry).
    FOpResult Result;
    if (!BeginOp(Mesh, Result)) return Result;

    // Read BEFORE the append: after it the layers are already gone and the count is unrecoverable.
    const int32 UVLayersToPreserve = GeometryOpsAdvanced_UVLayersWorthPreserving(Mesh);

    const FOpResult Validation = ValidateAppendBuffers(Params);
    if (!Validation.bSuccess)
    {
        // FailIn, not `return Validation`: the before-counts BeginOp recorded (and any warning
        // a later rule adds here) would be thrown away by returning the validator's own fresh
        // struct instead. The wire sees only code and message, both forwarded verbatim.
        return FOpResult::FailIn(Result, *Validation.ErrorCode, Validation.ErrorMessage);
    }

    FGeometryScriptSimpleMeshBuffers Buffers;
    Buffers.Vertices = MoveTemp(Params.Vertices);
    Buffers.Triangles = MoveTemp(Params.Triangles);
    if (Params.Normals.Num() > 0)
    {
        Buffers.Normals = MoveTemp(Params.Normals);
    }
    if (Params.UVs.Num() > 0)
    {
        Buffers.UV0 = MoveTemp(Params.UVs);
    }
    if (Params.Colors.Num() > 0)
    {
        Buffers.VertexColors = MoveTemp(Params.Colors);
    }
    // One group ID per triangle so a non-default groupId applies to the whole batch.
    Buffers.TriGroupIDs.Init(Params.GroupId, Buffers.Triangles.Num());

    // DATA-LOSS GUARD, not a convenience. See GeometryOpsAdvanced_UVLayersWorthPreserving above:
    // AppendBuffersToMesh sets the TARGET's UV layer count to whatever the INCOMING buffers carry,
    // so appending UV-less geometry to a UV'd mesh wipes the UVs off geometry that was already
    // there. The plugin cannot change the engine call, and it cannot undo the loss afterwards
    // (SetNumUVLayers destroys the overlays, so restoring the count restores empty layers) - the
    // only place to intervene is here, by making the incoming buffers declare as many UV sets as
    // the target already has.
    //
    // The engine counts sets as a PREFIX RUN: it walks UV0..UV7 and stops at the first array whose
    // length is not the vertex count (MeshBasicEditFunctions.cpp:973-982). Padding therefore has
    // to fill every slot from the first empty one up, and a padded slot must be exactly
    // NumVertices long.
    //
    // The pad value is (0,0). That is an honest placeholder, not a projection: the appended
    // triangles genuinely have no authored UVs, and inventing a box projection here would make
    // `append_buffers` silently disagree with `uvs=` for the same geometry. The warning says so.
    // Preserving beats warning-only because the loss is unrecoverable and silent, and it costs the
    // published RPC response nothing - BulkEditHandler does not surface FOpResult::Warnings, so
    // geometry.append_buffers answers with the same fields it always has. The .pwmodel compiler
    // does surface them, as PWMODEL_STAGE_WARNING.
    const int32 NumVertices = Buffers.Vertices.Num();
    const int32 IncomingUVSets = (Buffers.UV0.Num() == NumVertices && NumVertices > 0) ? 1 : 0;
    if (UVLayersToPreserve > IncomingUVSets)
    {
        TArray<FVector2D>* const BufferUVSets[8] = {
            &Buffers.UV0, &Buffers.UV1, &Buffers.UV2, &Buffers.UV3,
            &Buffers.UV4, &Buffers.UV5, &Buffers.UV6, &Buffers.UV7 };

        // FGeometryScriptSimpleMeshBuffers carries eight UV slots and no more
        // (MeshBasicEditFunctions.h:33-47), so a target with more than eight layers cannot be
        // fully preserved through this call. Say so rather than reporting a clean preserve.
        const int32 PadTo = FMath::Min(UVLayersToPreserve, 8);
        for (int32 LayerIndex = IncomingUVSets; LayerIndex < PadTo; ++LayerIndex)
        {
            BufferUVSets[LayerIndex]->Init(FVector2D::ZeroVector, NumVertices);
        }

        Result.Warnings.Add(FString::Printf(
            TEXT("append_buffers carried %d UV set(s) but the mesh already had %d UV layer(s) with "
                 "UV data; the engine's append sets the layer count EXACTLY, so those layers would "
                 "have been deleted off the EXISTING geometry. Padded the appended vertices with "
                 "placeholder (0,0) UVs in layer(s) %d-%d to keep them. Pass uvs= to author real "
                 "UVs for the appended geometry."),
            IncomingUVSets, UVLayersToPreserve, IncomingUVSets, PadTo - 1));

        if (UVLayersToPreserve > 8)
        {
            Result.Warnings.Add(FString::Printf(
                TEXT("append_buffers can preserve at most 8 UV layers (the engine's buffer struct "
                     "has UV0-UV7); layers 8-%d were deleted"), UVLayersToPreserve - 1));
        }
    }

    const int32 RequestedTriangles = Buffers.Triangles.Num();

    // ROLLBACK BUFFER, and the side-effect audit for this site (rpc-design.md 12) is why it is
    // here rather than a comment saying the mesh may be partial.
    //
    // Every other wired site in this module can sequence its failure check BEFORE the thing that
    // commits. This one cannot: AppendBuffersToMesh mutates the target INSIDE the same
    // EditMesh lambda in which it decides a triangle is unacceptable, and the decision is made
    // per triangle after the vertices are already in. By the time HasError() can be read, the
    // mesh already holds every vertex the caller passed plus whatever subset of the triangles the
    // engine accepted. There is no ordering that fixes that, so the commit has to be undone
    // instead of deferred.
    //
    // Undoing it by hand is not an option: the accepted triangles are recoverable from
    // NewTriangleIndices, but the appended VERTICES are not. FDynamicMesh3::AppendVertex reuses
    // ids from the free list, so on a mesh that has ever had a vertex deleted the new vertices do
    // not occupy a contiguous tail above the pre-call MaxVertexID and cannot be identified after
    // the fact. A copy is the only exact rollback available.
    //
    // Cost: one FDynamicMesh3 copy per append onto a NON-EMPTY mesh, paid on the success path
    // too. An empty target - the common "build a mesh out of arrays" call - pays nothing, because
    // rolling an empty mesh back is Reset(). Note that append_buffers does NOT enforce
    // GEOM_MAX_TRIANGLES_PER_DYNAMIC_MESH the way the boolean and subdivide ops do, so the copy is
    // bounded only by whatever the target already holds; if that guard is ever added here, this
    // copy inherits its bound.
    const bool bTargetHadGeometry = (Result.VerticesBefore > 0 || Result.TrianglesBefore > 0);
    UE::Geometry::FDynamicMesh3 PreAppendMesh;
    if (bTargetHadGeometry)
    {
        Mesh->ProcessMesh([&PreAppendMesh](const UE::Geometry::FDynamicMesh3& ReadMesh)
        {
            PreAppendMesh = ReadMesh;
        });
    }

    // The failure channel. AppendBuffersToMesh returns TargetMesh unconditionally and reports a
    // triangle it will not take by calling AppendError and skipping it - one message per refused
    // triangle, then it carries on. Passing nullptr here is what let geometry.append_buffers
    // answer success over a mesh missing the caller's geometry, with appendedTriangles quietly
    // short of what was passed and the orphaned vertices still counted in appendedVertices.
    GeometryOps::FGeometryScriptDebugSink Debug;

    FGeometryScriptIndexList NewTriangleIndices;
    UGeometryScriptLibrary_MeshBasicEditFunctions::AppendBuffersToMesh(
        Mesh, Buffers, NewTriangleIndices, Params.MaterialId, /*bDeferChangeNotifications=*/false, Debug.Get());

    // Forwarded before the failure check for the same reason Boolean() does it: a warning must
    // survive a failing call. AppendBuffersToMesh has no AppendWarning path on UE 5.8, so this
    // adds nothing today and exists so that the first engine that does add one is not dropped.
    Debug.DrainWarningsInto(Result);

    if (Debug.HasError())
    {
        const int32 RefusedTriangles = Debug.ErrorCount();
        const int32 AcceptedTriangles = Mesh->GetTriangleCount() - Result.TrianglesBefore;

        if (bTargetHadGeometry)
        {
            Mesh->SetMesh(MoveTemp(PreAppendMesh));
        }
        else
        {
            Mesh->Reset();
        }

        UE_LOG(LogMcpGeometryHandlersNew, Warning,
            TEXT("append_buffers refused %d of %d triangles; mesh rolled back: %s"),
            RefusedTriangles, RequestedTriangles, *Debug.ErrorSummary());

        // FailIn, so the before-counts BeginOp recorded survive - and after the rollback above
        // they are the mesh's CURRENT counts, which is what makes them worth keeping.
        //
        // ErrorSummary, not ErrorText: one message per refused triangle means a 500k-triangle
        // batch of duplicates would otherwise serialise half a million copies of one sentence
        // into a single error field.
        return FOpResult::FailIn(Result, ErrorCodes::ERR_MESH_APPEND_FAILED,
            FString::Printf(
                TEXT("append_buffers: the engine refused %d of %d triangle(s) (it would have "
                     "accepted %d). The mesh has been restored to its state before the append, so "
                     "nothing was partially applied. The engine reported: %s"),
                RefusedTriangles, RequestedTriangles, AcceptedTriangles, *Debug.ErrorSummary()));
    }

    FinishOp(Mesh, Result);

    Out.AppendedVertices = Result.VerticesAfter - Result.VerticesBefore;
    Out.AppendedTriangles = Result.TrianglesAfter - Result.TrianglesBefore;
    return Result;
}

} // namespace GeometryOps
