// Copyright (c) 2026 Alexander Penkin. MIT License.

// geometry.audit_static_meshes `self_intersection`: the third gate term on a SAVED StaticMesh
// without spawning an actor (F-self-intersection-no-asset-route). The fixture goes through a
// real UStaticMesh - written with CopyMeshToStaticMesh into a transient asset, read back with
// MeasureStaticMesh - so the test covers the asset read path, not only the evaluator.
#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Geometry/GeometryOps_Primitives.h"
#include "Handlers/Geometry/MeshAuditUtils.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "Engine/StaticMesh.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace MeshAuditSITest
{
    // One open, edge-connected surface whose second wall passes through its floor: eight
    // triangles, no shared vertex between the crossing pairs (same shape as the .pwmodel
    // self-intersection fixture). Every other audit check is blind to it.
    UDynamicMesh* NewCrossingSheets()
    {
        UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage());
        Mesh->EditMesh([](UE::Geometry::FDynamicMesh3& Edit)
        {
            const int32 H0 = Edit.AppendVertex(FVector3d(0, -10, 0));
            const int32 H1 = Edit.AppendVertex(FVector3d(0, 10, 0));
            const int32 H2 = Edit.AppendVertex(FVector3d(20, -10, 0));
            const int32 H3 = Edit.AppendVertex(FVector3d(20, 10, 0));
            const int32 H4 = Edit.AppendVertex(FVector3d(40, -10, 0));
            const int32 H5 = Edit.AppendVertex(FVector3d(40, 10, 0));
            const int32 V0 = Edit.AppendVertex(FVector3d(10, -10, 20));
            const int32 V1 = Edit.AppendVertex(FVector3d(10, 10, 20));
            const int32 V2 = Edit.AppendVertex(FVector3d(30, -10, -10));
            const int32 V3 = Edit.AppendVertex(FVector3d(30, 10, -10));
            Edit.AppendTriangle(H0, H2, H3);
            Edit.AppendTriangle(H0, H3, H1);
            Edit.AppendTriangle(H2, H4, H5);
            Edit.AppendTriangle(H2, H5, H3);
            Edit.AppendTriangle(H0, H1, V1);
            Edit.AppendTriangle(H0, V1, V0);
            Edit.AppendTriangle(V0, V1, V3);
            Edit.AppendTriangle(V0, V3, V2);
        }, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
        return Mesh;
    }

    UDynamicMesh* NewBox()
    {
        UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage());
        GeometryOps::FBoxParams Params;
        Params.Size = FVector(100.0, 100.0, 100.0);
        GeometryOps::GenerateBox(Mesh, Params, FTransform::Identity);
        return Mesh;
    }

    // A transient UStaticMesh holding Source as its LOD 0 source model. Null on failure.
    UStaticMesh* BakeTransientStaticMesh(UDynamicMesh* Source)
    {
        UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage());
        FGeometryScriptCopyMeshToAssetOptions Options;
        Options.bEmitTransaction = false;
        FGeometryScriptMeshWriteLOD Target;
        EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
        UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh(
            Source, Mesh, Options, Target, Outcome, /*bUseSectionMaterials=*/true, nullptr);
#else
        UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh(
            Source, Mesh, Options, Target, Outcome, nullptr);
