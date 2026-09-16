// Copyright (c) 2026 Alexander Penkin. MIT License.

// MeshAuditUtils.cpp - the static-mesh health sweep. See MeshAuditUtils.h for why it exists
// and for the four design constraints its shape comes from.
#include "Handlers/Geometry/MeshAuditUtils.h"

#include "AssetRegistry/AssetData.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimationPoseData.h"
#include "Animation/AttributesRuntime.h"
#include "Animation/AnimTypes.h"
#include "AnimationRuntime.h"
#include "BoneContainer.h"
#include "BonePose.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Handlers/ErrorCodes.h"
#include "Compat/EngineVersionCompat.h"
#include "Misc/MemStack.h"
#include "MeshQueries.h"
#include "Selections/MeshConnectedComponents.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "UDynamicMesh.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "UObject/Package.h"

#if __has_include("GeometryScript/GeometryScriptTypes.h")
#include "GeometryScript/GeometryScriptTypes.h"
#else
#include "GeometryScriptTypes.h"
#endif
#include "GeometryScript/MeshAssetFunctions.h"

namespace MeshAudit
{
    // ---- Check registry -------------------------------------------------------------------

    const TArray<FCheckInfo>& AllChecks()
    {
        // Function-local static so the table is built once and its address is stable. Order
        // is ECheck order; the runtime assert below is what keeps the two from drifting.
        static const TArray<FCheckInfo> Checks = {
            {ECheck::Inverted, TEXT("inverted"), ErrorCodes::ERR_MESH_AUDIT_INVERTED,
             ESeverity::Error, true, true,
             TEXT("Every edge-connected component is checked independently. A closed component "
                  "whose signed volume is negative is inside out; an open or near-zero-volume "
                  "component is UNKNOWN, never clean. The response includes each component's "
                  "status and signed volume because a whole-mesh sum can cancel to zero.")},
            {ECheck::InconsistentWinding, TEXT("inconsistent_winding"),
             ErrorCodes::ERR_MESH_AUDIT_INCONSISTENT_WINDING, ESeverity::Error, true, false,
             TEXT("Interior edges whose two triangles traverse the shared edge in the SAME "
                  "direction: part of the mesh is wound against the rest. Independent of "
                  "`inverted` in both directions - a uniformly inverted shell is perfectly "
                  "consistent, and a partially inverted one can still sum to a positive "
                  "volume - so neither check substitutes for the other.")},
            {ECheck::NotClosed, TEXT("not_closed"), ErrorCodes::ERR_MESH_AUDIT_NOT_CLOSED,
             ESeverity::Warning, true, false,
             TEXT("Edges adjacent to exactly one triangle. A warning, not an error: open is "
                  "CORRECT for a card, a plane or an authored shell, and the sweep cannot "
                  "tell an intended shell from a cut that broke through a wall. It is "
                  "reported because it is also what makes `inverted` unanswerable.")},
            {ECheck::DegenerateTriangles, TEXT("degenerate_triangles"),
             ErrorCodes::ERR_MESH_AUDIT_DEGENERATE, ESeverity::Warning, true, false,
             TEXT("Triangles with area below GeometryUtils::DegenerateAreaEpsilon. Common and "
                  "usually survivable - the static-mesh build's own bRemoveDegenerates welds "
                  "most away - so this matters as a TREND: a jump means two boolean operands "
                  "were placed to touch exactly rather than to overlap.")},
            {ECheck::NonManifoldVertices, TEXT("non_manifold"),
             ErrorCodes::ERR_MESH_AUDIT_NON_MANIFOLD, ESeverity::Warning, true, false,
             TEXT("Bowtie vertices: two or more triangle fans meeting at one vertex. "
                  "Non-manifold EDGES cannot exist in the dynamic-mesh representation the "
                  "measurement runs on, so the bowtie vertex is the detectable form.")},
            {ECheck::EmptyMesh, TEXT("empty"), ErrorCodes::ERR_MESH_AUDIT_EMPTY,
             ESeverity::Error, true, false,
             TEXT("The asset read back with zero triangles. Checked FIRST and reported on its "
                  "own terms, because a null mesh is closed, consistent and non-degenerate by "
                  "every other measurement in this table - it would otherwise be the cleanest "
                  "asset in the sweep.")},
            {ECheck::MirroredBuildScale, TEXT("mirrored_build_scale"),
             ErrorCodes::ERR_MESH_AUDIT_MIRRORED_BUILD_SCALE, ESeverity::Warning, true, false,
             TEXT("The asset's Build Scale has an odd number of negative axes, so the build "
                  "mirrors the mesh and inverts its winding. Names the CAUSE when `inverted` "
                  "fires on triangles that are themselves correctly wound. Reported "
                  "independently of `inverted` because the two can disagree: reading with "
                  "useBuildScale:false measures the unmirrored triangles.")},
            {ECheck::ThinShell, TEXT("thin_shell"), ErrorCodes::ERR_MESH_AUDIT_THIN_SHELL,
             ESeverity::Warning, false, true,
             TEXT("A closed mesh enclosing negligible volume against its own surface area - a "
                  "sheet folded back on itself. It passes `inverted` (the volume is not "
                  "negative) and `not_closed` (there are no boundary edges) while enclosing "
                  "nothing. Off by default: it needs a threshold, and legitimately flat "
                  "content trips it.")},
            {ECheck::ZFighting, TEXT("z_fighting"), ErrorCodes::ERR_MESH_AUDIT_Z_FIGHTING,
             ESeverity::Warning, true, false,
             TEXT("Overlapping near-coplanar triangles from different connected components. "
                  "The detector uses a spatial hash followed by normal, plane-distance and "
                  "projected-overlap tests, then groups fighting pairs into ranked regions. "
                  "It is a geometric proxy for depth-buffer precision, not a universal camera "
                  "prediction, so this warning does not replace a visual review.")},
            {ECheck::FloatingComponents, TEXT("floating_components"),
             ErrorCodes::ERR_MESH_AUDIT_FLOATING_COMPONENT, ESeverity::Warning, true, false,
             TEXT("A component island is spatially isolated from the largest individual "
                  "connected component by triangle count. Disconnected shells are normal; this "
                  "only warns when triangle-level distance exceeds a tolerance derived from the "
                  "model's bounding sphere. The finding reports each floater's size, centre and "
                  "nearest-component distance.")},
        };
        checkf(Checks.Num() == CheckCount, TEXT("MeshAudit check table is out of sync with ECheck."));
        return Checks;
    }

    // CheckInfo / ParseCheckId / CheckBit / HasCheck / DefaultCheckMask / AllCheckMask are the
    // shared contract's, inlined in the header over this table (Audit/AuditFramework.h).

    // ---- LOD vocabulary ---------------------------------------------------------------------

    bool ParseLodSource(const FString& Token, ELodSource& OutSource)
    {
        const FString Trimmed = Token.TrimStartAndEnd();
        if (Trimmed.IsEmpty() || Trimmed.Equals(TEXT("MaxAvailable"), ESearchCase::IgnoreCase))
        { OutSource = ELodSource::MaxAvailable; return true; }
        if (Trimmed.Equals(TEXT("HiResSourceModel"), ESearchCase::IgnoreCase))
        { OutSource = ELodSource::HiResSourceModel; return true; }
        if (Trimmed.Equals(TEXT("SourceModel"), ESearchCase::IgnoreCase))
        { OutSource = ELodSource::SourceModel; return true; }
        if (Trimmed.Equals(TEXT("RenderData"), ESearchCase::IgnoreCase))
        { OutSource = ELodSource::RenderData; return true; }
        return false;
    }

    const TCHAR* LodSourceToString(ELodSource Source)
    {
        switch (Source)
        {
        case ELodSource::HiResSourceModel: return TEXT("HiResSourceModel");
        case ELodSource::SourceModel:      return TEXT("SourceModel");
        case ELodSource::RenderData:       return TEXT("RenderData");
        case ELodSource::MaxAvailable:
        default:                           return TEXT("MaxAvailable");
        }
    }

    // ---- Derived measurements -----------------------------------------------------------------

    double FAssetMeasurement::VolumeRatio() const
    {
        // Scale-free by construction: volume grows as L^3 and Area^1.5 grows as L^3, so the
        // ratio is the same for a 10 cm bolt and a 100 m cliff and one threshold covers both.
        // Guarded rather than clamped - an area of zero means there are no triangles, which is
        // `empty`'s finding to report and not a thinness of any kind.
        if (SurfaceArea <= 0.0)
        {
            return 0.0;
        }
        return FMath::Abs(Health.SignedVolume) / FMath::Pow(SurfaceArea, 1.5);
    }

    void MeasureComponents(UDynamicMesh* Mesh, TArray<FComponentMeasurement>& OutComponents)
    {
        TArray<FZFightTriangle> UnusedTriangles;
        MeasureComponents(Mesh, OutComponents, UnusedTriangles);
    }

    void MeasureComponents(UDynamicMesh* Mesh, TArray<FComponentMeasurement>& OutComponents,
                           TArray<FZFightTriangle>& OutTriangles)
    {
        OutComponents.Reset();
        OutTriangles.Reset();
        if (!Mesh)
        {
            return;
        }

        const UE::Geometry::FDynamicMesh3& EditMesh = Mesh->GetMeshRef();
        UE::Geometry::FMeshConnectedComponents Connected(&EditMesh);
        Connected.FindConnectedTriangles();

        OutComponents.Reserve(Connected.Num());
        for (int32 ComponentIndex = 0; ComponentIndex < Connected.Num(); ++ComponentIndex)
        {
            const UE::Geometry::FMeshConnectedComponents::FComponent& Component =
                Connected.GetComponent(ComponentIndex);
            FComponentMeasurement Measurement;
            Measurement.Index = ComponentIndex;
            Measurement.TriangleCount = Component.Indices.Num();
            Measurement.TriangleIDs = Component.Indices;
            for (const int32 TriangleID : Component.Indices)
            {
                Measurement.FirstTriangleID =
                    Measurement.FirstTriangleID < 0
                        ? TriangleID
                        : FMath::Min(Measurement.FirstTriangleID, TriangleID);

                const UE::Geometry::FIndex3i TriangleEdges = EditMesh.GetTriEdges(TriangleID);
                for (int32 EdgeCorner = 0; EdgeCorner < 3; ++EdgeCorner)
                {
                    if (EditMesh.IsBoundaryEdge(TriangleEdges[EdgeCorner]))
                    {
                        ++Measurement.BoundaryEdges;
                    }
                }

                FVector3d V0, V1, V2;
                EditMesh.GetTriVertices(TriangleID, V0, V1, V2);
                const FVector3d Cross = FVector3d::CrossProduct(V1 - V0, V2 - V0);
                const double TriangleArea = 0.5 * Cross.Length();
                if (TriangleArea < GeometryUtils::DegenerateAreaEpsilon)
                {
                    ++Measurement.DegenerateTriangles;
                }
                Measurement.Bounds += FVector(V0);
                Measurement.Bounds += FVector(V1);
                Measurement.Bounds += FVector(V2);

                FZFightTriangle Triangle;
                Triangle.TriangleID = TriangleID;
                Triangle.ComponentIndex = ComponentIndex;
                Triangle.V0 = V0;
                Triangle.V1 = V1;
                Triangle.V2 = V2;
                Triangle.Area = TriangleArea;
                Triangle.Normal = Cross.GetSafeNormal();
                Triangle.BoundsMin = FVector3d(
                    FMath::Min3(V0.X, V1.X, V2.X), FMath::Min3(V0.Y, V1.Y, V2.Y),
                    FMath::Min3(V0.Z, V1.Z, V2.Z));
                Triangle.BoundsMax = FVector3d(
                    FMath::Max3(V0.X, V1.X, V2.X), FMath::Max3(V0.Y, V1.Y, V2.Y),
                    FMath::Max3(V0.Z, V1.Z, V2.Z));
                OutTriangles.Add(MoveTemp(Triangle));
            }

            const FVector2d VolumeArea =
                UE::Geometry::TMeshQueries<UE::Geometry::FDynamicMesh3>::GetVolumeArea(
                    EditMesh, Component.Indices);
            Measurement.SignedVolume = VolumeArea.X;
            Measurement.SurfaceArea = VolumeArea.Y;
            if (Measurement.Bounds.IsValid != 0)
            {
                Measurement.Center = Measurement.Bounds.GetCenter();
            }
            OutComponents.Add(MoveTemp(Measurement));
        }
    }

    namespace
    {
        struct FZFPoint2 { double X = 0.0; double Y = 0.0; };

        enum class EZFProjectionAxis : uint8
        {
            X,
            Y,
            Z
        };

        struct FZFAcceptedPair
        {
            int32 IndexA = -1;
            int32 IndexB = -1;
            double OverlapArea = 0.0;
            double PlaneDistance = 0.0;
            double AbsNormalDot = 0.0;
        };

        struct FZFQuantizedPoint
        {
            int64 X = 0;
            int64 Y = 0;

            bool operator==(const FZFQuantizedPoint& Other) const
            {
                return X == Other.X && Y == Other.Y;
            }

            friend uint32 GetTypeHash(const FZFQuantizedPoint& Point)
            {
                return HashCombine(::GetTypeHash(Point.X), ::GetTypeHash(Point.Y));
            }
        };

        struct FZFSegmentKey
        {
            FZFQuantizedPoint A;
            FZFQuantizedPoint B;

            bool operator==(const FZFSegmentKey& Other) const
            {
                return A == Other.A && B == Other.B;
            }

            friend uint32 GetTypeHash(const FZFSegmentKey& Segment)
            {
                return HashCombine(GetTypeHash(Segment.A), GetTypeHash(Segment.B));
            }
        };

        struct FZFGridRange
        {
            FIntVector Lo = FIntVector::ZeroValue;
            FIntVector Hi = FIntVector::ZeroValue;
            int32 CellCount = 0;
        };

        double ZFArea2(const FZFPoint2& A, const FZFPoint2& B, const FZFPoint2& C)
        { return (B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X); }

        double ZFCross2(const FZFPoint2& A, const FZFPoint2& B)
        {
            return A.X * B.Y - A.Y * B.X;
        }

        double ZFDot2(const FZFPoint2& A, const FZFPoint2& B)
        {
            return A.X * B.X + A.Y * B.Y;
        }

        double ZFPolygonSignedArea(const TArray<FZFPoint2>& Polygon)
        {
            double Area2 = 0.0;
            for (int32 Index = 0; Index < Polygon.Num(); ++Index)
            {
                Area2 += ZFCross2(Polygon[Index], Polygon[(Index + 1) % Polygon.Num()]);
            }
            return Area2 * 0.5;
        }

        EZFProjectionAxis ZFChooseProjectionAxis(const FVector3d& Normal)
        {
            const double AX = FMath::Abs(Normal.X);
            const double AY = FMath::Abs(Normal.Y);
            const double AZ = FMath::Abs(Normal.Z);
            return AX >= AY && AX >= AZ ? EZFProjectionAxis::X
                : AY >= AZ ? EZFProjectionAxis::Y : EZFProjectionAxis::Z;
        }

        FZFPoint2 ZFProject(const FVector3d& Vertex, EZFProjectionAxis Axis)
        {
            switch (Axis)
            {
            case EZFProjectionAxis::X: return {Vertex.Y, Vertex.Z};
            case EZFProjectionAxis::Y: return {Vertex.X, Vertex.Z};
            case EZFProjectionAxis::Z:
            default:                   return {Vertex.X, Vertex.Y};
            }
        }

        double ZFProjectionScale(const FVector3d& Normal, EZFProjectionAxis Axis)
        {
            switch (Axis)
            {
            case EZFProjectionAxis::X: return FMath::Abs(Normal.X);
            case EZFProjectionAxis::Y: return FMath::Abs(Normal.Y);
            case EZFProjectionAxis::Z:
            default:                   return FMath::Abs(Normal.Z);
            }
        }

        TArray<FZFPoint2> ZFClip(const TArray<FZFPoint2>& Subject, const FZFPoint2& A,
                                 const FZFPoint2& B)
        {
            TArray<FZFPoint2> Result;
            if (Subject.Num() == 0)
            {
                return Result;
            }
            for (int32 I = 0; I < Subject.Num(); ++I)
            {
                const FZFPoint2 P = Subject[I], Q = Subject[(I + 1) % Subject.Num()];
                const double DP = ZFArea2(A, B, P);
                const double DQ = ZFArea2(A, B, Q);
                const bool bPInside = DP >= -1.0e-12;
                const bool bQInside = DQ >= -1.0e-12;
                if (bPInside)
                {
                    Result.Add(P);
                }
                if (bPInside != bQInside)
                {
                    const double Denominator = DP - DQ;
                    if (FMath::IsFinite(Denominator) && FMath::Abs(Denominator) > UE_DOUBLE_SMALL_NUMBER)
                    {
                        const double T = DP / Denominator;
                        Result.Add({P.X + (Q.X - P.X) * T,
                                    P.Y + (Q.Y - P.Y) * T});
                    }
                }
            }
            return Result;
        }

