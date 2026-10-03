// Copyright (c) 2026 Alexander Penkin. MIT License.

// MeshAuditSelfIntersection.cpp - the `self_intersection` row of geometry.audit_static_meshes.
//
// The measurement is GeometryUtils::MeasureMeshSelfIntersection, the same one
// geometry.check_health runs under checkSelfIntersection, so the asset sweep and the actor
// verb cannot disagree about a mesh. Before this row the third term of the published gate
// `isClosed && signedVolume > 0 && selfIntersections === 0` had no route to a saved StaticMesh
// that did not spawn an actor (F-self-intersection-no-asset-route).
#include "Handlers/Geometry/MeshAuditUtils.h"

namespace MeshAudit
{
    bool DescribeSelfIntersection(const FAssetMeasurement& M, FJsonObject& Measurements,
                                  bool& bOutFlagged, FString& OutMessage)
    {
        const GeometryUtils::FMeshSelfIntersection& S = M.SelfIntersection;
        Measurements.SetBoolField(TEXT("selfIntersectionsMeasured"), S.bMeasured);
        Measurements.SetNumberField(TEXT("selfIntersectionMaxTriangles"),
                                    GeometryUtils::SelfIntersectionMaxTriangles);
        if (!S.bMeasured)
        {
            // Empty meshes never reach here (not-applicable), so a decline is the budget.
            OutMessage = FString::Printf(
                TEXT("Self-intersection was not measured: the mesh has %d triangle(s), above the ")
                TEXT("%d-triangle budget of the per-component AABB descent. Unmeasured, not clean."),
                M.Health.TriangleCount, GeometryUtils::SelfIntersectionMaxTriangles);
            return false;
        }

        Measurements.SetNumberField(TEXT("selfIntersections"), S.PairCount);
        Measurements.SetNumberField(TEXT("selfIntersectingComponents"),
                                    S.SelfIntersectingComponents);
        Measurements.SetBoolField(TEXT("selfIntersectionsTruncated"), S.bTruncated);
        if (S.bHasWitness)
        {
            TSharedPtr<FJsonObject> Witness = MakeShared<FJsonObject>();
            Witness->SetNumberField(TEXT("x"), S.Witness.X);
            Witness->SetNumberField(TEXT("y"), S.Witness.Y);
            Witness->SetNumberField(TEXT("z"), S.Witness.Z);
            Measurements.SetObjectField(TEXT("witness"), Witness);
        }

        bOutFlagged = S.PairCount > 0;
        OutMessage = FString::Printf(
            TEXT("%s%d crossing triangle pair(s) in %d edge-connected component(s): the surface ")
            TEXT("passes through itself, so `isClosed && signedVolume > 0` is not evidence of a ")
            TEXT("solid. Separate shells that interpenetrate are not counted. `witness` is a ")
            TEXT("point on the first crossing."),
            S.bTruncated ? TEXT("At least ") : TEXT(""), S.PairCount,
            S.SelfIntersectingComponents);
        return true;
    }
}