#endif
        return Outcome == EGeometryScriptOutcomePins::Success ? Mesh : nullptr;
    }

    // MeasureStaticMesh + EvaluateAsset, the per-asset body of MeshAudit::Run.
    MeshAudit::FReport Audit(UStaticMesh* Mesh, uint32 Checks, MeshAudit::FAssetMeasurement& OutM)
    {
        MeshAudit::FConfig Config;
        Config.SelectedChecks = Checks;
        TStrongObjectPtr<UDynamicMesh> Scratch(NewObject<UDynamicMesh>(GetTransientPackage()));
        MeshAudit::MeasureStaticMesh(Mesh, Config, Scratch.Get(), OutM);
        MeshAudit::FReport R;
        MeshAudit::EvaluateAsset(TEXT("/Engine/Transient.SM_Fixture"), TEXT("SM_Fixture"),
                                 OutM, Config, R);
        return R;
    }

    const MeshAudit::FFinding* FindSelfIntersection(const MeshAudit::FReport& R)
    {
        return R.Findings.FindByPredicate([](const MeshAudit::FFinding& F)
        {
            return F.Check == MeshAudit::ECheck::SelfIntersection;
        });
    }

    const MeshAudit::FCheckTally& Tally(const MeshAudit::FReport& R)
    {
        return R.Tallies[static_cast<int32>(MeshAudit::ECheck::SelfIntersection)];
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshAuditSelfIntersectionSavedMeshTest,
    "PinWright.Geometry.MeshAudit.SelfIntersection.SavedMeshCrossingItselfIsFlagged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshAuditSelfIntersectionSavedMeshTest::RunTest(const FString& Parameters)
{
    const uint32 Only = MeshAudit::CheckBit(MeshAudit::ECheck::SelfIntersection);

    TStrongObjectPtr<UDynamicMesh> Sheets(MeshAuditSITest::NewCrossingSheets());
    TStrongObjectPtr<UStaticMesh> Crossing(MeshAuditSITest::BakeTransientStaticMesh(Sheets.Get()));
    if (!TestNotNull(TEXT("the crossing fixture bakes into a StaticMesh"), Crossing.Get()))
    {
        return false;
    }

    // Default set: the check is opt-in, so nobody looked - and the row says so.
    {
        MeshAudit::FAssetMeasurement M;
        const MeshAudit::FReport R =
            MeshAuditSITest::Audit(Crossing.Get(), MeshAudit::DefaultCheckMask(), M);
        TestTrue(TEXT("the default sweep reads the asset"), M.bMeasured);
        TestFalse(TEXT("and does not pay for the self-intersection descent"),
            M.SelfIntersection.bMeasured);
        TestEqual(TEXT("so the unselected row is empty, not clean"),
            MeshAuditSITest::Tally(R).Applicable, 0);
    }

    MeshAudit::FAssetMeasurement M;
    const MeshAudit::FReport R = MeshAuditSITest::Audit(Crossing.Get(), Only, M);
    TestTrue(TEXT("the asset is read"), M.bMeasured);
    TestEqual(TEXT("fixture survives the asset round trip as eight triangles"),
        M.Health.TriangleCount, 8);
    TestEqual(TEXT("in ONE edge-connected component (else the per-shell rule excludes it)"),
        M.Health.ComponentCount, 1);
    TestTrue(TEXT("self-intersection is measured on the saved mesh"), M.SelfIntersection.bMeasured);

    TestEqual(TEXT("the row is applicable"), MeshAuditSITest::Tally(R).Applicable, 1);
    TestEqual(TEXT("and flagged"), MeshAuditSITest::Tally(R).Flagged, 1);
    const MeshAudit::FFinding* Finding = MeshAuditSITest::FindSelfIntersection(R);
    if (TestNotNull(TEXT("a self_intersection finding is emitted"), Finding))
    {
        TestEqual(TEXT("coded MESH_AUDIT_SELF_INTERSECTION"), Finding->Code,
            FString(ErrorCodes::ERR_MESH_AUDIT_SELF_INTERSECTION));
        TestTrue(TEXT("as a flagged error"),
            Finding->Status == MeshAudit::EFindingStatus::Flagged
            && Finding->Severity == MeshAudit::ESeverity::Error);
        double Pairs = 0.0;
        TestTrue(TEXT("measurements carry the pair count"),
            Finding->Measurements.IsValid()
            && Finding->Measurements->TryGetNumberField(TEXT("selfIntersections"), Pairs)
            && Pairs > 0.0);
        TestTrue(TEXT("and a witness point"),
            Finding->Measurements.IsValid() && Finding->Measurements->HasField(TEXT("witness")));
    }
    TestFalse(TEXT("so the sweep fails"), R.DerivePass(TEXT("error")));

    // Control: a correct closed box must stay clean, or the check gets switched off.
    TStrongObjectPtr<UDynamicMesh> BoxSource(MeshAuditSITest::NewBox());
    TStrongObjectPtr<UStaticMesh> Box(MeshAuditSITest::BakeTransientStaticMesh(BoxSource.Get()));
    if (TestNotNull(TEXT("the box control bakes into a StaticMesh"), Box.Get()))
    {
        MeshAudit::FAssetMeasurement BoxM;
        const MeshAudit::FReport BoxR = MeshAuditSITest::Audit(Box.Get(), Only, BoxM);
        TestTrue(TEXT("the box is measured"), BoxM.SelfIntersection.bMeasured);
        TestEqual(TEXT("and clean"), MeshAuditSITest::Tally(BoxR).Clean, 1);
        TestNull(TEXT("with no finding"), MeshAuditSITest::FindSelfIntersection(BoxR));
        TestTrue(TEXT("so the box passes"), BoxR.DerivePass(TEXT("error")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshAuditSelfIntersectionDeclinedTest,
    "PinWright.Geometry.MeshAudit.SelfIntersection.OptInAndDeclinedIsUnrunnable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshAuditSelfIntersectionDeclinedTest::RunTest(const FString& Parameters)
{

    MeshAudit::ECheck Parsed = MeshAudit::ECheck::Inverted;
    TestTrue(TEXT("self_intersection is a check id"),
        MeshAudit::ParseCheckId(TEXT("self_intersection"), Parsed)
        && Parsed == MeshAudit::ECheck::SelfIntersection);
    TestFalse(TEXT("and it is off by default"),
        MeshAudit::HasCheck(MeshAudit::DefaultCheckMask(), MeshAudit::ECheck::SelfIntersection));

    // A non-empty mesh whose measurement declined (the over-budget case): bMeasured is false.
    TStrongObjectPtr<UDynamicMesh> Box(MeshAuditSITest::NewBox());
    MeshAudit::FAssetMeasurement M;
    M.bMeasured = true;
    M.Health = GeometryUtils::MeasureMeshHealth(Box.Get());
    TestTrue(TEXT("fixture precondition: the mesh is not empty"), M.Health.TriangleCount > 0);
    TestFalse(TEXT("fixture precondition: self-intersection was not measured"),
        M.SelfIntersection.bMeasured);

    MeshAudit::FConfig Config;
    Config.SelectedChecks = MeshAudit::CheckBit(MeshAudit::ECheck::SelfIntersection);
    MeshAudit::FReport R;
    MeshAudit::EvaluateAsset(TEXT("/Game/T/Big"), TEXT("Big"), M, Config, R);

    TestEqual(TEXT("a declined measurement is unrunnable"),
        MeshAuditSITest::Tally(R).Unrunnable, 1);
    TestEqual(TEXT("never clean"), MeshAuditSITest::Tally(R).Clean, 0);
    const MeshAudit::FFinding* Finding = MeshAuditSITest::FindSelfIntersection(R);
    if (TestNotNull(TEXT("and reported as a row"), Finding))
    {
        TestEqual(TEXT("coded MESH_AUDIT_SELF_INTERSECTION_UNRUNNABLE"), Finding->Code,
            FString(ErrorCodes::ERR_MESH_AUDIT_SELF_INTERSECTION_UNRUNNABLE));
        TestTrue(TEXT("with unrunnable status"),
            Finding->Status == MeshAudit::EFindingStatus::Unrunnable);
    }
    TestFalse(TEXT("so the sweep cannot pass even with failOn none"), R.DerivePass(TEXT("none")));
    return true;
}