        bool ZFBuildProjectedOverlap(const FZFightTriangle& A, const FZFightTriangle& B,
                                      EZFProjectionAxis Axis, TArray<FZFPoint2>& OutPolygon)
        {
            OutPolygon.Reset();
            TArray<FZFPoint2> Clip{ZFProject(A.V0, Axis), ZFProject(A.V1, Axis),
                                   ZFProject(A.V2, Axis)};
            const double ClipSignedArea = ZFArea2(Clip[0], Clip[1], Clip[2]);
            if (!FMath::IsFinite(ClipSignedArea)
                || FMath::Abs(ClipSignedArea) <= UE_DOUBLE_SMALL_NUMBER)
            {
                return false;
            }
            // Sutherland-Hodgman half-plane tests below expect a counter-clockwise clip
            // polygon. Normalize A itself; changing B's winding cannot fix clockwise clip
            // edges and was the source of the previous order-dependent result.
            if (ClipSignedArea < 0.0)
            {
                Swap(Clip[1], Clip[2]);
            }

            TArray<FZFPoint2> Poly{ZFProject(B.V0, Axis), ZFProject(B.V1, Axis),
                                   ZFProject(B.V2, Axis)};
            Poly = ZFClip(Poly, Clip[0], Clip[1]);
            Poly = ZFClip(Poly, Clip[1], Clip[2]);
            Poly = ZFClip(Poly, Clip[2], Clip[0]);
            if (Poly.Num() < 3)
            {
                return false;
            }
            const double Area = ZFPolygonSignedArea(Poly);
            if (!FMath::IsFinite(Area) || FMath::Abs(Area) <= UE_DOUBLE_SMALL_NUMBER)
            {
                return false;
            }
            if (Area < 0.0)
            {
                for (int32 Left = 0, Right = Poly.Num() - 1; Left < Right; ++Left, --Right)
                {
                    Swap(Poly[Left], Poly[Right]);
                }
            }
            OutPolygon = MoveTemp(Poly);
            return true;
        }

        double ZFProjectedOverlap(const FZFightTriangle& A, const FZFightTriangle& B)
        {
            const EZFProjectionAxis Axis = ZFChooseProjectionAxis(A.Normal);
            const double ProjectionScale = ZFProjectionScale(A.Normal, Axis);
            TArray<FZFPoint2> Polygon;
            return ProjectionScale > SMALL_NUMBER
                       && ZFBuildProjectedOverlap(A, B, Axis, Polygon)
                ? FMath::Abs(ZFPolygonSignedArea(Polygon)) / ProjectionScale
                : 0.0;
        }

        void ZFAddParameter(TArray<double>& Parameters, double Value)
        {
            if (FMath::IsFinite(Value) && Value >= -1.0e-10 && Value <= 1.0 + 1.0e-10)
            {
                Parameters.Add(FMath::Clamp(Value, 0.0, 1.0));
            }
        }

        void ZFAddSegmentIntersectionParameters(const FZFPoint2& P0, const FZFPoint2& P1,
                                                const FZFPoint2& Q0, const FZFPoint2& Q1,
                                                double Tolerance, TArray<double>& PParameters,
                                                TArray<double>& QParameters)
        {
            const FZFPoint2 R{P1.X - P0.X, P1.Y - P0.Y};
            const FZFPoint2 S{Q1.X - Q0.X, Q1.Y - Q0.Y};
            const FZFPoint2 QMinusP{Q0.X - P0.X, Q0.Y - P0.Y};
            const double RLength = FMath::Sqrt(ZFDot2(R, R));
            const double SLength = FMath::Sqrt(ZFDot2(S, S));
            const double RCrossS = ZFCross2(R, S);
            const double ParallelTolerance =
                1.0e-12 * FMath::Max(1.0, RLength * SLength);
            if (FMath::Abs(RCrossS) > ParallelTolerance)
            {
                ZFAddParameter(PParameters, ZFCross2(QMinusP, S) / RCrossS);
                ZFAddParameter(QParameters, ZFCross2(QMinusP, R) / RCrossS);
                return;
            }

            if (RLength <= Tolerance || SLength <= Tolerance
                || FMath::Abs(ZFCross2(QMinusP, R))
                    > Tolerance * FMath::Max(1.0, RLength))
            {
                return;
            }

            const double RLengthSquared = ZFDot2(R, R);
            const double SLengthSquared = ZFDot2(S, S);
            const double PAtQ0 = ZFDot2(QMinusP, R) / RLengthSquared;
            const FZFPoint2 Q1MinusP{Q1.X - P0.X, Q1.Y - P0.Y};
            const double PAtQ1 = ZFDot2(Q1MinusP, R) / RLengthSquared;
            const FZFPoint2 P0MinusQ{P0.X - Q0.X, P0.Y - Q0.Y};
            const FZFPoint2 P1MinusQ{P1.X - Q0.X, P1.Y - Q0.Y};
            const double QAtP0 = ZFDot2(P0MinusQ, S) / SLengthSquared;
            const double QAtP1 = ZFDot2(P1MinusQ, S) / SLengthSquared;
            ZFAddParameter(PParameters, PAtQ0);
            ZFAddParameter(PParameters, PAtQ1);
            ZFAddParameter(QParameters, QAtP0);
            ZFAddParameter(QParameters, QAtP1);
        }

        bool ZFPointInsideConvex(const TArray<FZFPoint2>& Polygon, const FZFPoint2& Point,
                                 double Tolerance, bool bStrict)
        {
            bool bOnBoundary = false;
            for (int32 Index = 0; Index < Polygon.Num(); ++Index)
            {
                const FZFPoint2& A = Polygon[Index];
                const FZFPoint2& B = Polygon[(Index + 1) % Polygon.Num()];
                const FZFPoint2 Edge{B.X - A.X, B.Y - A.Y};
                const double EdgeLength = FMath::Sqrt(ZFDot2(Edge, Edge));
                const double Cross = ZFArea2(A, B, Point);
                const double CrossTolerance = Tolerance * FMath::Max(1.0, EdgeLength);
                if (Cross < -CrossTolerance)
                {
                    return false;
                }
                bOnBoundary |= FMath::Abs(Cross) <= CrossTolerance;
            }
            return !bStrict || !bOnBoundary;
        }

        FZFSegmentKey ZFMakeSegmentKey(const FZFPoint2& A, const FZFPoint2& B,
                                       double Tolerance)
        {
            FZFSegmentKey Key;
            Key.A = {FMath::RoundToInt64(A.X / Tolerance),
                     FMath::RoundToInt64(A.Y / Tolerance)};
            Key.B = {FMath::RoundToInt64(B.X / Tolerance),
                     FMath::RoundToInt64(B.Y / Tolerance)};
            if (Key.B.X < Key.A.X || (Key.B.X == Key.A.X && Key.B.Y < Key.A.Y))
            {
                Swap(Key.A, Key.B);
            }
            return Key;
        }

        // The broad phase keeps this work local to a fighting region. Split every convex
        // overlap polygon at its pairwise edge intersections, then keep only boundary segments
        // whose left side is inside the union and whose right side is outside. The signed
        // boundary integral is the unique area, so overlapping pair evidence cannot be counted
        // twice when three or more shells cover the same surface.
        bool ZFUnionArea(const TArray<TArray<FZFPoint2>>& InputPolygons, double Tolerance,
                         double& OutArea)
        {
            OutArea = 0.0;
            TArray<TArray<FZFPoint2>> Polygons;
            for (const TArray<FZFPoint2>& Input : InputPolygons)
            {
                if (Input.Num() < 3)
                {
                    continue;
                }
                TArray<FZFPoint2> Polygon = Input;
                if (ZFPolygonSignedArea(Polygon) < 0.0)
                {
                    for (int32 Left = 0, Right = Polygon.Num() - 1;
                         Left < Right; ++Left, --Right)
                    {
                        Swap(Polygon[Left], Polygon[Right]);
                    }
                }
                if (FMath::IsFinite(ZFPolygonSignedArea(Polygon)))
                {
                    Polygons.Add(MoveTemp(Polygon));
                }
            }
            if (Polygons.Num() == 0)
            {
                return false;
            }

            TArray<TArray<TArray<double>>> EdgeParameters;
            EdgeParameters.SetNum(Polygons.Num());
            for (int32 PolygonIndex = 0; PolygonIndex < Polygons.Num(); ++PolygonIndex)
            {
                EdgeParameters[PolygonIndex].SetNum(Polygons[PolygonIndex].Num());
                for (TArray<double>& Parameters : EdgeParameters[PolygonIndex])
                {
                    Parameters.Add(0.0);
                    Parameters.Add(1.0);
                }
            }

            for (int32 FirstPolygon = 0; FirstPolygon < Polygons.Num(); ++FirstPolygon)
            {
                for (int32 SecondPolygon = FirstPolygon + 1;
                     SecondPolygon < Polygons.Num(); ++SecondPolygon)
                {
                    for (int32 FirstEdge = 0; FirstEdge < Polygons[FirstPolygon].Num();
                         ++FirstEdge)
                    {
                        const FZFPoint2 P0 = Polygons[FirstPolygon][FirstEdge];
                        const FZFPoint2 P1 = Polygons[FirstPolygon][
                            (FirstEdge + 1) % Polygons[FirstPolygon].Num()];
                        for (int32 SecondEdge = 0; SecondEdge < Polygons[SecondPolygon].Num();
                             ++SecondEdge)
                        {
                            ZFAddSegmentIntersectionParameters(
                                P0, P1, Polygons[SecondPolygon][SecondEdge],
                                Polygons[SecondPolygon][
                                    (SecondEdge + 1) % Polygons[SecondPolygon].Num()],
                                Tolerance, EdgeParameters[FirstPolygon][FirstEdge],
                                EdgeParameters[SecondPolygon][SecondEdge]);
                        }
                    }
                }
            }

            TSet<FZFSegmentKey> BoundarySegments;
            double BoundaryArea = 0.0;
            const double SampleOffset = FMath::Max(Tolerance * 10.0, 1.0e-9);
            for (int32 PolygonIndex = 0; PolygonIndex < Polygons.Num(); ++PolygonIndex)
            {
                const TArray<FZFPoint2>& Polygon = Polygons[PolygonIndex];
                for (int32 EdgeIndex = 0; EdgeIndex < Polygon.Num(); ++EdgeIndex)
                {
                    TArray<double>& Parameters = EdgeParameters[PolygonIndex][EdgeIndex];
                    Parameters.Sort();
                    for (int32 ParameterIndex = 0; ParameterIndex + 1 < Parameters.Num();
                         ++ParameterIndex)
                    {
                        const double T0 = Parameters[ParameterIndex];
                        const double T1 = Parameters[ParameterIndex + 1];
                        if (T1 - T0 <= 1.0e-10)
                        {
                            continue;
                        }
                        const FZFPoint2 A = Polygon[EdgeIndex];
                        const FZFPoint2 B = Polygon[(EdgeIndex + 1) % Polygon.Num()];
                        const FZFPoint2 Segment{B.X - A.X, B.Y - A.Y};
                        const double Length = FMath::Sqrt(ZFDot2(Segment, Segment));
                        if (!FMath::IsFinite(Length) || Length <= Tolerance)
                        {
                            continue;
                        }
                        const double MidT = (T0 + T1) * 0.5;
                        const FZFPoint2 Mid{A.X + Segment.X * MidT,
                                            A.Y + Segment.Y * MidT};
                        const FZFPoint2 UnitLeft{-Segment.Y / Length, Segment.X / Length};
                        const FZFPoint2 LeftSample{Mid.X + UnitLeft.X * SampleOffset,
                                                   Mid.Y + UnitLeft.Y * SampleOffset};
                        const FZFPoint2 RightSample{Mid.X - UnitLeft.X * SampleOffset,
                                                    Mid.Y - UnitLeft.Y * SampleOffset};
                        bool bLeftInside = false;
                        bool bRightInside = false;
                        for (const TArray<FZFPoint2>& Other : Polygons)
                        {
                            bLeftInside |= ZFPointInsideConvex(
                                Other, LeftSample, Tolerance, false);
                            bRightInside |= ZFPointInsideConvex(
                                Other, RightSample, Tolerance, false);
                        }
                        if (!bLeftInside || bRightInside)
                        {
                            continue;
                        }

                        const FZFPoint2 Start{A.X + Segment.X * T0,
                                             A.Y + Segment.Y * T0};
                        const FZFPoint2 End{A.X + Segment.X * T1,
                                           A.Y + Segment.Y * T1};
                        const FZFSegmentKey Key = ZFMakeSegmentKey(Start, End, Tolerance);
                        if (BoundarySegments.Contains(Key))
                        {
                            continue;
                        }
                        BoundarySegments.Add(Key);
                        BoundaryArea += ZFCross2(Start, End) * 0.5;
                    }
                }
            }

            OutArea = FMath::Abs(BoundaryArea);
            return FMath::IsFinite(OutArea) && OutArea > UE_DOUBLE_SMALL_NUMBER;
        }

        bool ZFIsFiniteVector(const FVector3d& Value)
        {
            return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y)
                && FMath::IsFinite(Value.Z);
        }

        bool ZFIsValidTriangle(const FZFightTriangle& Triangle)
        {
            const double NormalLengthSquared = Triangle.Normal.SizeSquared();
            return FMath::IsFinite(Triangle.Area)
                && Triangle.Area >= GeometryUtils::DegenerateAreaEpsilon
                && FMath::IsFinite(NormalLengthSquared)
                && NormalLengthSquared >= 0.999 && NormalLengthSquared <= 1.001
                && ZFIsFiniteVector(Triangle.V0) && ZFIsFiniteVector(Triangle.V1)
                && ZFIsFiniteVector(Triangle.V2)
                && ZFIsFiniteVector(Triangle.Normal)
                && ZFIsFiniteVector(Triangle.BoundsMin)
                && ZFIsFiniteVector(Triangle.BoundsMax)
                && Triangle.BoundsMin.X <= Triangle.BoundsMax.X
                && Triangle.BoundsMin.Y <= Triangle.BoundsMax.Y
                && Triangle.BoundsMin.Z <= Triangle.BoundsMax.Z;
        }

        bool ZFExpandedBoundsOverlap(const FZFightTriangle& A, const FZFightTriangle& B,
                                     double Expansion)
        {
            return A.BoundsMin.X - Expansion <= B.BoundsMax.X
                && B.BoundsMin.X - Expansion <= A.BoundsMax.X
                && A.BoundsMin.Y - Expansion <= B.BoundsMax.Y
                && B.BoundsMin.Y - Expansion <= A.BoundsMax.Y
                && A.BoundsMin.Z - Expansion <= B.BoundsMax.Z
                && B.BoundsMin.Z - Expansion <= A.BoundsMax.Z;
        }

        FZFGridRange ZFMakeGridRange(const FZFightTriangle& Triangle, const FVector3d& Origin,
                                     double CellSize, int32 Resolution, double Expansion)
        {
            auto Coordinate = [CellSize, Resolution](double Value, double AxisOrigin)
            {
                return FMath::Clamp(FMath::FloorToInt((Value - AxisOrigin) / CellSize),
                                    0, Resolution - 1);
            };

            FZFGridRange Range;
            Range.Lo = FIntVector(
                Coordinate(Triangle.BoundsMin.X - Expansion, Origin.X),
                Coordinate(Triangle.BoundsMin.Y - Expansion, Origin.Y),
                Coordinate(Triangle.BoundsMin.Z - Expansion, Origin.Z));
            Range.Hi = FIntVector(
                Coordinate(Triangle.BoundsMax.X + Expansion, Origin.X),
                Coordinate(Triangle.BoundsMax.Y + Expansion, Origin.Y),
                Coordinate(Triangle.BoundsMax.Z + Expansion, Origin.Z));
            const int64 Count = int64(Range.Hi.X - Range.Lo.X + 1)
                * int64(Range.Hi.Y - Range.Lo.Y + 1)
                * int64(Range.Hi.Z - Range.Lo.Z + 1);
            Range.CellCount = Count <= MAX_int32 ? static_cast<int32>(Count) : MAX_int32;
            return Range;
        }

        double ZFMaxDistanceToPlane(const FZFightTriangle& Triangle,
                                    const FVector3d& PlanePoint, const FVector3d& PlaneNormal)
        {
            return FMath::Max3(
                FMath::Abs(FVector3d::DotProduct(Triangle.V0 - PlanePoint, PlaneNormal)),
                FMath::Abs(FVector3d::DotProduct(Triangle.V1 - PlanePoint, PlaneNormal)),
                FMath::Abs(FVector3d::DotProduct(Triangle.V2 - PlanePoint, PlaneNormal)));
        }

        double MeshAuditBoxGap(const FBox& A, const FBox& B)
        {
            if (!A.IsValid || !B.IsValid)
            {
                return TNumericLimits<double>::Max();
            }
            const double DX = A.Max.X < B.Min.X ? B.Min.X - A.Max.X
                : B.Max.X < A.Min.X ? A.Min.X - B.Max.X : 0.0;
            const double DY = A.Max.Y < B.Min.Y ? B.Min.Y - A.Max.Y
                : B.Max.Y < A.Min.Y ? A.Min.Y - B.Max.Y : 0.0;
            const double DZ = A.Max.Z < B.Min.Z ? B.Min.Z - A.Max.Z
                : B.Max.Z < A.Min.Z ? A.Min.Z - B.Max.Z : 0.0;
            return FMath::Sqrt(DX * DX + DY * DY + DZ * DZ);
        }

        int32 MeshAuditFindRoot(TArray<int32>& Parent, int32 Value)
        {
            int32 Root = Value;
            while (Parent[Root] != Root)
            {
                Root = Parent[Root];
            }
            while (Parent[Value] != Value)
            {
                const int32 Next = Parent[Value];
                Parent[Value] = Root;
                Value = Next;
            }
            return Root;
        }
    }

    void AnalyzeZFighting(const TArray<FZFightTriangle>& Triangles, const FThresholds& Thresholds,
                          FZFightAnalysis& Out)
    {
        Out = FZFightAnalysis();
        Out.ModelTriangleCount = Triangles.Num();
        auto SetUnrunnable = [&Out](const FString& Reason)
        {
            Out.bUnrunnable = true;
            Out.UnrunnableCode = ErrorCodes::ERR_MESH_AUDIT_Z_FIGHTING_UNRUNNABLE;
            Out.UnrunnableReason = Reason;
        };

        if (!FMath::IsFinite(Thresholds.ZFightPlaneExtentFraction)
            || Thresholds.ZFightPlaneExtentFraction < 0.0
            || !FMath::IsFinite(Thresholds.ZFightNormalDotThreshold))
        {
            SetUnrunnable(TEXT("The z-fighting thresholds were non-finite or negative."));
            return;
        }

        FBox Bounds(ForceInit);
        TArray<uint8> Valid;
        Valid.Init(0, Triangles.Num());
        for (int32 Index = 0; Index < Triangles.Num(); ++Index)
        {
            if (!ZFIsValidTriangle(Triangles[Index]))
            {
                continue;
            }
            Valid[Index] = 1;
            Bounds += FVector(Triangles[Index].BoundsMin);
            Bounds += FVector(Triangles[Index].BoundsMax);
            ++Out.ValidTriangleCount;
        }
        if (!Bounds.IsValid || Out.ValidTriangleCount == 0)
        {
            SetUnrunnable(FString::Printf(
                TEXT("No finite, non-degenerate triangles were available (%d triangle(s) read)."),
                Out.ModelTriangleCount));
            return;
        }

        Out.ModelExtent = FMath::Max3(Bounds.GetSize().X, Bounds.GetSize().Y, Bounds.GetSize().Z);
        if (!FMath::IsFinite(Out.ModelExtent) || Out.ModelExtent <= 0.0)
        {
            SetUnrunnable(TEXT("The finite triangle set had no positive model extent."));
            return;
        }

        Out.PlaneDistanceEpsilon =
            Out.ModelExtent * Thresholds.ZFightPlaneExtentFraction;
        Out.NormalDotThreshold = FMath::Clamp(Thresholds.ZFightNormalDotThreshold, 0.0, 1.0);
        Out.GridCellSize = Out.ModelExtent / static_cast<double>(ZFightGridResolution);
        const double LargeCellSize =
            Out.ModelExtent / static_cast<double>(ZFightLargeGridResolution);
        const FVector3d GridOrigin(Bounds.Min);

        TMap<FIntVector, TArray<int32>> FineGrid;
        TMap<FIntVector, TArray<int32>> CoarseGrid;
        TArray<int32> LargeTriangles;
        for (int32 I = 0; I < Triangles.Num(); ++I)
        {
            if (!Valid[I])
            {
                continue;
            }

            const FZFGridRange CoarseRange = ZFMakeGridRange(
                Triangles[I], GridOrigin, LargeCellSize, ZFightLargeGridResolution,
                Out.PlaneDistanceEpsilon);
            for (int32 X = CoarseRange.Lo.X; X <= CoarseRange.Hi.X; ++X)
            for (int32 Y = CoarseRange.Lo.Y; Y <= CoarseRange.Hi.Y; ++Y)
            for (int32 Z = CoarseRange.Lo.Z; Z <= CoarseRange.Hi.Z; ++Z)
            {
                CoarseGrid.FindOrAdd(FIntVector(X, Y, Z)).Add(I);
                ++Out.GridReferenceCount;
            }

            const FZFGridRange FineRange = ZFMakeGridRange(
                Triangles[I], GridOrigin, Out.GridCellSize, ZFightGridResolution,
                Out.PlaneDistanceEpsilon);
            if (FineRange.CellCount > ZFightMaxFineCellsPerTriangle)
            {
                LargeTriangles.Add(I);
                continue;
            }
            for (int32 X = FineRange.Lo.X; X <= FineRange.Hi.X; ++X)
            for (int32 Y = FineRange.Lo.Y; Y <= FineRange.Hi.Y; ++Y)
            for (int32 Z = FineRange.Lo.Z; Z <= FineRange.Hi.Z; ++Z)
            {
                FineGrid.FindOrAdd(FIntVector(X, Y, Z)).Add(I);
                ++Out.GridReferenceCount;
            }
        }
        Out.LargeTriangleCount = LargeTriangles.Num();

        const int64 CandidateBudget64 = FMath::Max<int64>(
            ZFightMaxCandidatePairsPerTriangle,
            int64(Out.ValidTriangleCount) * ZFightMaxCandidatePairsPerTriangle);
        const int32 CandidateBudget = static_cast<int32>(
            FMath::Min<int64>(CandidateBudget64, MAX_int32));
        TSet<uint64> CandidateKeys;
        auto AddCandidate = [&](int32 A, int32 B) -> bool
        {
            if (A == B || !Valid.IsValidIndex(A) || !Valid.IsValidIndex(B)
                || !Valid[A] || !Valid[B]
                || Triangles[A].ComponentIndex == Triangles[B].ComponentIndex)
            {
                return true;
            }
            if (A > B)
            {
                Swap(A, B);
            }
            if (!ZFExpandedBoundsOverlap(
                    Triangles[A], Triangles[B], Out.PlaneDistanceEpsilon))
            {
                return true;
            }
            const uint64 Key = (uint64(uint32(A)) << 32) | uint32(B);
            if (!CandidateKeys.Contains(Key) && CandidateKeys.Num() >= CandidateBudget)
            {
                return false;
            }
            CandidateKeys.Add(Key);
            return true;
        };

        for (const TPair<FIntVector, TArray<int32>>& Cell : FineGrid)
        {
            if (Cell.Value.Num() > ZFightMaxTrianglesPerFineCell)
            {
                Out.CandidatePairCount = CandidateKeys.Num();
                SetUnrunnable(FString::Printf(
                    TEXT("A fine-grid cell contained %d triangles, above the bounded per-cell "
                         "limit of %d; the detector refused to enter an unbounded dense-cell "
                         "all-pairs loop."),
                    Cell.Value.Num(), ZFightMaxTrianglesPerFineCell));
                return;
            }
            for (int32 AIndex = 0; AIndex < Cell.Value.Num(); ++AIndex)
            {
                for (int32 BIndex = AIndex + 1; BIndex < Cell.Value.Num(); ++BIndex)
                {
                    if (!AddCandidate(Cell.Value[AIndex], Cell.Value[BIndex]))
                    {
                        Out.CandidatePairCount = CandidateKeys.Num();
                        SetUnrunnable(FString::Printf(
                            TEXT("The fine-grid candidate budget (%d) was exceeded; the detector "
                                 "refused to report a partial clean result."), CandidateBudget));
                        return;
                    }
                }
            }
        }

        for (const int32 LargeIndex : LargeTriangles)
        {
            const FZFGridRange CoarseRange = ZFMakeGridRange(
                Triangles[LargeIndex], GridOrigin, LargeCellSize,
                ZFightLargeGridResolution, Out.PlaneDistanceEpsilon);
            TSet<int32> LargeCandidates;
            int32 InspectedReferencesForTriangle = 0;
            bool bReferenceOverflow = false;
            bool bCandidateOverflow = false;
            for (int32 X = CoarseRange.Lo.X;
                 X <= CoarseRange.Hi.X && !bReferenceOverflow && !bCandidateOverflow; ++X)
            for (int32 Y = CoarseRange.Lo.Y;
                 Y <= CoarseRange.Hi.Y && !bReferenceOverflow && !bCandidateOverflow; ++Y)
            for (int32 Z = CoarseRange.Lo.Z;
                 Z <= CoarseRange.Hi.Z && !bReferenceOverflow && !bCandidateOverflow; ++Z)
            {
                const TArray<int32>* Bucket = CoarseGrid.Find(FIntVector(X, Y, Z));
                if (!Bucket)
                {
                    continue;
                }
                for (const int32 Candidate : *Bucket)
                {
                    ++InspectedReferencesForTriangle;
                    ++Out.LargeReferenceInspectCount;
                    Out.MaxLargeReferenceInspectCount = FMath::Max(
                        Out.MaxLargeReferenceInspectCount, InspectedReferencesForTriangle);
                    if (InspectedReferencesForTriangle
                        > ZFightMaxCoarseReferencesPerLargeTriangle)
                    {
                        bReferenceOverflow = true;
                        break;
                    }
                    if (Candidate == LargeIndex
                        || Triangles[Candidate].ComponentIndex
                            == Triangles[LargeIndex].ComponentIndex
                        || !ZFExpandedBoundsOverlap(Triangles[LargeIndex], Triangles[Candidate],
                                                    Out.PlaneDistanceEpsilon))
                    {
                        continue;
                    }
                    LargeCandidates.Add(Candidate);
                    if (LargeCandidates.Num() > ZFightMaxLargeCandidatesPerTriangle)
                    {
                        bCandidateOverflow = true;
                        break;
                    }
                }
            }
            if (bReferenceOverflow)
            {
                Out.CandidatePairCount = CandidateKeys.Num();
                SetUnrunnable(FString::Printf(
                    TEXT("Large triangle %d inspected more than the bounded coarse-grid "
                         "reference limit (%d); the detector refused to scan a dense fallback "
                         "bucket or report a partial clean result."),
                    Triangles[LargeIndex].TriangleID,
                    ZFightMaxCoarseReferencesPerLargeTriangle));
                return;
            }
            if (bCandidateOverflow)
            {
                Out.CandidatePairCount = CandidateKeys.Num();
                SetUnrunnable(FString::Printf(
                    TEXT("Large triangle %d exceeded the bounded fallback candidate limit (%d); "
                         "the detector refused to report a partial clean result."),
                    Triangles[LargeIndex].TriangleID,
                    ZFightMaxLargeCandidatesPerTriangle));
                return;
            }

            TArray<int32> OrderedLargeCandidates;
            OrderedLargeCandidates.Reserve(LargeCandidates.Num());
            for (const int32 Candidate : LargeCandidates)
            {
                OrderedLargeCandidates.Add(Candidate);
            }
            OrderedLargeCandidates.Sort();
            for (const int32 Candidate : OrderedLargeCandidates)
            {
                if (!AddCandidate(LargeIndex, Candidate))
                {
                    Out.CandidatePairCount = CandidateKeys.Num();
                    SetUnrunnable(FString::Printf(
                        TEXT("The candidate-pair budget (%d) was exceeded; the detector refused "
                             "to report a partial clean result."), CandidateBudget));
                    return;
                }
            }
        }

        TArray<uint64> OrderedCandidateKeys;
        OrderedCandidateKeys.Reserve(CandidateKeys.Num());
        for (const uint64 Key : CandidateKeys)
        {
            OrderedCandidateKeys.Add(Key);
        }
        OrderedCandidateKeys.Sort();
        Out.CandidatePairCount = OrderedCandidateKeys.Num();

        TArray<FZFAcceptedPair> AcceptedPairs;
        TArray<int32> Parent;
        Parent.SetNum(Triangles.Num());
        for (int32 I = 0; I < Parent.Num(); ++I)
        {
            Parent[I] = I;
        }
        auto Find = [&Parent](int32 Value)
        {
            while (Parent[Value] != Value)
            {
                Parent[Value] = Parent[Parent[Value]];
                Value = Parent[Value];
            }
            return Value;
        };
        auto Union = [&Find, &Parent](int32 A, int32 B)
        {
            const int32 RootA = Find(A);
            const int32 RootB = Find(B);
            if (RootA != RootB)
            {
                Parent[RootB] = RootA;
            }
        };

        for (const uint64 Key : OrderedCandidateKeys)
        {
            const int32 IndexA = static_cast<int32>(Key >> 32);
            const int32 IndexB = static_cast<int32>(Key & 0xffffffffu);
            const FZFightTriangle& A = Triangles[IndexA];
            const FZFightTriangle& B = Triangles[IndexB];
            ++Out.ExactOverlapTestCount;

            const double Dot = FMath::Abs(FVector3d::DotProduct(A.Normal, B.Normal));
            if (!FMath::IsFinite(Dot) || Dot < Out.NormalDotThreshold)
            {
                continue;
            }

            const double DistanceAtoB = ZFMaxDistanceToPlane(A, B.V0, B.Normal);
            const double DistanceBtoA = ZFMaxDistanceToPlane(B, A.V0, A.Normal);
            const double PlaneDistance = FMath::Max(DistanceAtoB, DistanceBtoA);
            if (!FMath::IsFinite(PlaneDistance)
                || PlaneDistance > Out.PlaneDistanceEpsilon)
            {
                continue;
            }

            const double Area = ZFProjectedOverlap(A, B);
            if (!FMath::IsFinite(Area) || Area <= UE_DOUBLE_SMALL_NUMBER)
            {
                continue;
            }

            FZFAcceptedPair& Accepted = AcceptedPairs.AddDefaulted_GetRef();
            Accepted.IndexA = IndexA;
            Accepted.IndexB = IndexB;
            Accepted.OverlapArea = Area;
            Accepted.PlaneDistance = PlaneDistance;
            Accepted.AbsNormalDot = Dot;

            Union(IndexA, IndexB);
        }
        Out.FightingPairCount = AcceptedPairs.Num();

        // Accepted pairs are already unique candidate survivors. Build a small vertex hash
        // over only their triangles, then union pair findings whose first and second source
        // triangles each share a vertex with the corresponding triangle of another pair. The
        // component checks keep duplicate surfaces from unrelated parts separate; transverse
        // pairs never enter AcceptedPairs because the exact normal test above rejects them.
        auto MakePairKey = [](int32 A, int32 B) -> uint64
        {
            if (A > B)
            {
                Swap(A, B);
            }
            return (uint64(uint32(A)) << 32) | uint32(B);
        };

        TMap<uint64, int32> AcceptedPairByKey;
        TArray<uint8> FightingTriangle;
        FightingTriangle.Init(0, Triangles.Num());
        for (int32 PairIndex = 0; PairIndex < AcceptedPairs.Num(); ++PairIndex)
        {
            const FZFAcceptedPair& Pair = AcceptedPairs[PairIndex];
            AcceptedPairByKey.Add(MakePairKey(Pair.IndexA, Pair.IndexB), PairIndex);
            FightingTriangle[Pair.IndexA] = 1;
            FightingTriangle[Pair.IndexB] = 1;
        }

        const double AdjacencyTolerance =
            FMath::Max(Out.ModelExtent * 1.0e-8, UE_DOUBLE_SMALL_NUMBER);
        const FVector3d AdjacencyOrigin(Bounds.Min);
        auto VertexKey = [AdjacencyOrigin, AdjacencyTolerance](const FVector3d& Vertex)
        {
            return FIntVector(
                FMath::RoundToInt((Vertex.X - AdjacencyOrigin.X) / AdjacencyTolerance),
                FMath::RoundToInt((Vertex.Y - AdjacencyOrigin.Y) / AdjacencyTolerance),
                FMath::RoundToInt((Vertex.Z - AdjacencyOrigin.Z) / AdjacencyTolerance));
        };

        TMap<FIntVector, TArray<int32>> VertexBuckets;
        for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
        {
            if (!FightingTriangle[TriangleIndex])
            {
                continue;
            }
            const FZFightTriangle& Triangle = Triangles[TriangleIndex];
            const FVector3d Vertices[] = {Triangle.V0, Triangle.V1, Triangle.V2};
            for (const FVector3d& Vertex : Vertices)
            {
                VertexBuckets.FindOrAdd(VertexKey(Vertex)).AddUnique(TriangleIndex);
            }
        }

        auto GatherAdjacentTriangles = [&VertexBuckets, &VertexKey, &Triangles](
            int32 TriangleIndex, TArray<int32>& OutAdjacent)
        {
            OutAdjacent.Reset();
            const FZFightTriangle& Triangle = Triangles[TriangleIndex];
            const FVector3d Vertices[] = {Triangle.V0, Triangle.V1, Triangle.V2};
            for (const FVector3d& Vertex : Vertices)
            {
                const TArray<int32>* Bucket = VertexBuckets.Find(VertexKey(Vertex));
                if (!Bucket)
                {
                    continue;
                }
                for (const int32 Candidate : *Bucket)
                {
                    if (Candidate != TriangleIndex
                        && Triangles[Candidate].ComponentIndex == Triangle.ComponentIndex
                        && !OutAdjacent.Contains(Candidate))
                    {
                        OutAdjacent.Add(Candidate);
                    }
                }
            }
        };

        for (int32 PairIndex = 0; PairIndex < AcceptedPairs.Num(); ++PairIndex)
        {
            const FZFAcceptedPair& Pair = AcceptedPairs[PairIndex];
            TArray<int32> AdjacentA;
            TArray<int32> AdjacentB;
            GatherAdjacentTriangles(Pair.IndexA, AdjacentA);
            GatherAdjacentTriangles(Pair.IndexB, AdjacentB);
            for (const int32 NeighborA : AdjacentA)
            {
                for (const int32 NeighborB : AdjacentB)
                {
                    const int32* NeighborPairIndex =
                        AcceptedPairByKey.Find(MakePairKey(NeighborA, NeighborB));
                    if (!NeighborPairIndex || *NeighborPairIndex <= PairIndex)
                    {
                        continue;
                    }

                    const FZFAcceptedPair& Neighbor = AcceptedPairs[*NeighborPairIndex];
                    const bool bDirectComponents =
                        Triangles[Neighbor.IndexA].ComponentIndex
                            == Triangles[Pair.IndexA].ComponentIndex
                        && Triangles[Neighbor.IndexB].ComponentIndex
                            == Triangles[Pair.IndexB].ComponentIndex;
                    const bool bSwappedComponents =
                        Triangles[Neighbor.IndexA].ComponentIndex
                            == Triangles[Pair.IndexB].ComponentIndex
                        && Triangles[Neighbor.IndexB].ComponentIndex
                            == Triangles[Pair.IndexA].ComponentIndex;
                    if (bDirectComponents || bSwappedComponents)
                    {
                        Union(Pair.IndexA, Neighbor.IndexA);
                    }
                }
            }
        }

        struct FRegionAccumulator
        {
            FZFightRegion Region;
            TSet<int32> TriangleIds;
            TSet<int32> ComponentIds;
            TArray<int32> PairIndices;
        };
        TArray<FRegionAccumulator> Accumulators;
        TMap<int32, int32> AccumulatorByRoot;
        TSet<int32> AllFightingTriangles;
        for (int32 PairIndex = 0; PairIndex < AcceptedPairs.Num(); ++PairIndex)
        {
            const FZFAcceptedPair& Pair = AcceptedPairs[PairIndex];
            const int32 Root = Find(Pair.IndexA);
            int32* ExistingIndex = AccumulatorByRoot.Find(Root);
            int32 AccumulatorIndex = INDEX_NONE;
            if (ExistingIndex)
            {
                AccumulatorIndex = *ExistingIndex;
            }
            else
            {
                AccumulatorIndex = Accumulators.AddDefaulted();
                AccumulatorByRoot.Add(Root, AccumulatorIndex);
            }

            FRegionAccumulator& Accumulator = Accumulators[AccumulatorIndex];
            Accumulator.Region.LargestPairOverlapArea = FMath::Max(
                Accumulator.Region.LargestPairOverlapArea, Pair.OverlapArea);
            ++Accumulator.Region.PairCount;
            Accumulator.PairIndices.Add(PairIndex);

            const FZFightTriangle& A = Triangles[Pair.IndexA];
            const FZFightTriangle& B = Triangles[Pair.IndexB];
            Accumulator.TriangleIds.Add(A.TriangleID);
            Accumulator.TriangleIds.Add(B.TriangleID);
            Accumulator.ComponentIds.Add(A.ComponentIndex);
            Accumulator.ComponentIds.Add(B.ComponentIndex);
            AllFightingTriangles.Add(A.TriangleID);
            AllFightingTriangles.Add(B.TriangleID);

            FZFightRegion::FPairEvidence& Evidence =
                Accumulator.Region.TopPairs.AddDefaulted_GetRef();
            Evidence.TriangleA = A.TriangleID;
            Evidence.TriangleB = B.TriangleID;
            Evidence.ComponentA = A.ComponentIndex;
            Evidence.ComponentB = B.ComponentIndex;
            Evidence.OverlapArea = Pair.OverlapArea;
            Evidence.PlaneDistance = Pair.PlaneDistance;
            Evidence.AbsNormalDot = Pair.AbsNormalDot;
        }

        for (FRegionAccumulator& Accumulator : Accumulators)
        {
            for (const int32 TriangleId : Accumulator.TriangleIds)
            {
                Accumulator.Region.TriangleIds.Add(TriangleId);
            }
            for (const int32 ComponentId : Accumulator.ComponentIds)
            {
                Accumulator.Region.ComponentIds.Add(ComponentId);
            }
            Accumulator.Region.TriangleIds.Sort();
            Accumulator.Region.ComponentIds.Sort();
            Accumulator.Region.TopPairs.Sort(
                [](const FZFightRegion::FPairEvidence& A,
                   const FZFightRegion::FPairEvidence& B)
                {
                    if (A.OverlapArea != B.OverlapArea)
                    {
                        return A.OverlapArea > B.OverlapArea;
                    }
                    if (A.TriangleA != B.TriangleA)
                    {
                        return A.TriangleA < B.TriangleA;
                    }
                    return A.TriangleB < B.TriangleB;
                });
            if (Accumulator.Region.TopPairs.Num() > ZFightMaxTopPairsPerRegion)
            {
                Accumulator.Region.TopPairs.SetNum(ZFightMaxTopPairsPerRegion);
            }

            if (Accumulator.PairIndices.Num() > ZFightMaxUnionPolygonsPerRegion)
            {
                SetUnrunnable(FString::Printf(
                    TEXT("A fighting region contained %d accepted pair polygons, above the "
                         "bounded unique-area union limit of %d."),
                    Accumulator.PairIndices.Num(), ZFightMaxUnionPolygonsPerRegion));
                return;
            }

            const FZFAcceptedPair& FirstPair = AcceptedPairs[Accumulator.PairIndices[0]];
            const FZFightTriangle& FirstTriangle = Triangles[FirstPair.IndexA];
            const EZFProjectionAxis ProjectionAxis =
                ZFChooseProjectionAxis(FirstTriangle.Normal);
            const double ProjectionScale =
                ZFProjectionScale(FirstTriangle.Normal, ProjectionAxis);
            if (ProjectionScale <= SMALL_NUMBER)
            {
                SetUnrunnable(TEXT("A fighting region had no usable projection scale."));
                return;
            }

            TArray<TArray<FZFPoint2>> OverlapPolygons;
            OverlapPolygons.Reserve(Accumulator.PairIndices.Num());
            for (const int32 PairIndex : Accumulator.PairIndices)
            {
                const FZFAcceptedPair& Pair = AcceptedPairs[PairIndex];
                TArray<FZFPoint2> Polygon;
                if (!ZFBuildProjectedOverlap(Triangles[Pair.IndexA], Triangles[Pair.IndexB],
                                              ProjectionAxis, Polygon))
                {
                    SetUnrunnable(TEXT("A fighting pair could not be reconstructed for region "
                                       "area union."));
                    return;
                }
                OverlapPolygons.Add(MoveTemp(Polygon));
            }

            double UniqueProjectedArea = 0.0;
            const double UnionTolerance =
                FMath::Max(Out.ModelExtent * 1.0e-9, 1.0e-9);
            if (!ZFUnionArea(OverlapPolygons, UnionTolerance, UniqueProjectedArea)
                || !FMath::IsFinite(UniqueProjectedArea))
            {
                SetUnrunnable(TEXT("A fighting region's projected overlap union was not "
                                   "finite and measurable."));
                return;
            }
            Accumulator.Region.OverlapArea = UniqueProjectedArea / ProjectionScale;
            if (!FMath::IsFinite(Accumulator.Region.OverlapArea))
            {
                SetUnrunnable(TEXT("A fighting region's unique overlap area was non-finite."));
                return;
            }
            Out.TotalOverlapArea += Accumulator.Region.OverlapArea;
            Out.Regions.Add(MoveTemp(Accumulator.Region));
        }

        Out.Regions.Sort([](const FZFightRegion& A, const FZFightRegion& B)
        {
            if (A.OverlapArea != B.OverlapArea)
            {
                return A.OverlapArea > B.OverlapArea;
            }
            const int32 FirstA = A.TriangleIds.Num() > 0 ? A.TriangleIds[0] : MAX_int32;
            const int32 FirstB = B.TriangleIds.Num() > 0 ? B.TriangleIds[0] : MAX_int32;
            return FirstA < FirstB;
        });
        for (int32 RegionIndex = 0; RegionIndex < Out.Regions.Num(); ++RegionIndex)
        {
            Out.Regions[RegionIndex].Rank = RegionIndex + 1;
        }
        Out.FightingTriangleCount = AllFightingTriangles.Num();
    }

    void MeasureSpatialProximity(UDynamicMesh* Mesh, FSpatialMeasurement& OutSpatial,
                                 TArray<FComponentMeasurement>& InOutComponents,
                                 double ToleranceFraction, double FixedLinkTolerance)
    {
        OutSpatial = FSpatialMeasurement();
        if (!Mesh)
        {
            return;
        }
        if (InOutComponents.Num() == 0 && Mesh->GetTriangleCount() > 0)
        {
            MeasureComponents(Mesh, InOutComponents);
        }

        const UE::Geometry::FAxisAlignedBox3d MeshBounds = Mesh->GetMeshRef().GetBounds();
        if (!MeshBounds.IsEmpty())
        {
            OutSpatial.Bounds = FBox(FVector(MeshBounds.Min), FVector(MeshBounds.Max));
            OutSpatial.BoundingSphereRadius = OutSpatial.Bounds.GetExtent().Size();
        }
        OutSpatial.bMeasured = true;

        const UE::Geometry::FDynamicMesh3& EditMesh = Mesh->GetMeshRef();
        TArray<TUniquePtr<UE::Geometry::FDynamicMeshAABBTree3>> Trees;
        Trees.Reserve(InOutComponents.Num());
        for (const FComponentMeasurement& Component : InOutComponents)
        {
            if (Component.TriangleIDs.Num() == 0)
            {
                Trees.Add(nullptr);
                continue;
            }
            TUniquePtr<UE::Geometry::FDynamicMeshAABBTree3> Tree =
                MakeUnique<UE::Geometry::FDynamicMeshAABBTree3>(&EditMesh, false);
            Tree->Build(Component.TriangleIDs);
            Trees.Add(MoveTemp(Tree));
        }

        const double Infinite = TNumericLimits<double>::Max();
        const double SafeToleranceFraction =
            FMath::IsFinite(ToleranceFraction) && ToleranceFraction >= 0.0
                ? ToleranceFraction : DefaultFloatingToleranceFraction;
        const double LinkTolerance = FMath::IsFinite(FixedLinkTolerance)
            && FixedLinkTolerance >= 0.0
                ? FixedLinkTolerance
                : OutSpatial.BoundingSphereRadius * SafeToleranceFraction;
        struct FPairCandidate
        {
            int32 ComponentA = INDEX_NONE;
            int32 ComponentB = INDEX_NONE;
            double LowerBound = TNumericLimits<double>::Max();
        };
        TArray<FPairCandidate> Candidates;
        for (int32 A = 0; A < InOutComponents.Num(); ++A)
        {
            if (!Trees[A].IsValid())
            {
                continue;
            }
            for (int32 B = A + 1; B < InOutComponents.Num(); ++B)
            {
                if (!Trees[B].IsValid())
                {
                    continue;
                }

                FPairCandidate& Candidate = Candidates.AddDefaulted_GetRef();
                Candidate.ComponentA = A;
                Candidate.ComponentB = B;
                Candidate.LowerBound = MeshAuditBoxGap(
                    InOutComponents[A].Bounds, InOutComponents[B].Bounds);
            }
        }
        Candidates.Sort([](const FPairCandidate& A, const FPairCandidate& B)
        {
            if (A.LowerBound != B.LowerBound)
            {
                return A.LowerBound < B.LowerBound;
            }
            if (A.ComponentA != B.ComponentA)
            {
                return A.ComponentA < B.ComponentA;
            }
            return A.ComponentB < B.ComponentB;
        });

        // Preserve every possible graph link. Beyond the link tolerance, query only while the
        // AABB lower bound can still improve either endpoint's exact nearest distance.
        TArray<double> NearestDistance;
        NearestDistance.Init(Infinite, InOutComponents.Num());
        for (const FPairCandidate& Candidate : Candidates)
        {
            const bool bCouldLink = Candidate.LowerBound <= LinkTolerance;
            const bool bCouldImproveNearest =
                Candidate.LowerBound <= NearestDistance[Candidate.ComponentA]
                || Candidate.LowerBound <= NearestDistance[Candidate.ComponentB];
            if (!bCouldLink && !bCouldImproveNearest)
            {
                continue;
            }

            UE::Geometry::IMeshSpatial::FQueryOptions QueryOptions;
            QueryOptions.MaxDistance = Infinite;
            double Distance = Infinite;
            const UE::Geometry::FIndex2i Pair = Trees[Candidate.ComponentA]->FindNearestTriangles(
                *Trees[Candidate.ComponentB], nullptr, Distance, QueryOptions);
            if (Pair == UE::Geometry::FIndex2i::Invalid() || !FMath::IsFinite(Distance))
            {
                continue;
            }

            FComponentPairDistance& Record = OutSpatial.PairDistances.AddDefaulted_GetRef();
            Record.ComponentA = Candidate.ComponentA;
            Record.ComponentB = Candidate.ComponentB;
            Record.Distance = Distance;
            NearestDistance[Candidate.ComponentA] =
                FMath::Min(NearestDistance[Candidate.ComponentA], Distance);
            NearestDistance[Candidate.ComponentB] =
                FMath::Min(NearestDistance[Candidate.ComponentB], Distance);
        }

        // A detached component can be closer to another detached component than to the body.
        // Keep the graph used for island classification, then add one exact distance from every
        // non-main component to the main proximity island. This makes the later finding metric
        // describe separation from the body rather than separation from an arbitrary sibling.
        TArray<int32> IslandParent;
        IslandParent.SetNumUninitialized(InOutComponents.Num());
        for (int32 Index = 0; Index < IslandParent.Num(); ++Index)
        {
            IslandParent[Index] = Index;
        }
        for (const FComponentPairDistance& Pair : OutSpatial.PairDistances)
        {
            if (Pair.Distance <= LinkTolerance
                && InOutComponents.IsValidIndex(Pair.ComponentA)
                && InOutComponents.IsValidIndex(Pair.ComponentB))
            {
                const int32 RootA = MeshAuditFindRoot(IslandParent, Pair.ComponentA);
                const int32 RootB = MeshAuditFindRoot(IslandParent, Pair.ComponentB);
                if (RootA != RootB)
                {
                    IslandParent[RootB] = RootA;
                }
            }
        }

        int32 LargestComponentIndex = INDEX_NONE;
        for (int32 Index = 0; Index < InOutComponents.Num(); ++Index)
        {
            if (LargestComponentIndex == INDEX_NONE
                || InOutComponents[Index].TriangleCount
                    > InOutComponents[LargestComponentIndex].TriangleCount
                || (InOutComponents[Index].TriangleCount
                        == InOutComponents[LargestComponentIndex].TriangleCount
                    && Index < LargestComponentIndex))
            {
                LargestComponentIndex = Index;
            }
        }

        if (LargestComponentIndex != INDEX_NONE)
        {
            const int32 MainRoot = MeshAuditFindRoot(IslandParent, LargestComponentIndex);
            TArray<int32> MainTriangleIDs;
            TMap<int32, int32> MainTriangleToComponent;
            for (int32 ComponentIndex = 0;
                 ComponentIndex < InOutComponents.Num();
                 ++ComponentIndex)
            {
                if (MeshAuditFindRoot(IslandParent, ComponentIndex) != MainRoot)
                {
                    continue;
                }
                for (const int32 TriangleID : InOutComponents[ComponentIndex].TriangleIDs)
                {
                    MainTriangleIDs.Add(TriangleID);
                    MainTriangleToComponent.Add(TriangleID, ComponentIndex);
                }
            }

            if (MainTriangleIDs.Num() > 0)
            {
                TUniquePtr<UE::Geometry::FDynamicMeshAABBTree3> MainTree =
                    MakeUnique<UE::Geometry::FDynamicMeshAABBTree3>(&EditMesh, false);
                MainTree->Build(MainTriangleIDs);
                UE::Geometry::IMeshSpatial::FQueryOptions QueryOptions;
                QueryOptions.MaxDistance = Infinite;
                for (int32 ComponentIndex = 0;
                     ComponentIndex < InOutComponents.Num();
                     ++ComponentIndex)
                {
                    if (MeshAuditFindRoot(IslandParent, ComponentIndex) == MainRoot
                        || !Trees[ComponentIndex].IsValid())
                    {
                        continue;
                    }

                    double Distance = Infinite;
                    const UE::Geometry::FIndex2i Pair = Trees[ComponentIndex]
                        ->FindNearestTriangles(*MainTree, nullptr, Distance, QueryOptions);
                    if (Pair == UE::Geometry::FIndex2i::Invalid() || !FMath::IsFinite(Distance))
                    {
                        continue;
                    }
                    const int32* MainComponent = MainTriangleToComponent.Find(Pair.B);
                    if (!MainComponent)
                    {
                        continue;
                    }

                    FComponentPairDistance& Record =
                        OutSpatial.PairDistances.AddDefaulted_GetRef();
                    Record.ComponentA = ComponentIndex;
                    Record.ComponentB = *MainComponent;
                    Record.Distance = Distance;
                }
            }
        }
    }

    void ClassifyFloatingComponents(const TArray<FComponentMeasurement>& Components,
                                    const FSpatialMeasurement& Spatial, double ToleranceFraction,
                                    FFloatingReport& OutReport, double FixedLinkTolerance)
    {
        OutReport = FFloatingReport();
        OutReport.bMeasured = Spatial.bMeasured;
        OutReport.ComponentCount = Components.Num();
        OutReport.ComponentIslandIds.SetNum(Components.Num());
        OutReport.ToleranceFraction = FMath::IsFinite(ToleranceFraction) && ToleranceFraction >= 0.0
            ? ToleranceFraction : DefaultFloatingToleranceFraction;
        OutReport.BoundingSphereRadius = Spatial.BoundingSphereRadius;
        OutReport.Tolerance = FMath::IsFinite(FixedLinkTolerance) && FixedLinkTolerance >= 0.0
            ? FixedLinkTolerance
            : OutReport.BoundingSphereRadius * OutReport.ToleranceFraction;
        const double Infinite = TNumericLimits<double>::Max();
        if (!Spatial.bMeasured || Components.Num() == 0)
        {
            return;
        }

        TArray<int32> Parent;
        Parent.SetNumUninitialized(Components.Num());
        for (int32 Index = 0; Index < Parent.Num(); ++Index)
        {
            Parent[Index] = Index;
        }

        for (const FComponentPairDistance& Pair : Spatial.PairDistances)
        {
            if (Pair.Distance <= OutReport.Tolerance
                && Components.IsValidIndex(Pair.ComponentA)
                && Components.IsValidIndex(Pair.ComponentB))
            {
                const int32 RootA = MeshAuditFindRoot(Parent, Pair.ComponentA);
                const int32 RootB = MeshAuditFindRoot(Parent, Pair.ComponentB);
                if (RootA != RootB)
                {
                    Parent[RootB] = RootA;
                }
            }
        }

        TSet<int32> Roots;
        for (int32 Index = 0; Index < Components.Num(); ++Index)
        {
            Roots.Add(MeshAuditFindRoot(Parent, Index));
        }
        OutReport.IslandCount = Roots.Num();

        // Anchor the main island on the largest individual edge-connected component. Several
        // small nearby components must not outweigh one larger component by aggregation.
        TMap<int32, int32> RootToIsland;
        TArray<int32> IslandTriangleCounts;
        TArray<int32> IslandRepresentatives;
        for (int32 Index = 0; Index < Components.Num(); ++Index)
        {
            const int32 Root = MeshAuditFindRoot(Parent, Index);
            int32 IslandIndex = INDEX_NONE;
            if (const int32* Existing = RootToIsland.Find(Root))
            {
                IslandIndex = *Existing;
            }
            else
            {
                IslandIndex = RootToIsland.Add(Root, IslandTriangleCounts.Num());
                IslandTriangleCounts.Add(0);
                IslandRepresentatives.Add(Index);
            }
            IslandTriangleCounts[IslandIndex] += FMath::Max(0, Components[Index].TriangleCount);
            IslandRepresentatives[IslandIndex] = FMath::Min(
                IslandRepresentatives[IslandIndex], Index);
        }

        for (int32 Index = 0; Index < Components.Num(); ++Index)
        {
            const int32 Root = MeshAuditFindRoot(Parent, Index);
            const int32 IslandIndex = RootToIsland.FindChecked(Root);
            OutReport.ComponentIslandIds[Index] = IslandRepresentatives[IslandIndex];
        }

        OutReport.LargestComponentIndex = INDEX_NONE;
        for (int32 Index = 0; Index < Components.Num(); ++Index)
        {
            if (OutReport.LargestComponentIndex == INDEX_NONE
                || Components[Index].TriangleCount
                    > Components[OutReport.LargestComponentIndex].TriangleCount
                || (Components[Index].TriangleCount
                        == Components[OutReport.LargestComponentIndex].TriangleCount
                    && Index < OutReport.LargestComponentIndex))
            {
                OutReport.LargestComponentIndex = Index;
            }
        }

        if (OutReport.LargestComponentIndex == INDEX_NONE)
        {
            return;
        }
        const int32 MainRoot = MeshAuditFindRoot(Parent, OutReport.LargestComponentIndex);
        const int32 MainIslandIndex = RootToIsland.FindChecked(MainRoot);
        OutReport.NonMainIslandCount = FMath::Max(0, OutReport.IslandCount - 1);
        OutReport.LargestIslandTriangleCount = IslandTriangleCounts[MainIslandIndex];

        TArray<double> Nearest;
        TArray<int32> NearestIndex;
        Nearest.Init(Infinite, Components.Num());
        NearestIndex.Init(-1, Components.Num());
        for (const FComponentPairDistance& Pair : Spatial.PairDistances)
        {
            if (Components.IsValidIndex(Pair.ComponentA) && Components.IsValidIndex(Pair.ComponentB))
            {
                const bool bAInMainIsland =
                    MeshAuditFindRoot(Parent, Pair.ComponentA) == MainRoot;
                const bool bBInMainIsland =
                    MeshAuditFindRoot(Parent, Pair.ComponentB) == MainRoot;
                if (bAInMainIsland == bBInMainIsland)
                {
                    continue;
                }
                if (Pair.Distance < Nearest[Pair.ComponentA])
                {
                    Nearest[Pair.ComponentA] = Pair.Distance;
                    NearestIndex[Pair.ComponentA] = Pair.ComponentB;
                }
                if (Pair.Distance < Nearest[Pair.ComponentB])
                {
                    Nearest[Pair.ComponentB] = Pair.Distance;
                    NearestIndex[Pair.ComponentB] = Pair.ComponentA;
                }
            }
        }

        for (int32 Index = 0; Index < Components.Num(); ++Index)
        {
            if (MeshAuditFindRoot(Parent, Index) == MainRoot)
            {
                continue;
            }
            FFloatingComponent& Floating = OutReport.Components.AddDefaulted_GetRef();
            Floating.ComponentIndex = Index;
            Floating.ProximityIslandId = OutReport.ComponentIslandIds[Index];
            Floating.TriangleCount = Components[Index].TriangleCount;
            Floating.SignedVolume = Components[Index].SignedVolume;
            Floating.Center = Components[Index].Center;
            Floating.NearestComponentIndex = NearestIndex[Index];
            Floating.NearestDistance = Nearest[Index];
        }
        OutReport.FloatingCount = OutReport.Components.Num();
        OutReport.FloatingComponentRows = OutReport.Components.Num();
        OutReport.SuppressedCount = 0;
        OutReport.UnsuppressedCount = OutReport.FloatingCount;
    }

#if WITH_EDITOR
    namespace
    {
        bool MeshAuditEvaluateSequencePose(const UAnimSequence& Sequence,
                                           const FFrameRate& FrameRate,
                                           FBoneContainer& RequiredBones,
                                           const FReferenceSkeleton& RefSkeleton,
                                           const TArray<FTransform>& RefLocal,
                                           const FFrameNumber& Frame,
                                           TArray<FTransform>& OutLocal,
                                           FString& OutFailureReason)
        {
            OutFailureReason.Reset();
            // FCompactPose, FBlendedCurve and FStackAttributeContainer all use the mem-stack
            // allocator. Mark each sampled pose so its temporary allocations are released before
            // the next frame instead of accumulating for the entire animation scan.
            FMemMark Mark(FMemStack::Get());
            FCompactPose CompactPose;
            CompactPose.SetBoneContainer(&RequiredBones);
            FBlendedCurve Curve;
            Curve.InitFrom(RequiredBones);
            UE::Anim::FStackAttributeContainer Attributes;
            FAnimationPoseData PoseData(CompactPose, Curve, Attributes);
            FAnimExtractContext Context(FrameRate.AsSeconds(Frame), false);
            if (Sequence.IsValidAdditive())
            {
                CompactPose.ResetToAdditiveIdentity();
                Sequence.GetAnimationPose(PoseData, Context);

                FCompactPose BasePose;
                BasePose.SetBoneContainer(&RequiredBones);
                FBlendedCurve BaseCurve;
                BaseCurve.InitFrom(RequiredBones);
                UE::Anim::FStackAttributeContainer BaseAttributes;
                FAnimationPoseData BasePoseData(BasePose, BaseCurve, BaseAttributes);
                Sequence.GetAdditiveBasePose(BasePoseData, Context);
                FAnimationRuntime::AccumulateAdditivePose(
                    BasePoseData, PoseData, 1.0f, Sequence.GetAdditiveAnimType());
                BasePose.NormalizeRotations();

                OutLocal = RefLocal;
                for (const FCompactPoseBoneIndex CompactIndex : BasePose.ForEachBoneIndex())
                {
                    const FSkeletonPoseBoneIndex SkeletonIndex =
                        RequiredBones.GetSkeletonPoseIndexFromCompactPoseIndex(CompactIndex);
                    if (SkeletonIndex.GetInt() >= 0 && OutLocal.IsValidIndex(SkeletonIndex.GetInt()))
                    {
                        OutLocal[SkeletonIndex.GetInt()] = BasePose[CompactIndex];
                    }
                }
            }
            else
            {
                const IAnimationDataModel* DataModel = Sequence.GetDataModel();
                TArray<FBoneAnimationTrack> SourceTracks;
                if (DataModel)
                {
                    TArray<FName> SourceTrackNames;
                    PRAGMA_DISABLE_DEPRECATION_WARNINGS
                    DataModel->GetBoneTrackNames(SourceTrackNames);
                    for (const FName& TrackName : SourceTrackNames)
                    {
                        FBoneAnimationTrack Track = DataModel->GetBoneTrackByName(TrackName);
                        // Some editor-only models expose a valid name list while their deprecated
                        // track lookup still returns an uninitialized name. Keep the name-list
                        // identity in that case; an explicitly stored replacement remains intact
                        // and is reported as an unknown track below.
                        if (Track.Name == NAME_None)
                        {
                            Track.Name = TrackName;
                        }
                        SourceTracks.Add(Track);
                    }
                    PRAGMA_ENABLE_DEPRECATION_WARNINGS
                }

                CompactPose.ResetToRefPose();
                Sequence.GetAnimationPose(PoseData, Context);

                // The engine evaluation above is authoritative: it applies the sequence's
                // retargeting and any model-side curve processing. A transient model can still
                // contain a track whose BoneTreeIndex has not been populated, in which case the
                // engine deliberately skips that track. Fill only those missing mappings; do not
                // overwrite a valid engine result with raw source-model data.
                if (DataModel)
                {
                    for (const FBoneAnimationTrack& Track : SourceTracks)
                    {
                        const int32 SkeletonBoneIndex = RefSkeleton.FindBoneIndex(Track.Name);
                        if (SkeletonBoneIndex == INDEX_NONE)
                        {
                            OutFailureReason = FString::Printf(
                                TEXT("Animation track '%s' is not present in the mesh skeleton."),
                                *Track.Name.ToString());
                            return false;
                        }

                        const FSkeletonPoseBoneIndex SkeletonPoseIndex(SkeletonBoneIndex);
                        const bool bTargetSkeletonIndexUsable =
                            RequiredBones.IsSkeletonPoseIndexValid(SkeletonPoseIndex);
                        const FCompactPoseBoneIndex CompactIndex = bTargetSkeletonIndexUsable
                            ? RequiredBones.GetCompactPoseIndexFromSkeletonPoseIndex(
                                SkeletonPoseIndex)
                            : FCompactPoseBoneIndex(INDEX_NONE);
                        const bool bTrackIndexUsable =
                            Track.BoneTreeIndex != INDEX_NONE
                            && RequiredBones.IsSkeletonPoseIndexValid(
                                FSkeletonPoseBoneIndex(Track.BoneTreeIndex));
                        const bool bMappingMissing =
                            !bTrackIndexUsable || Track.BoneTreeIndex != SkeletonBoneIndex;
                        if (bMappingMissing)
                        {
                            // Raw source-model fallback cannot reproduce a retargeted pose. A
                            // missing mapping therefore makes a retargeted sequence unrunnable;
                            // never replace its engine result with source-local transforms.
                            if (Sequence.RetargetSource != NAME_None)
                            {
                                OutFailureReason = FString::Printf(
                                    TEXT("Animation track '%s' has no valid mapping for the "
                                         "retargeted pose."),
                                    *Track.Name.ToString());
                                return false;
                            }
                            if (!bTargetSkeletonIndexUsable)
                            {
                                OutFailureReason = FString::Printf(
                                    TEXT("Animation track '%s' maps to a skeleton bone that is "
                                         "not in the evaluated pose."),
                                    *Track.Name.ToString());
                                return false;
                            }
                            if (!CompactIndex.IsValid())
                            {
                                OutFailureReason = FString::Printf(
                                    TEXT("Animation track '%s' has no valid compact-pose mapping."),
                                    *Track.Name.ToString());
                                return false;
                            }
                            CompactPose[CompactIndex] = DataModel->EvaluateBoneTrackTransform(
                                Track.Name, FFrameTime(Frame), Sequence.Interpolation);
                        }
                    }
                }

                OutLocal = RefLocal;
                for (const FCompactPoseBoneIndex CompactIndex : CompactPose.ForEachBoneIndex())
                {
                    const FSkeletonPoseBoneIndex SkeletonIndex =
                        RequiredBones.GetSkeletonPoseIndexFromCompactPoseIndex(CompactIndex);
                    if (SkeletonIndex.GetInt() >= 0 && OutLocal.IsValidIndex(SkeletonIndex.GetInt()))
                    {
                        OutLocal[SkeletonIndex.GetInt()] = CompactPose[CompactIndex];
                    }
                }
            }
            return OutLocal.Num() == RefSkeleton.GetNum();
        }

        bool MeshAuditBuildPosedSkeletalLOD(const FSkeletalMeshLODModel& LOD,
                                             const TArray<FTransform>& ReferenceSpace,
                                             const TArray<FTransform>& PoseSpace,
                                             UDynamicMesh*& OutMesh)
        {
            OutMesh = NewObject<UDynamicMesh>(GetTransientPackage());
            if (!OutMesh || ReferenceSpace.Num() != PoseSpace.Num())
            {
                return false;
            }

            OutMesh->EditMesh(
                [&LOD, &ReferenceSpace, &PoseSpace](UE::Geometry::FDynamicMesh3& EditMesh)
                {
                    for (const FSkelMeshSection& Section : LOD.Sections)
                    {
                        TArray<int32> VertexMap;
                        VertexMap.SetNum(Section.SoftVertices.Num());
                        for (int32 VertexIndex = 0; VertexIndex < Section.SoftVertices.Num();
                             ++VertexIndex)
                        {
                            const FSoftSkinVertex& Source = Section.SoftVertices[VertexIndex];
                            FVector Skinned = FVector::ZeroVector;
                            double WeightSum = 0.0;
                            const FVector SourcePosition(Source.Position);
                            for (int32 Influence = 0; Influence < MAX_TOTAL_INFLUENCES;
                                 ++Influence)
                            {
                                const double Weight =
                                    static_cast<double>(Source.InfluenceWeights[Influence])
                                    / 65535.0;
                                if (Weight <= 0.0
                                    || !Section.BoneMap.IsValidIndex(
                                        Source.InfluenceBones[Influence]))
                                {
                                    continue;
                                }
                                const int32 BoneIndex =
                                    Section.BoneMap[Source.InfluenceBones[Influence]];
                                if (!ReferenceSpace.IsValidIndex(BoneIndex)
                                    || !PoseSpace.IsValidIndex(BoneIndex))
                                {
                                    continue;
                                }
                                // Convert reference component-space vertices into the posed
                                // component space. The order matters for a rotated child bone
                                // whose reference transform carries a translation.
                                const FTransform ReferenceToLocal =
                                    ReferenceSpace[BoneIndex].Inverse() * PoseSpace[BoneIndex];
                                Skinned += ReferenceToLocal.TransformPosition(SourcePosition)
                                    * Weight;
                                WeightSum += Weight;
                            }
                            if (WeightSum <= SMALL_NUMBER)
                            {
                                Skinned = SourcePosition;
                            }
                            else if (!FMath::IsNearlyEqual(WeightSum, 1.0))
                            {
                                Skinned /= WeightSum;
                            }
                            VertexMap[VertexIndex] = EditMesh.AppendVertex(FVector3d(Skinned));
                        }

                        for (uint32 Triangle = 0; Triangle < Section.NumTriangles; ++Triangle)
                        {
                            const uint32 Base = Section.BaseIndex + Triangle * 3;
                            if (!LOD.IndexBuffer.IsValidIndex(Base + 2))
                            {
                                continue;
                            }
                            const uint32 I0 = LOD.IndexBuffer[Base] - Section.BaseVertexIndex;
                            const uint32 I1 = LOD.IndexBuffer[Base + 1] - Section.BaseVertexIndex;
                            const uint32 I2 = LOD.IndexBuffer[Base + 2] - Section.BaseVertexIndex;
                            if (VertexMap.IsValidIndex(I0) && VertexMap.IsValidIndex(I1)
                                && VertexMap.IsValidIndex(I2))
                            {
                                EditMesh.AppendTriangle(UE::Geometry::FIndex3i(
                                    VertexMap[I0], VertexMap[I1], VertexMap[I2]));
                            }
                        }
                    }
                });
            return OutMesh->GetTriangleCount() > 0;
        }

        void MeshAuditMakeComponentSpacePose(const FReferenceSkeleton& RefSkeleton,
                                              const TArray<FTransform>& Local,
                                              TArray<FTransform>& OutComponent)
        {
            OutComponent.Reset();
            FAnimationRuntime::FillUpComponentSpaceTransforms(
                RefSkeleton, Local, OutComponent);
        }

        int32 MeshAuditResolveBindComponent(
            const TArray<FComponentMeasurement>& BindComponents,
            const TMap<int32, int32>& TriangleToBindComponent,
            const FComponentMeasurement& FrameComponent)
        {
            TMap<int32, int32> Counts;
            for (const int32 TriangleID : FrameComponent.TriangleIDs)
            {
                if (const int32* BindComponent = TriangleToBindComponent.Find(TriangleID))
                {
                    ++Counts.FindOrAdd(*BindComponent);
                }
            }

            int32 BestComponent = INDEX_NONE;
            int32 BestCount = 0;
            for (const TPair<int32, int32>& Entry : Counts)
            {
                if (BestComponent == INDEX_NONE || Entry.Value > BestCount
                    || (Entry.Value == BestCount && Entry.Key < BestComponent))
                {
                    BestComponent = Entry.Key;
                    BestCount = Entry.Value;
                }
            }
            return BindComponents.IsValidIndex(BestComponent) ? BestComponent : INDEX_NONE;
        }
    }
#endif

    void MeasureSkeletalAnimationFloating(USkeletalMesh* SkeletalMesh, UAnimSequence* Sequence,
                                          int32 SampleStride, double ToleranceFraction,
                                          FAnimationFloatingReport& OutReport)
    {
        OutReport = FAnimationFloatingReport();
        OutReport.SampleStride = FMath::Max(1, SampleStride);
        OutReport.ToleranceFraction = FMath::Max(0.0, ToleranceFraction);

#if !WITH_EDITOR
        OutReport.UnrunnableReason = TEXT("Skeletal animation source geometry requires an editor build.");
        return;
#else
        if (!IsValid(SkeletalMesh) || !IsValid(Sequence))
        {
            OutReport.UnrunnableReason = TEXT("A skeletal mesh and an animation sequence are required.");
            return;
        }
        USkeleton* MeshSkeleton = SkeletalMesh->GetSkeleton();
        USkeleton* SequenceSkeleton = Sequence->GetSkeleton();
        if (!MeshSkeleton || !SequenceSkeleton)
        {
            OutReport.UnrunnableReason = TEXT(
                "Both the skeletal mesh and animation sequence must reference a skeleton.");
            return;
        }
        if (MeshSkeleton != SequenceSkeleton)
        {
            OutReport.UnrunnableReason = TEXT("The animation and skeletal mesh use different skeleton assets.");
            return;
        }
        const IAnimationDataModel* Model = Sequence->GetDataModel();
        const FSkeletalMeshModel* Imported = SkeletalMesh->GetImportedModel();
        if (!Model || !Imported || Imported->LODModels.Num() == 0)
        {
            OutReport.UnrunnableReason = TEXT("The sequence has no editor data model or the mesh has no imported LOD0.");
            return;
        }

        // NumberOfFrames is the timeline span; NumberOfKeys is the count of stored samples and
        // is not a safe frame loop bound. A two-frame timeline has frame positions 0, 1 and 2.
        const int32 FrameCount = Model->GetNumberOfFrames();
        const FFrameRate FrameRate = Model->GetFrameRate();
        const FReferenceSkeleton& RefSkeleton = SkeletalMesh->GetRefSkeleton();
        const TArray<FTransform>& RefLocal = RefSkeleton.GetRefBonePose();
        if (FrameCount < 1 || RefSkeleton.GetNum() == 0
            || RefLocal.Num() != RefSkeleton.GetNum()
            || FrameRate.Numerator <= 0 || FrameRate.Denominator <= 0)
        {
            OutReport.UnrunnableReason = TEXT(
                "At least one animation frame, a valid frame rate and a complete mesh reference "
                "pose are required.");
            return;
        }
        OutReport.FrameCount = FrameCount;

        TArray<FFrameNumber> SampleFrames;
        for (int32 Frame = 0; Frame <= FrameCount; Frame += OutReport.SampleStride)
        {
            SampleFrames.Add(FFrameNumber(Frame));
        }
        if (SampleFrames.Num() == 0 || SampleFrames.Last().Value != FrameCount)
        {
            SampleFrames.Add(FFrameNumber(FrameCount));
        }
        OutReport.SampledFrameCount = SampleFrames.Num();
        OutReport.SampledFrameIds.Reserve(SampleFrames.Num());
        for (const FFrameNumber Frame : SampleFrames)
        {
            OutReport.SampledFrameIds.Add(Frame.Value);
        }

        TArray<FBoneIndexType> RequiredBoneIndices;
        RequiredBoneIndices.SetNumUninitialized(RefSkeleton.GetNum());
        for (int32 BoneIndex = 0; BoneIndex < RequiredBoneIndices.Num(); ++BoneIndex)
        {
            RequiredBoneIndices[BoneIndex] = static_cast<FBoneIndexType>(BoneIndex);
        }
        FBoneContainer RequiredBones;
        RequiredBones.InitializeTo(
            RequiredBoneIndices, UE::Anim::FCurveFilterSettings(UE::Anim::ECurveFilterMode::DisallowAll),
            *SkeletalMesh);
        RequiredBones.SetUseSourceData(true);
        if (!RequiredBones.IsValid())
        {
            OutReport.UnrunnableReason = TEXT("The mesh skeleton could not initialize an animation pose evaluator.");
            return;
        }

        TArray<TArray<FTransform>> LocalBySample;
        LocalBySample.SetNum(SampleFrames.Num());
        for (int32 Sample = 0; Sample < SampleFrames.Num(); ++Sample)
        {
            FString PoseFailureReason;
            if (!MeshAuditEvaluateSequencePose(
                    *Sequence, FrameRate, RequiredBones, RefSkeleton, RefLocal,
                    SampleFrames[Sample], LocalBySample[Sample], PoseFailureReason))
            {
                OutReport.UnrunnableReason = PoseFailureReason.IsEmpty()
                    ? FString::Printf(
                        TEXT("Could not evaluate animation pose at frame %d."),
                        SampleFrames[Sample].Value)
                    : MoveTemp(PoseFailureReason);
                return;
            }
        }

        TArray<FTransform> ReferenceSpace;
        MeshAuditMakeComponentSpacePose(RefSkeleton, RefLocal, ReferenceSpace);
        const FSkeletalMeshLODModel& LOD = Imported->LODModels[0];
        UDynamicMesh* BindMesh = nullptr;
        if (!MeshAuditBuildPosedSkeletalLOD(LOD, ReferenceSpace, ReferenceSpace, BindMesh))
        {
            OutReport.UnrunnableReason = TEXT("The skeletal mesh LOD did not produce measurable triangles.");
            return;
        }

        TArray<FComponentMeasurement> BindComponents;
        MeasureComponents(BindMesh, BindComponents);
        FSpatialMeasurement BindSpatial;
        MeasureSpatialProximity(BindMesh, BindSpatial, BindComponents, OutReport.ToleranceFraction);
        FFloatingReport BindFloating;
        ClassifyFloatingComponents(BindComponents, BindSpatial, OutReport.ToleranceFraction,
                                    BindFloating);
        OutReport.BindIslandCount = BindFloating.IslandCount;
        OutReport.BindNonMainIslandCount = BindFloating.NonMainIslandCount;
        OutReport.BindFloatingCount = BindFloating.FloatingCount;
        OutReport.BoundingSphereRadius = BindFloating.BoundingSphereRadius;
        OutReport.Tolerance = BindFloating.Tolerance;

        TMap<int32, int32> TriangleToBindComponent;
        for (const FComponentMeasurement& Component : BindComponents)
        {
            for (const int32 TriangleID : Component.TriangleIDs)
            {
                TriangleToBindComponent.Add(TriangleID, Component.Index);
            }
        }

        TArray<int32> RowByComponent;
        RowByComponent.Init(-1, BindComponents.Num());
        for (const FFloatingComponent& Component : BindFloating.Components)
        {
            if (!RowByComponent.IsValidIndex(Component.ComponentIndex))
            {
                continue;
            }
            FAnimationFloatingComponent& Row = OutReport.Components.AddDefaulted_GetRef();
            Row.ComponentIndex = Component.ComponentIndex;
            Row.ProximityIslandId = BindFloating.ComponentIslandIds.IsValidIndex(
                Component.ComponentIndex)
                ? BindFloating.ComponentIslandIds[Component.ComponentIndex]
                : INDEX_NONE;
            Row.TriangleCount = Component.TriangleCount;
            Row.SignedVolume = Component.SignedVolume;
            Row.Center = Component.Center;
            Row.NearestComponentIndex = Component.NearestComponentIndex;
            Row.FirstSeparation = Component.NearestDistance;
            // Bind pose is the model's frame-zero reference. Give the table a concrete frame
            // for the measured separation while the explicit flag keeps it out of the
            // animation-caused count below.
            Row.FirstSeparatedFrame = 0;
            Row.WorstFrame = 0;
            Row.WorstSeparation = Component.NearestDistance;
            Row.bAlreadySeparatedAtBindPose = true;
            RowByComponent[Component.ComponentIndex] = OutReport.Components.Num() - 1;
        }

        for (int32 Sample = 0; Sample < LocalBySample.Num(); ++Sample)
        {
            TArray<FTransform> PoseSpace;
            MeshAuditMakeComponentSpacePose(RefSkeleton, LocalBySample[Sample], PoseSpace);
            UDynamicMesh* FrameMesh = nullptr;
            if (!MeshAuditBuildPosedSkeletalLOD(LOD, ReferenceSpace, PoseSpace, FrameMesh))
            {
                OutReport.UnrunnableReason = FString::Printf(
                    TEXT("Could not skin sampled frame %d."), SampleFrames[Sample].Value);
                return;
            }
            TArray<FComponentMeasurement> FrameComponents;
            MeasureComponents(FrameMesh, FrameComponents);
            FSpatialMeasurement FrameSpatial;
            // Keep both pruning and classification on the absolute bind-pose tolerance.
            MeasureSpatialProximity(FrameMesh, FrameSpatial, FrameComponents,
                                    OutReport.ToleranceFraction, OutReport.Tolerance);
            FFloatingReport FrameFloating;
            ClassifyFloatingComponents(FrameComponents, FrameSpatial,
                                        OutReport.ToleranceFraction, FrameFloating,
                                        OutReport.Tolerance);
            for (const FFloatingComponent& Component : FrameFloating.Components)
            {
                if (!FrameComponents.IsValidIndex(Component.ComponentIndex))
                {
                    continue;
                }
                const int32 BindComponentIndex = MeshAuditResolveBindComponent(
                    BindComponents, TriangleToBindComponent,
                    FrameComponents[Component.ComponentIndex]);
                if (!RowByComponent.IsValidIndex(BindComponentIndex))
                {
                    continue;
                }
                int32 FrameIslandId = INDEX_NONE;
                if (FrameFloating.ComponentIslandIds.IsValidIndex(Component.ComponentIndex))
                {
                    const int32 FrameIslandRepresentative =
                        FrameFloating.ComponentIslandIds[Component.ComponentIndex];
                    for (int32 CandidateIndex = 0;
                         CandidateIndex < FrameComponents.Num(); ++CandidateIndex)
                    {
                        if (FrameFloating.ComponentIslandIds[CandidateIndex]
                            != FrameIslandRepresentative)
                        {
                            continue;
                        }
                        const int32 CandidateBindComponent = MeshAuditResolveBindComponent(
                            BindComponents, TriangleToBindComponent,
                            FrameComponents[CandidateIndex]);
                        if (CandidateBindComponent >= 0)
                        {
                            FrameIslandId = FrameIslandId < 0
                                ? CandidateBindComponent
                                : FMath::Min(FrameIslandId, CandidateBindComponent);
                        }
                    }
                }
                if (FrameIslandId < 0)
                {
                    FrameIslandId = BindComponentIndex;
                }
                const int32 ExistingRowIndex = RowByComponent[BindComponentIndex];
                FAnimationFloatingComponent* Row = nullptr;
                if (ExistingRowIndex < 0)
                {
                    FAnimationFloatingComponent& NewRow = OutReport.Components.AddDefaulted_GetRef();
                    NewRow.ComponentIndex = BindComponentIndex;
                    NewRow.ProximityIslandId = FrameIslandId;
                    NewRow.TriangleCount = BindComponents[BindComponentIndex].TriangleCount;
                    RowByComponent[BindComponentIndex] = OutReport.Components.Num() - 1;
                }
                Row = &OutReport.Components[RowByComponent[BindComponentIndex]];
                if (Row->FirstSeparatedFrame < 0)
                {
                    Row->ProximityIslandId = FrameIslandId;
                    Row->FirstSeparatedFrame = SampleFrames[Sample].Value;
                    Row->FirstSeparation = Component.NearestDistance;
                }
                if (Row->WorstFrame < 0 || Component.NearestDistance > Row->WorstSeparation)
                {
                    int32 NearestBindComponent = INDEX_NONE;
                    if (FrameComponents.IsValidIndex(Component.NearestComponentIndex))
                    {
                        NearestBindComponent = MeshAuditResolveBindComponent(
                            BindComponents, TriangleToBindComponent,
                            FrameComponents[Component.NearestComponentIndex]);
                    }
                    Row->SignedVolume = Component.SignedVolume;
                    Row->Center = Component.Center;
                    Row->NearestComponentIndex = NearestBindComponent;
                    Row->WorstSeparation = Component.NearestDistance;
                    Row->WorstFrame = SampleFrames[Sample].Value;
                }
            }
        }

        TSet<int32> AnimationSeparatedIslandIds;
        for (const FAnimationFloatingComponent& Row : OutReport.Components)
        {
            if (!Row.bAlreadySeparatedAtBindPose && Row.FirstSeparatedFrame >= 0
                && Row.ProximityIslandId >= 0)
            {
                AnimationSeparatedIslandIds.Add(Row.ProximityIslandId);
            }
        }
        OutReport.FloatingComponentRows = OutReport.Components.Num();
        OutReport.UniqueAnimationSeparatedIslandCount = AnimationSeparatedIslandIds.Num();

        OutReport.bMeasured = true;
        return;
#endif
    }

    // ---- Verdict -------------------------------------------------------------------------------
    //
    // FReport::DerivePass now forwards to PinWrightAudit::FVerdict::DerivePass in the header.
    // The rule and the reasons its last two terms ignore failOn are stated there, once, for
    // every audit rather than in a copy per verb.

    // ---- Reading one asset ----------------------------------------------------------------------

    namespace
    {
        EGeometryScriptLODType ToEngineLodType(ELodSource Source)
        {
            switch (Source)
            {
            case ELodSource::HiResSourceModel: return EGeometryScriptLODType::HiResSourceModel;
            case ELodSource::SourceModel:      return EGeometryScriptLODType::SourceModel;
            case ELodSource::RenderData:       return EGeometryScriptLODType::RenderData;
            case ELodSource::MaxAvailable:
            default:                           return EGeometryScriptLODType::MaxAvailable;
            }
        }
    }

    void MeasureStaticMesh(UStaticMesh* Mesh, const FConfig& Config, UDynamicMesh* Scratch,
                           FAssetMeasurement& Out)
    {
        Out = FAssetMeasurement();

        if (!IsValid(Mesh))
        {
            Out.UnrunnableCode = ErrorCodes::ERR_MESH_AUDIT_UNLOADABLE;
            Out.UnrunnableReason = TEXT("The matched asset did not resolve to a UStaticMesh.");
            return;
        }
        if (!IsValid(Scratch))
        {
            Out.UnrunnableCode = ErrorCodes::ERR_MESH_AUDIT_READ_FAILED;
            Out.UnrunnableReason = TEXT("No destination dynamic mesh was available to read into.");
            return;
        }

        // Asset-level facts, read off the UStaticMesh rather than off the copied triangles.
        // BuildScale is the one that matters: an odd number of negative axes mirrors the mesh
        // at build time, so an asset whose authored triangles are correctly wound still ships
        // inverted. Reading it here rather than inferring it from the volume sign is what lets
        // the sweep report the CAUSE next to the effect.
#if WITH_EDITORONLY_DATA
        Out.SourceLodCount = Mesh->GetNumSourceModels();
        Out.bHasSourceModel = Out.SourceLodCount > 0;
        if (Out.bHasSourceModel)
        {
            Out.BuildScale = Mesh->GetSourceModel(0).BuildSettings.BuildScale3D;
        }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        Out.bNaniteEnabled = Mesh->GetNaniteSettings().bEnabled;
#else
        Out.bNaniteEnabled = Mesh->NaniteSettings.bEnabled;
#endif
#endif

        // Guarded before the copy, not after: CopyMeshFromStaticMesh builds the whole
        // FDynamicMesh3 up front, so a Nanite hero asset is a multi-hundred-MB spike. A sweep
        // that OOMs mid-page takes the editor with it and reports nothing at all, so the
        // budget check produces an Unrunnable row instead - the sweep says it stopped looking.
        if (!GeometryUtils::IsMemoryPressureSafe())
        {
            Out.UnrunnableCode = ErrorCodes::ERR_MEMORY_PRESSURE;
            Out.UnrunnableReason = FString::Printf(
                TEXT("Insufficient memory headroom (%.0f%% used) to read this mesh. Re-run with a ")
                TEXT("smaller `limit`, or page past it."), GeometryUtils::GetMemoryUsagePercent());
            return;
        }

        FGeometryScriptCopyMeshFromAssetOptions CopyOptions;
        CopyOptions.bApplyBuildSettings = Config.Read.bApplyBuildSettings;
        // See the handler sites: bUseBuildScale arrived in UE 5.4 and 5.3 behaves as if it were
        // always true. The audit refuses the un-servable request rather than reporting checks run
        // over a mesh read with the opposite scale.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        CopyOptions.bUseBuildScale = Config.Read.bUseBuildScale;
#else
        if (!Config.Read.bUseBuildScale)
        {
            Out.UnrunnableCode = ErrorCodes::ERR_UNSUPPORTED_ENGINE_VERSION;
            Out.UnrunnableReason = TEXT(
                "useBuildScale=false needs FGeometryScriptCopyMeshFromAssetOptions::bUseBuildScale, "
                "added in UE 5.4. This engine always applies the LOD's build scale.");
            return;
        }
#endif
        // Tangents are not read by any check here and cost time and memory per asset.
        CopyOptions.bRequestTangents = false;

        FGeometryScriptMeshReadLOD ReadLOD;
        ReadLOD.LODType = ToEngineLodType(Config.Read.LodSource);
        ReadLOD.LODIndex = Config.Read.LodIndex;

        EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;

        // The 6-arg overload on purpose, for the same reason MeshAssetIOHandler.cpp:335 uses
        // it: on 5.5+ it is a non-deprecated inline forwarder to CopyMeshFromStaticMeshV2, and
        // on 5.3/5.4 it IS the exported function, so it needs no UE_VERSION_* guard. The copy
        // REPLACES the destination's contents (ToDynamicMesh->SetMesh(MoveTemp(NewMesh))),
        // which is what makes reusing one scratch mesh across the whole sweep correct by
        // construction rather than by an extra clear step per asset.
        UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMesh(
            Mesh, Scratch, CopyOptions, ReadLOD, Outcome, nullptr);

        if (Outcome != EGeometryScriptOutcomePins::Success)
        {
            Out.UnrunnableCode = ErrorCodes::ERR_MESH_AUDIT_READ_FAILED;
            Out.UnrunnableReason = FString::Printf(
                TEXT("The asset loaded but its %s LOD %d would not copy into a measurable mesh."),
                LodSourceToString(Config.Read.LodSource), Config.Read.LodIndex);
            return;
        }

        // Two walks rather than one. MeasureMeshHealth delegates its orientation half to
        // MeasureMeshOrientation, so the two cannot answer differently about the same mesh -
        // but only the orientation form carries SurfaceArea, which `thin_shell` needs as the
        // scale the volume is read against. The second walk is O(E+T) and allocation-free,
        // which is nothing beside the package load that preceded it.
        Out.Health = GeometryUtils::MeasureMeshHealth(Scratch);
        Out.SurfaceArea = GeometryUtils::MeasureMeshOrientation(Scratch).SurfaceArea;
        MeasureComponents(Scratch, Out.Components, Out.ZFightTriangles);
        if (HasCheck(Config.SelectedChecks, ECheck::FloatingComponents))
        {
            MeasureSpatialProximity(Scratch, Out.Spatial, Out.Components,
                                    Config.Thresholds.FloatingToleranceFraction);
        }
        // Keep the reported component count tied to the same edge-connected walk that supplied
        // the per-component volumes. This avoids a whole-mesh helper and the audit disagreeing
        // about what a component is on a mesh with unwelded shells.
        Out.Health.ComponentCount = Out.Components.Num();
        Out.bMeasured = true;
    }

    // ---- Evaluating one measurement ---------------------------------------------------------------

    namespace
    {
        // Every finding carries the numbers the verdict was derived from. A lint whose inputs
        // and thresholds are not in its own output cannot be argued with.
        TSharedPtr<FJsonObject> BaseMeasurements(const FAssetMeasurement& M)
        {
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetNumberField(TEXT("triangleCount"), M.Health.TriangleCount);
            Obj->SetNumberField(TEXT("vertexCount"), M.Health.VertexCount);
            Obj->SetNumberField(TEXT("boundaryEdges"), M.Health.BoundaryEdges);
            Obj->SetNumberField(TEXT("inconsistentEdges"), M.Health.InconsistentEdges);
            Obj->SetNumberField(TEXT("signedVolume"), M.Health.SignedVolume);
            Obj->SetNumberField(TEXT("surfaceArea"), M.SurfaceArea);
            Obj->SetNumberField(TEXT("componentCount"), M.Components.Num());
            Obj->SetBoolField(TEXT("isClosed"), M.Health.IsClosed());
            return Obj;
        }

        struct FInvertedSummary
        {
            int32 Answered = 0;
            int32 Unknown = 0;
            int32 Inverted = 0;
            int32 Clean = 0;
            bool bNoComponentMeasurement = false;
        };

        void AddComponentMeasurements(const FAssetMeasurement& M, double MinVolumeRatio,
                                      TSharedPtr<FJsonObject>& Measurements,
                                      FInvertedSummary& OutSummary)
        {
            const double Threshold = FMath::Max(0.0, MinVolumeRatio);
            TArray<TSharedPtr<FJsonValue>> ComponentRows;
            ComponentRows.Reserve(M.Components.Num());

            for (const FComponentMeasurement& Component : M.Components)
            {
                const double Ratio = Component.VolumeRatio();
                const bool bFinite = FMath::IsFinite(Component.SignedVolume)
                    && FMath::IsFinite(Component.SurfaceArea) && FMath::IsFinite(Ratio);
                const bool bNearZero = !bFinite || Component.SurfaceArea <= 0.0
                    || Ratio <= Threshold;

                TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
                Row->SetNumberField(TEXT("index"), Component.Index);
                Row->SetNumberField(TEXT("firstTriangleId"), Component.FirstTriangleID);
                Row->SetNumberField(TEXT("triangleCount"), Component.TriangleCount);
                Row->SetNumberField(TEXT("boundaryEdges"), Component.BoundaryEdges);
                Row->SetNumberField(TEXT("degenerateTriangles"), Component.DegenerateTriangles);
                Row->SetNumberField(TEXT("signedVolume"), Component.SignedVolume);
                Row->SetNumberField(TEXT("surfaceArea"), Component.SurfaceArea);
                Row->SetNumberField(TEXT("volumeRatio"), Ratio);
                Row->SetNumberField(
                    TEXT("volumeTolerance"),
                    bFinite && Component.SurfaceArea > 0.0
                        ? Threshold * FMath::Pow(Component.SurfaceArea, 1.5)
                        : 0.0);

                if (!Component.IsClosed())
                {
                    ++OutSummary.Unknown;
                    Row->SetStringField(TEXT("status"), TEXT("unknown"));
                    Row->SetStringField(TEXT("unknownReason"), TEXT("open"));
                }
                else if (Component.DegenerateTriangles > 0)
                {
                    ++OutSummary.Unknown;
                    Row->SetStringField(TEXT("status"), TEXT("unknown"));
                    Row->SetStringField(TEXT("unknownReason"), TEXT("degenerate"));
                }
                else if (bNearZero)
                {
                    ++OutSummary.Unknown;
                    Row->SetStringField(TEXT("status"), TEXT("unknown"));
                    Row->SetStringField(TEXT("unknownReason"), TEXT("near_zero_volume"));
                }
                else if (Component.SignedVolume < 0.0)
                {
                    ++OutSummary.Answered;
                    ++OutSummary.Inverted;
                    Row->SetStringField(TEXT("status"), TEXT("inverted"));
                }
                else
                {
                    ++OutSummary.Answered;
                    ++OutSummary.Clean;
                    Row->SetStringField(TEXT("status"), TEXT("clean"));
                }
                ComponentRows.Add(MakeShared<FJsonValueObject>(Row));
            }

            // An empty asset, or a measured non-empty asset with no component walk, is UNKNOWN
            // rather than clean. There is no real component to put in unknownComponents, so
            // keep the component buckets reconciled and carry the asset-level reason separately.
            if (M.Health.TriangleCount <= 0 || M.Components.Num() == 0)
            {
                OutSummary.bNoComponentMeasurement = true;
                Measurements->SetBoolField(TEXT("componentMeasurementAvailable"), false);
                Measurements->SetStringField(
                    TEXT("componentUnknownReason"),
                    M.Health.TriangleCount <= 0 ? TEXT("empty_mesh") : TEXT("no_components"));
            }
            else
            {
                Measurements->SetBoolField(TEXT("componentMeasurementAvailable"), true);
            }

            Measurements->SetNumberField(TEXT("answeredComponents"), OutSummary.Answered);
            Measurements->SetNumberField(TEXT("unknownComponents"), OutSummary.Unknown);
            Measurements->SetNumberField(TEXT("invertedComponents"), OutSummary.Inverted);
            Measurements->SetNumberField(TEXT("cleanComponents"), OutSummary.Clean);
            Measurements->SetNumberField(TEXT("minVolumeRatio"), Threshold);
            Measurements->SetArrayField(TEXT("components"), ComponentRows);
        }

        void PushFinding(FReport& R, int32 MaxRows, const FString& AssetPath,
                         const FString& AssetName, const FCheckInfo& Info, EFindingStatus Status,
                         const FString& Code, const FString& Message,
                         const TSharedPtr<FJsonObject>& Measurements)
        {
            // Counts are tallied BEFORE the row cap, so the severity totals and the per-check
            // histogram stay exact on a page whose findings[] was clipped. Only rows are lost,
            // and findingsTruncated says so - a clipped page can never report pass.
            if (Status == EFindingStatus::Unrunnable) { ++R.UnrunnableCount; }
            else if (Info.Severity == ESeverity::Error) { ++R.ErrorCount; }
            else { ++R.WarningCount; }

            if (R.Findings.Num() >= MaxRows)
            {
                R.bFindingsTruncated = true;
                ++R.FindingsDropped;
                return;
            }

            FFinding Finding;
            Finding.AssetPath = AssetPath;
            Finding.AssetName = AssetName;
            Finding.Check = Info.Check;
            Finding.Status = Status;
            Finding.Severity = Info.Severity;
            Finding.Code = Code;
            Finding.Message = Message;
            Finding.Measurements = Measurements;
            R.Findings.Add(MoveTemp(Finding));
        }
    }

    void EvaluateAsset(const FString& AssetPath, const FString& AssetName,
                       const FAssetMeasurement& M, const FConfig& Config, FReport& R)
    {
        ++R.AssetsExamined;
        const int32 ErrorsBefore = R.ErrorCount;
        const int32 WarningsBefore = R.WarningCount;
        const int32 UnrunnableBefore = R.UnrunnableCount;

        // ---- the asset could not be read at all ----
        // Every SELECTED check reports Unrunnable rather than the asset being skipped. A
        // skipped asset is invisible; an Unrunnable one is an unanswered question with a
        // reason attached, and it forces pass:false. This single rule is what separates this
        // sweep from the class of tool that exits 0 having examined nothing.
        if (!M.bMeasured)
        {
            ++R.AssetsUnmeasured;
            for (const FCheckInfo& Info : AllChecks())
            {
                if (!HasCheck(Config.SelectedChecks, Info.Check)) { continue; }
                FCheckTally& Tally = R.Tallies[static_cast<int32>(Info.Check)];
                ++Tally.Applicable;
                ++Tally.Unrunnable;
                PushFinding(R, Config.MaxFindings, AssetPath, AssetName, Info,
                            EFindingStatus::Unrunnable, M.UnrunnableCode, M.UnrunnableReason,
                            nullptr);
            }
            ++R.AssetsFlagged;
            return;
        }

        R.TotalTriangles += M.Health.TriangleCount;
        R.TotalVertices += M.Health.VertexCount;

        const bool bClosed = M.Health.IsClosed();
        const bool bEmpty = M.Health.TriangleCount <= 0;

        for (const FCheckInfo& Info : AllChecks())
        {
            if (!HasCheck(Config.SelectedChecks, Info.Check)) { continue; }
            FCheckTally& Tally = R.Tallies[static_cast<int32>(Info.Check)];

            // Not-applicable is a real answer here, not a polite form of clean. A closed-only
            // check on an OPEN mesh has nothing to say: FMeshOrientation::IsInverted() is
            // deliberately false on an open mesh, because an open surface is not inside out,
            // it is open - and folding that into "clean" would put a second wrong answer next
            // to the first. `inverted` is the deliberate exception: it remains applicable so
            // its component walk can report OPEN as UNKNOWN/unrunnable. An EMPTY mesh is also
            // applicable to `inverted` for the same reason; the component walk reports UNKNOWN.
            const bool bApplicable =
                (Info.Check == ECheck::EmptyMesh) ? true
                : bEmpty ? (Info.Check == ECheck::Inverted
                            || Info.Check == ECheck::ZFighting)
                : (Info.Check == ECheck::MirroredBuildScale) ? M.bHasSourceModel
                : (Info.Check == ECheck::Inverted) ? true
                : (!Info.bNeedsClosed || bClosed);

            if (!bApplicable)
            {
                ++Tally.NotApplicable;
                continue;
            }
            ++Tally.Applicable;

            bool bFlagged = false;
            FString Message;
            TSharedPtr<FJsonObject> Measurements = BaseMeasurements(M);

            switch (Info.Check)
            {
            case ECheck::Inverted:
            {
                FInvertedSummary Summary;
                AddComponentMeasurements(M, Config.Thresholds.MinVolumeRatio, Measurements, Summary);
                if (Summary.Unknown > 0 || Summary.bNoComponentMeasurement)
                {
                    // This check was selected but at least one component, or the component walk
                    // itself, cannot answer the question. Use the shared unrunnable status so
                    // FVerdict fails even for failOn:"none"; a not-applicable or clean row would
                    // be false.
                    ++Tally.Unrunnable;
                    FString UnknownReason;
                    if (Summary.bNoComponentMeasurement)
                    {
                        UnknownReason = TEXT(
                            "No component measurement was available (the mesh was empty or the "
                            "component walk returned no components)");
                    }
                    else
                    {
                        UnknownReason = FString::Printf(
                            TEXT("%d connected component(s) were UNKNOWN because they are open, "
                                 "degenerate, near-zero-volume or non-finite"),
                            Summary.Unknown);
                    }
                    const FString UnknownMessage = FString::Printf(
                        TEXT("%s; %d component(s) were answered (%d inverted, %d clean). The whole-"
                             "mesh signed volume is %+.6g and must not replace these component "
                             "answers."),
                        *UnknownReason, Summary.Answered, Summary.Inverted, Summary.Clean,
                        M.Health.SignedVolume);
                    PushFinding(R, Config.MaxFindings, AssetPath, AssetName, Info,
                                EFindingStatus::Unrunnable,
                                ErrorCodes::ERR_MESH_AUDIT_COMPONENT_UNKNOWN,
                                UnknownMessage, Measurements);
                    continue;
                }

                bFlagged = Summary.Inverted > 0;
                Message = FString::Printf(
                    TEXT("%d of %d answered connected component(s) are inverted. Their per-"
                         "component signed volumes are in measurements; the whole-mesh sum is "
                         "%+.6g and is retained only as context."),
                    Summary.Inverted, Summary.Answered, M.Health.SignedVolume);
                break;
            }

            case ECheck::InconsistentWinding:
                bFlagged = !M.Health.IsOrientationConsistent();
                Message = FString::Printf(
                    TEXT("%d interior edge(s) whose two triangles traverse them the SAME way: ")
                    TEXT("part of the mesh is wound against the rest. A uniform inversion does ")
                    TEXT("not produce this, so it is an independent second fault."),
                    M.Health.InconsistentEdges);
                break;

            case ECheck::NotClosed:
                bFlagged = !bClosed;
                Message = FString::Printf(
                    TEXT("%d boundary edge(s) across %d connected component(s): the mesh is open. ")
                    TEXT("Correct for a card, a plane or an authored shell; otherwise a cut broke ")
                    TEXT("through a wall. While it is open, `inverted` cannot be answered at all."),
                    M.Health.BoundaryEdges, M.Health.ComponentCount);
                break;

            case ECheck::DegenerateTriangles:
                bFlagged = M.Health.DegenerateTriangles > 0;
                Measurements->SetNumberField(TEXT("degenerateTriangles"), M.Health.DegenerateTriangles);
                Measurements->SetNumberField(TEXT("degenerateAreaEpsilon"),
                                             GeometryUtils::DegenerateAreaEpsilon);
                Message = FString::Printf(
                    TEXT("%d triangle(s) with area below %g. Most die in the static-mesh build's ")
                    TEXT("own bRemoveDegenerates pass, so this is a trend rather than a defect: a ")
                    TEXT("jump means two operands were placed to touch exactly, not to overlap."),
                    M.Health.DegenerateTriangles, GeometryUtils::DegenerateAreaEpsilon);
                break;

            case ECheck::NonManifoldVertices:
                bFlagged = M.Health.NonManifoldVertices > 0;
                Measurements->SetNumberField(TEXT("nonManifoldVertices"), M.Health.NonManifoldVertices);
                Message = FString::Printf(
                    TEXT("%d bowtie vertex/vertices: two or more triangle fans meet at one point."),
                    M.Health.NonManifoldVertices);
                break;

            case ECheck::EmptyMesh:
                bFlagged = bEmpty;
                Message = TEXT("The asset read back with zero triangles. Every other check here ")
                          TEXT("would call it clean, which is exactly why it needs its own.");
                break;

            case ECheck::MirroredBuildScale:
                bFlagged = M.IsMirroredByBuildScale();
                Measurements->SetNumberField(TEXT("buildScaleX"), M.BuildScale.X);
                Measurements->SetNumberField(TEXT("buildScaleY"), M.BuildScale.Y);
                Measurements->SetNumberField(TEXT("buildScaleZ"), M.BuildScale.Z);
                Measurements->SetNumberField(TEXT("negativeBuildScaleAxes"),
                                             M.NegativeBuildScaleAxes());
                Message = FString::Printf(
                    TEXT("Build Scale (%g, %g, %g) carries %d negative axis/axes - an odd count, ")
                    TEXT("so the build mirrors the mesh and inverts its winding. This is the CAUSE ")
                    TEXT("an `inverted` finding on this asset would otherwise only show as effect."),
                    M.BuildScale.X, M.BuildScale.Y, M.BuildScale.Z, M.NegativeBuildScaleAxes());
                break;

            case ECheck::ThinShell:
            {
                const double Ratio = M.VolumeRatio();
                bFlagged = Ratio < Config.Thresholds.MinVolumeRatio;
                Measurements->SetNumberField(TEXT("volumeRatio"), Ratio);
                Measurements->SetNumberField(TEXT("minVolumeRatio"), Config.Thresholds.MinVolumeRatio);
                Message = FString::Printf(
                    TEXT("Closed mesh enclosing almost nothing: |volume| / area^1.5 is %.3g, below ")
                    TEXT("%.3g. A cube reads 0.068 and a sphere 0.094, so this is a sheet folded ")
                    TEXT("back on itself rather than a solid."),
                    Ratio, Config.Thresholds.MinVolumeRatio);
                break;
            }

            case ECheck::FloatingComponents:
            {
                FFloatingReport Floating;
                ClassifyFloatingComponents(M.Components, M.Spatial,
                                            Config.Thresholds.FloatingToleranceFraction, Floating);
                Measurements->SetNumberField(TEXT("islandCount"), Floating.IslandCount);
                Measurements->SetNumberField(TEXT("totalProximityIslands"), Floating.IslandCount);
                Measurements->SetNumberField(TEXT("nonMainProximityIslands"),
                                             Floating.NonMainIslandCount);
                Measurements->SetNumberField(TEXT("componentCount"), Floating.ComponentCount);
                Measurements->SetNumberField(TEXT("floatingCount"), Floating.FloatingCount);
                Measurements->SetNumberField(TEXT("floatingComponentRows"),
                                             Floating.FloatingComponentRows);
                Measurements->SetNumberField(TEXT("suppressedCount"), Floating.SuppressedCount);
                Measurements->SetNumberField(TEXT("unsuppressedCount"), Floating.UnsuppressedCount);
                Measurements->SetNumberField(TEXT("largestComponentIndex"),
                                             Floating.LargestComponentIndex);
                Measurements->SetNumberField(TEXT("largestIslandTriangleCount"),
                                             Floating.LargestIslandTriangleCount);
                Measurements->SetNumberField(TEXT("toleranceFraction"), Floating.ToleranceFraction);
                Measurements->SetNumberField(TEXT("boundingSphereRadius"),
                                             Floating.BoundingSphereRadius);
                Measurements->SetNumberField(TEXT("tolerance"), Floating.Tolerance);

                TArray<TSharedPtr<FJsonValue>> FloatingRows;
                for (const FFloatingComponent& Component : Floating.Components)
                {
                    TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
                    Row->SetNumberField(TEXT("componentIndex"), Component.ComponentIndex);
                    Row->SetNumberField(TEXT("proximityIslandId"), Component.ProximityIslandId);
                    Row->SetNumberField(TEXT("triangleCount"), Component.TriangleCount);
                    Row->SetNumberField(TEXT("signedVolume"), Component.SignedVolume);
                    Row->SetNumberField(TEXT("nearestComponentIndex"),
                                        Component.NearestComponentIndex);
                    Row->SetNumberField(TEXT("nearestDistance"), Component.NearestDistance);
                    TArray<TSharedPtr<FJsonValue>> PartIndices;
                    for (const int32 PartIndex : Component.OwningPartIndices)
                    {
                        PartIndices.Add(MakeShared<FJsonValueNumber>(PartIndex));
                    }
                    Row->SetArrayField(TEXT("partIndices"), PartIndices);
                    TSharedPtr<FJsonObject> Center = MakeShared<FJsonObject>();
                    Center->SetNumberField(TEXT("x"), Component.Center.X);
                    Center->SetNumberField(TEXT("y"), Component.Center.Y);
                    Center->SetNumberField(TEXT("z"), Component.Center.Z);
                    Row->SetObjectField(TEXT("center"), Center);
                    Row->SetBoolField(TEXT("suppressed"), Component.bSuppressed);
                    FloatingRows.Add(MakeShared<FJsonValueObject>(Row));
                }
                Measurements->SetArrayField(TEXT("floatingComponents"), FloatingRows);

                bFlagged = Floating.UnsuppressedCount > 0;
                Message = FString::Printf(
                    TEXT("%d proximity island(s), including %d non-main island(s), produce %d "
                         "floating component row(s) outside the island containing the largest "
                         "component. Tolerance is %.6g (%.6g of the bounding-sphere radius %.6g). "
                         "Each row's triangle count, volume, centre and nearest component distance "
                         "is in measurements."),
                    Floating.IslandCount, Floating.NonMainIslandCount, Floating.FloatingComponentRows,
                    Floating.Tolerance,
                    Floating.ToleranceFraction, Floating.BoundingSphereRadius);
                break;
            }

            case ECheck::ZFighting:
            {
                FZFightAnalysis Analysis;
                AnalyzeZFighting(M.ZFightTriangles, Config.Thresholds, Analysis);
                Measurements->SetNumberField(TEXT("modelTriangleCount"), Analysis.ModelTriangleCount);
                Measurements->SetNumberField(TEXT("validTriangleCount"), Analysis.ValidTriangleCount);
                Measurements->SetNumberField(TEXT("modelExtent"), Analysis.ModelExtent);
                Measurements->SetNumberField(TEXT("planeDistanceEpsilon"), Analysis.PlaneDistanceEpsilon);
                Measurements->SetNumberField(TEXT("normalDotThreshold"), Analysis.NormalDotThreshold);
                Measurements->SetNumberField(TEXT("gridCellSize"), Analysis.GridCellSize);
                Measurements->SetNumberField(TEXT("candidatePairCount"), Analysis.CandidatePairCount);
                 Measurements->SetNumberField(TEXT("exactOverlapTestCount"), Analysis.ExactOverlapTestCount);
                 Measurements->SetNumberField(TEXT("gridReferenceCount"), Analysis.GridReferenceCount);
                 Measurements->SetNumberField(TEXT("largeTriangleCount"), Analysis.LargeTriangleCount);
                 Measurements->SetNumberField(TEXT("largeReferenceInspectCount"),
                                               Analysis.LargeReferenceInspectCount);
                 Measurements->SetNumberField(TEXT("maxLargeReferenceInspectCount"),
                                               Analysis.MaxLargeReferenceInspectCount);
                Measurements->SetNumberField(TEXT("fightingPairCount"), Analysis.FightingPairCount);
                Measurements->SetNumberField(TEXT("fightingTriangleCount"), Analysis.FightingTriangleCount);
                Measurements->SetNumberField(TEXT("regionCount"), Analysis.Regions.Num());
                Measurements->SetNumberField(TEXT("totalOverlapArea"), Analysis.TotalOverlapArea);

                TArray<TSharedPtr<FJsonValue>> Regions;
                for (const FZFightRegion& Region : Analysis.Regions)
                {
                    TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
                    Row->SetNumberField(TEXT("rank"), Region.Rank);
                    Row->SetNumberField(TEXT("overlapArea"), Region.OverlapArea);
                    Row->SetNumberField(TEXT("largestPairOverlapArea"), Region.LargestPairOverlapArea);
                    Row->SetNumberField(TEXT("pairCount"), Region.PairCount);
                    TArray<TSharedPtr<FJsonValue>> Components;
                    for (const int32 Id : Region.ComponentIds)
                    {
                        Components.Add(MakeShared<FJsonValueNumber>(Id));
                    }
                    Row->SetArrayField(TEXT("componentIds"), Components);
                    TArray<TSharedPtr<FJsonValue>> Triangles;
                    for (const int32 Id : Region.TriangleIds)
                    {
                        Triangles.Add(MakeShared<FJsonValueNumber>(Id));
                    }
                    Row->SetArrayField(TEXT("triangleIds"), Triangles);
                    TArray<TSharedPtr<FJsonValue>> Pairs;
                    for (const FZFightRegion::FPairEvidence& Pair : Region.TopPairs)
                    {
                        TSharedPtr<FJsonObject> PairRow = MakeShared<FJsonObject>();
                        PairRow->SetNumberField(TEXT("triangleA"), Pair.TriangleA);
                        PairRow->SetNumberField(TEXT("triangleB"), Pair.TriangleB);
                        PairRow->SetNumberField(TEXT("componentA"), Pair.ComponentA);
                        PairRow->SetNumberField(TEXT("componentB"), Pair.ComponentB);
                        PairRow->SetNumberField(TEXT("overlapArea"), Pair.OverlapArea);
                        PairRow->SetNumberField(TEXT("planeDistance"), Pair.PlaneDistance);
                        PairRow->SetNumberField(TEXT("absNormalDot"), Pair.AbsNormalDot);
                        Pairs.Add(MakeShared<FJsonValueObject>(PairRow));
                    }
                    Row->SetArrayField(TEXT("topPairs"), Pairs);
                    Regions.Add(MakeShared<FJsonValueObject>(Row));
                }
                Measurements->SetArrayField(TEXT("regions"), Regions);

                if (Analysis.bUnrunnable || Analysis.ValidTriangleCount == 0)
                {
                    ++Tally.Unrunnable;
                    const FString Reason = Analysis.bUnrunnable
                        ? Analysis.UnrunnableReason
                        : FString::Printf(TEXT("No finite, non-degenerate triangles were available "
                                              "for the z-fighting detector (%d triangle(s) read)."),
                                           Analysis.ModelTriangleCount);
                    PushFinding(R, Config.MaxFindings, AssetPath, AssetName, Info,
                                EFindingStatus::Unrunnable,
                                Analysis.bUnrunnable ? Analysis.UnrunnableCode
                                                     : ErrorCodes::ERR_MESH_AUDIT_Z_FIGHTING_UNRUNNABLE,
                                Reason, Measurements);
                    continue;
                }

                bFlagged = Analysis.FightingPairCount > 0;
                Message = FString::Printf(
                    TEXT("%d ranked region(s), %d fighting pair(s), and %.6g total projected overlap "
                         "area. Plane epsilon is %.6g from model extent %.6g; normal threshold is %.6g. "
                         "This is a geometric depth-buffer proxy, not a universal camera-distance prediction."),
                    Analysis.Regions.Num(), Analysis.FightingPairCount, Analysis.TotalOverlapArea,
                    Analysis.PlaneDistanceEpsilon, Analysis.ModelExtent, Analysis.NormalDotThreshold);
                break;
            }

            default:
                break;
            }

            if (bFlagged)
            {
                ++Tally.Flagged;
                PushFinding(R, Config.MaxFindings, AssetPath, AssetName, Info,
                            EFindingStatus::Flagged, Info.Code, Message, Measurements);
            }
            else
            {
                ++Tally.Clean;
            }
        }

        if (R.ErrorCount != ErrorsBefore || R.WarningCount != WarningsBefore
            || R.UnrunnableCount != UnrunnableBefore)
        {
            ++R.AssetsFlagged;
        }
        else if (R.CleanAssets.Num() < Config.MaxCleanAssets)
        {
            R.CleanAssets.Add(AssetPath);
        }
    }

    // ---- The sweep ------------------------------------------------------------------------------

    void Run(const TArray<FAssetData>& Page, int32 Offset, int32 Matched, const FConfig& Config,
             FReport& R)
    {
        R.Offset = Offset;
        R.AssetsMatched = Matched;

        // ONE scratch mesh for the whole page. CopyMeshFromStaticMesh replaces the
        // destination's contents wholesale, so reuse is correct by construction and there is
        // no per-asset clear step to forget. This is also the concrete form of "batch by
        // construction": the expensive allocation happens once per CALL, not once per asset,
        // and there is no per-asset entry point that could tempt a caller into a loop.
        UDynamicMesh* Scratch = GeometryUtils::GetOrCreateDynamicMesh(GetTransientPackage());

        for (const FAssetData& Data : Page)
        {
            const FString AssetPath = Data.GetObjectPathString();
            const FString AssetName = Data.AssetName.ToString();

            FAssetMeasurement Measurement;
            if (!Scratch)
            {
                Measurement.UnrunnableCode = ErrorCodes::ERR_MESH_AUDIT_READ_FAILED;
                Measurement.UnrunnableReason =
                    TEXT("Could not allocate a destination dynamic mesh for the sweep.");
            }
            else
            {
                // GetAsset() LOADS the package. That cost is why this verb pages and why it
                // returns a ticket; it is not avoidable, because the orientation of a mesh is
                // not in the asset registry's tags.
                UStaticMesh* Mesh = Cast<UStaticMesh>(Data.GetAsset());
                MeasureStaticMesh(Mesh, Config, Scratch, Measurement);
            }

            EvaluateAsset(AssetPath, AssetName, Measurement, Config, R);
        }

        if (Scratch)
        {
            Scratch->MarkAsGarbage();
        }

        // Same derivation as LevelAuditUtils.cpp:1048. A page that stopped short of the match
        // set has not audited the set, and saying so is what stops "50 of 5000 were clean"
        // from being read as "the folder is clean".
        R.bPageTruncated = (Offset + R.AssetsExamined) < Matched;

        if (Config.Read.LodSource == ELodSource::RenderData)
        {
            R.Caveats.Add(TEXT(
                "Read from RenderData, which is split at every UV seam and hard-normal crease. A "
                "perfectly closed authored mesh reads as thousands of boundary edges there, so "
                "not_closed will fire on almost everything and every closed-only check except "
                "inverted goes not-applicable. Inverted reports those open components as "
                "UNKNOWN/unrunnable. Re-run with the default lodType for an orientation verdict."));
        }
        if (!Config.Read.bUseBuildScale)
        {
            R.Caveats.Add(TEXT(
                "Read with useBuildScale:false, so a mirroring Build Scale was NOT applied. The "
                "triangles measured are the authored ones, not the ones that ship; check the "
                "mirrored_build_scale rows for assets the build inverts on its way out."));
        }
    }
}
