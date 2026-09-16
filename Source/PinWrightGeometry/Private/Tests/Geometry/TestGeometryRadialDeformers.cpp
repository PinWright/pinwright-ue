// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the two radial deformers, GeometryOps::Spherify and
// GeometryOps::Cylindrify (GeometryOps_Modeling.h).
//
// They exist because of a defect that was diagnosed wrong three times: `box size=(150,118,92)`
// + `spherify 0.72-0.82` + `noise_deform` left a black cavity in the boulder's flank, and the
// blame went first to spherify's radial math and then to the engine. Neither is guilty - the map
// is radially monotone and direction-preserving, so it cannot fold a convex surface, and a
// faithful FGridBoxMeshGenerator model measures 0 inverted, 0 degenerate and 0 self-intersecting
// triangles at up to 10:1 aspect and factor 1.0. What the op DOES do on an oblong box is skew the
// triangle sizes it hands to the next op, and there was no diagnostic for that at all.
//
// What each test guards, and what breaks it:
//
//  1. The anisotropy warning fires on the two measured FAILING configurations and carries the
//     numbers (extents, factor, resulting radial scales) an author needs to act. Reverting the
//     warning removes it and the test fails.
//  2. It does NOT fire on the measured CLEAN configuration, nor on a perfect cube at factor 1.0.
//     The cube case is the one that rules out the obvious-but-wrong metric: the raw radial scale
//     spread is sqrt(3) = 1.73 on a cube - HIGHER than the 1.58 of the box the shipped
//     crystal_cluster.pwmodel calls clean - so a threshold on spread would warn loudest about
//     the op's intended use. Without this test the threshold means nothing.
//  3. Numeric equivalence for the EditMesh rewrite. Both ops moved to a single locked pass over
//     FDynamicMesh3 from two Geometry Script library calls PER VERTEX; these pin every resulting
//     position against the closed form, plus the counts, plus FCylindrifyOutcome.
//  4. Gap safety. The old loop walked GetAllVertexIDs, which reports gaps through a bool nobody
//     read; the replacement walks VertexIndicesItr. A mesh with a freed vertex ID in the middle
//     of its ID space is the input that separates the two.
#include "Misc/AutomationTest.h"

#include "Handlers/Geometry/GeometryOps_Modeling.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include "GeometryScript/MeshPrimitiveFunctions.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper
// with a common name would collide with a sibling test TU once Unity merges them.

// A gridded box centred on the origin. `Segments` is VERTICES PER EDGE, not quads - AppendBox
// forwards it to FGridBoxMeshGenerator::EdgeVertices, which opens with FMath::Max(2, N). (5,5,4)
// is therefore 82 vertices and 160 triangles, and it is the tessellation the failing .pwmodel
// used.
UDynamicMesh* GeometryRadialOpsTest_NewBox(double SizeX, double SizeY, double SizeZ,
                                           int32 StepsX = 5, int32 StepsY = 5, int32 StepsZ = 4)
{
    UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage());
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
        Mesh, Options, FTransform::Identity,
        static_cast<float>(SizeX), static_cast<float>(SizeY), static_cast<float>(SizeZ),
        StepsX, StepsY, StepsZ, EGeometryScriptPrimitiveOriginMode::Center, nullptr);
    return Mesh;
}

bool GeometryRadialOpsTest_AnyWarningContains(const GeometryOps::FOpResult& Op, const TCHAR* Fragment)
{
    for (const FString& Warning : Op.Warnings)
    {
        if (Warning.Contains(Fragment))
        {
            return true;
        }
    }
    return false;
}

FString GeometryRadialOpsTest_JoinWarnings(const GeometryOps::FOpResult& Op)
{
    return FString::Join(Op.Warnings, TEXT(" | "));
}

// Every vertex position, keyed by the ID it lives at, so a later assertion can compare the
// deformed mesh against the closed form vertex by vertex rather than against a count.
TMap<int32, FVector3d> GeometryRadialOpsTest_SnapshotVertices(UDynamicMesh* Mesh)
{
    TMap<int32, FVector3d> Snapshot;
    Mesh->ProcessMesh([&Snapshot](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        for (int32 VertexID : ReadMesh.VertexIndicesItr())
        {
            Snapshot.Add(VertexID, ReadMesh.GetVertex(VertexID));
        }
    });
    return Snapshot;
}

// The bounding box the ops read, recomputed here from the snapshot rather than from the op, so a
// change to which box the op measures shows up as a failure instead of cancelling out.
FBox GeometryRadialOpsTest_BoundsOf(const TMap<int32, FVector3d>& Snapshot)
{
    FBox Box(ForceInit);
    for (const TPair<int32, FVector3d>& Entry : Snapshot)
    {
        Box += FVector(Entry.Value);
    }
    return Box;
}
}

// ============================================================================
// The diagnostic: it must fire on the measured failures and stay silent on the measured clean
// case. Both halves are load-bearing; only the second one makes the threshold mean anything.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometrySpherifyAnisotropyWarningTest,
    "PinWright.Geometry.Ops.Modeling.SpherifyWarnsOnOblongSourceGeometry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometrySpherifyAnisotropyWarningTest::RunTest(const FString& Parameters)
{
    // The configuration the .pwmodel comment records as producing a black cavity in the flank.
    // Half-extents 75 / 59 / 46, so the aspect is 75/46 = 1.630:1 and the anisotropy at factor
    // 0.82 is 0.82 * 0.630 = 0.517, above the 1/3 threshold.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeometryRadialOpsTest_NewBox(150.0, 118.0, 92.0));
        GeometryOps::FSpherifyParams Params;
        Params.Factor = 0.82;
        const GeometryOps::FOpResult Op = GeometryOps::Spherify(Mesh.Get(), Params);

        TestTrue(TEXT("spherify still succeeds on the oblong box"), Op.bSuccess);
        TestTrue(TEXT("spherify warns that the radial map is strongly anisotropic"),
            GeometryRadialOpsTest_AnyWarningContains(Op, TEXT("anisotropic")));

        // The numbers, not just the fact of a warning: an author has to be able to read the
        // shape, the factor and the resulting scales out of the text.
        const FString Text = GeometryRadialOpsTest_JoinWarnings(Op);
        TestTrue(TEXT("the warning names the bounding box"), Text.Contains(TEXT("150")));
        TestTrue(TEXT("the warning names the other two dimensions"),
            Text.Contains(TEXT("118")) && Text.Contains(TEXT("92")));
        TestTrue(TEXT("the warning names the aspect ratio"), Text.Contains(TEXT("1.63:1")));
        TestTrue(TEXT("the warning names the factor it ran at"), Text.Contains(TEXT("0.82")));
        // (1 - 0.82) + 0.82 * 75/46 = 1.517 at the flattest face, exactly 1.000 at the longest.
        TestTrue(TEXT("the warning names the scale at the flattest direction"),
            Text.Contains(TEXT("1.517")));
        TestTrue(TEXT("the warning names the scale at the longest direction"),
            Text.Contains(TEXT("1.000")));
        TestTrue(TEXT("the warning names a remedy"), Text.Contains(TEXT("remesh_uniform")));
    }

    // Same box, the low end of the range the author reported failing: 0.72 * 0.630 = 0.454.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeometryRadialOpsTest_NewBox(150.0, 118.0, 92.0));
        GeometryOps::FSpherifyParams Params;
        Params.Factor = 0.72;
        const GeometryOps::FOpResult Op = GeometryOps::Spherify(Mesh.Get(), Params);
        TestTrue(TEXT("the low end of the failing factor range warns too"),
            GeometryRadialOpsTest_AnyWarningContains(Op, TEXT("anisotropic")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometrySpherifyNoFalsePositiveTest,
    "PinWright.Geometry.Ops.Modeling.SpherifyStaysSilentOnNearCubicSourceGeometry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometrySpherifyNoFalsePositiveTest::RunTest(const FString& Parameters)
{
    // The configuration the shipped Examples/pwmodel/crystal_cluster.pwmodel actually uses, and
    // calls clean in its own comment: half-extents 66 / 59 / 52, aspect 1.269:1, anisotropy
    // 0.70 * 0.269 = 0.189 - below 1/3.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeometryRadialOpsTest_NewBox(132.0, 118.0, 104.0));
        GeometryOps::FSpherifyParams Params;
        Params.Factor = 0.70;
        const GeometryOps::FOpResult Op = GeometryOps::Spherify(Mesh.Get(), Params);

        TestTrue(TEXT("spherify succeeds on the near-cubic box"), Op.bSuccess);
        TestFalse(TEXT("the near-cubic box does not trip the anisotropy warning"),
            GeometryRadialOpsTest_AnyWarningContains(Op, TEXT("anisotropic")));
        TestEqual(TEXT("and warns about nothing else either"), Op.Warnings.Num(), 0);
    }

    // The case that rules out the obvious-but-wrong metric. A perfect cube at factor 1.0 has a
    // raw radial scale spread of sqrt(3) = 1.73, HIGHER than the 1.58 of the near-cubic box
    // above, because corner-vs-face-centre spread is inherent to turning any box into a sphere.
    // A threshold on spread would fire hardest on the op's intended use. The anisotropy metric
    // is exactly 0 here at every factor.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeometryRadialOpsTest_NewBox(100.0, 100.0, 100.0));
        GeometryOps::FSpherifyParams Params;
        Params.Factor = 1.0;
        const GeometryOps::FOpResult Op = GeometryOps::Spherify(Mesh.Get(), Params);
        TestFalse(TEXT("a fully spherified cube is the intended use and must not warn"),
            GeometryRadialOpsTest_AnyWarningContains(Op, TEXT("anisotropic")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCylindrifyAnisotropyWarningTest,
    "PinWright.Geometry.Ops.Modeling.CylindrifyWarnsOnOblongCrossSectionOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryCylindrifyAnisotropyWarningTest::RunTest(const FString& Parameters)
{
    // Cylindrify's anisotropy is measured across the CROSS-SECTION, so the axis dimension is
    // irrelevant to it: 200 x 100 in X/Y is 2:1 whatever Z is.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeometryRadialOpsTest_NewBox(200.0, 100.0, 92.0));
        GeometryOps::FCylindrifyParams Params;
        Params.Axis = GeometryOps::EMeshAxis::Z;
        Params.Factor = 0.82;
        const GeometryOps::FOpResult Op = GeometryOps::Cylindrify(Mesh.Get(), Params);

        TestTrue(TEXT("cylindrify succeeds on the oblong cross-section"), Op.bSuccess);
        TestTrue(TEXT("cylindrify warns on a 2:1 cross-section"),
            GeometryRadialOpsTest_AnyWarningContains(Op, TEXT("anisotropic")));
        const FString Text = GeometryRadialOpsTest_JoinWarnings(Op);
        TestTrue(TEXT("and says the aspect it measured is the cross-section's"),
            Text.Contains(TEXT("cross-section aspect ratio")));
        TestTrue(TEXT("and names the op rather than spherify"), Text.Contains(TEXT("cylindrify")));
    }

    // A square cross-section is isotropic under this map at every factor and every fitted
    // radius, exactly as a cube is under spherify. The long Z is deliberate: it proves the
    // metric ignores the axis dimension.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeometryRadialOpsTest_NewBox(120.0, 120.0, 400.0));
        GeometryOps::FCylindrifyParams Params;
        Params.Axis = GeometryOps::EMeshAxis::Z;
        Params.Factor = 0.82;
        const GeometryOps::FOpResult Op = GeometryOps::Cylindrify(Mesh.Get(), Params);
        TestFalse(TEXT("a square cross-section does not warn however long the axis is"),
            GeometryRadialOpsTest_AnyWarningContains(Op, TEXT("anisotropic")));
    }

    return true;
}

// ============================================================================
// The EditMesh rewrite: same arithmetic, same counts, same outcome struct.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometrySpherifyClosedFormTest,
    "PinWright.Geometry.Ops.Modeling.SpherifyMatchesItsClosedFormAndKeepsCounts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometrySpherifyClosedFormTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(GeometryRadialOpsTest_NewBox(150.0, 118.0, 92.0));

    const TMap<int32, FVector3d> Before = GeometryRadialOpsTest_SnapshotVertices(Mesh.Get());
    const FBox Bounds = GeometryRadialOpsTest_BoundsOf(Before);
    const FVector3d Center(Bounds.GetCenter());
    const double TargetRadius = Bounds.GetExtent().GetMax();

    TestEqual(TEXT("segments (5,5,4) is 82 vertices, not the 6x6x5 a quad reading would give"),
        Before.Num(), 82);
    TestEqual(TEXT("and 160 triangles"), Mesh->GetTriangleCount(), 160);
    TestEqual(TEXT("the box's largest half-extent is the target radius"), TargetRadius, 75.0);

    const double Factor = 0.82;
    GeometryOps::FSpherifyParams Params;
    Params.Factor = Factor;
    const GeometryOps::FOpResult Op = GeometryOps::Spherify(Mesh.Get(), Params);

    TestTrue(TEXT("spherify succeeds"), Op.bSuccess);
    TestTrue(TEXT("spherify reports a change"), Op.bChanged);
    TestEqual(TEXT("spherify creates and destroys no vertices"), Op.VerticesAfter, Op.VerticesBefore);
    TestEqual(TEXT("spherify creates and destroys no triangles"),
        Op.TrianglesAfter, Op.TrianglesBefore);

    const TMap<int32, FVector3d> After = GeometryRadialOpsTest_SnapshotVertices(Mesh.Get());
    TestEqual(TEXT("every vertex ID survives"), After.Num(), Before.Num());

    int32 Compared = 0;
    double WorstError = 0.0;
    for (const TPair<int32, FVector3d>& Entry : Before)
    {
        const FVector3d* Moved = After.Find(Entry.Key);
        if (!Moved)
        {
            AddError(FString::Printf(TEXT("vertex %d vanished"), Entry.Key));
            continue;
        }

        // P' = C + dir * ((1-t)*d + t*R), i.e. the lerp toward the target sphere written
        // radially. Every vertex of a box is well clear of the centre, so nothing is skipped.
        FVector3d Direction = Entry.Value - Center;
        const double Distance = Direction.Size();
        TestTrue(TEXT("no box vertex sits on the centre"), Distance > KINDA_SMALL_NUMBER);
        Direction /= Distance;
        const FVector3d Expected =
            Center + Direction * ((1.0 - Factor) * Distance + Factor * TargetRadius);

        WorstError = FMath::Max(WorstError, (Expected - *Moved).Size());
        ++Compared;
    }

    TestEqual(TEXT("every vertex was checked"), Compared, 82);
    TestTrue(FString::Printf(
        TEXT("every position matches the closed form (worst error %g uu)"), WorstError),
        WorstError < 1.0e-6);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCylindrifyClosedFormTest,
    "PinWright.Geometry.Ops.Modeling.CylindrifyMatchesItsClosedFormAndOutcome",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryCylindrifyClosedFormTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(GeometryRadialOpsTest_NewBox(150.0, 118.0, 92.0));

    const TMap<int32, FVector3d> Before = GeometryRadialOpsTest_SnapshotVertices(Mesh.Get());
    const FBox Bounds = GeometryRadialOpsTest_BoundsOf(Before);
    const FVector3d Center(Bounds.GetCenter());
    constexpr int32 AxisIndex = 2; // EMeshAxis::Z

    // The average is over EVERY vertex including the ones the second pass skips, and it is taken
    // from the mesh as it arrived. Recomputing it here from the snapshot is what catches a
    // rewrite that fused the two passes and let the mean drift onto already-moved positions.
    double TotalRadius = 0.0;
    for (const TPair<int32, FVector3d>& Entry : Before)
    {
        FVector3d Perp = Entry.Value - Center;
        Perp[AxisIndex] = 0.0;
        TotalRadius += Perp.Size();
    }
    const double ExpectedAvgRadius = TotalRadius / Before.Num();

    const double Factor = 0.5;
    GeometryOps::FCylindrifyParams Params;
    Params.Axis = GeometryOps::EMeshAxis::Z;
    Params.Factor = Factor;
    GeometryOps::FCylindrifyOutcome Outcome;
    const GeometryOps::FOpResult Op = GeometryOps::Cylindrify(Mesh.Get(), Params, &Outcome);

    TestTrue(TEXT("cylindrify succeeds"), Op.bSuccess);
    TestTrue(TEXT("cylindrify reports a change"), Op.bChanged);
    TestEqual(TEXT("cylindrify creates and destroys no vertices"),
        Op.VerticesAfter, Op.VerticesBefore);
    TestEqual(TEXT("cylindrify creates and destroys no triangles"),
        Op.TrianglesAfter, Op.TrianglesBefore);
    TestTrue(FString::Printf(TEXT("avgRadius is the mean over the ORIGINAL positions "
        "(expected %g, got %g)"), ExpectedAvgRadius, Outcome.AverageRadius),
        FMath::Abs(Outcome.AverageRadius - ExpectedAvgRadius) < 1.0e-6);

    // The two vertices the op must skip are the centres of the +Z and -Z faces, at (0, 0, +-46):
    // their perpendicular radius is exactly 0, so they have no direction to be pushed along.
    // 82 - 2 = 80 is therefore a real prediction about this tessellation, not a restatement.
    TestEqual(TEXT("cylindrify moves every vertex except the two on the axis"),
        Outcome.VerticesModified, 80);

    const TMap<int32, FVector3d> After = GeometryRadialOpsTest_SnapshotVertices(Mesh.Get());
    int32 Skipped = 0;
    double WorstError = 0.0;
    for (const TPair<int32, FVector3d>& Entry : Before)
    {
        const FVector3d* Moved = After.Find(Entry.Key);
        if (!Moved)
        {
            AddError(FString::Printf(TEXT("vertex %d vanished"), Entry.Key));
            continue;
        }

        const FVector3d FromCenter = Entry.Value - Center;
        FVector3d Perp = FromCenter;
        Perp[AxisIndex] = 0.0;
        const double PerpDist = Perp.Size();

        FVector3d Expected = Entry.Value;
        if (PerpDist > KINDA_SMALL_NUMBER)
        {
            Perp /= PerpDist;
            FVector3d CylinderPos = Center + Perp * ExpectedAvgRadius;
            CylinderPos[AxisIndex] = Center[AxisIndex] + FromCenter[AxisIndex];
            Expected = FMath::Lerp(Entry.Value, CylinderPos, Factor);
        }
        else
        {
            ++Skipped;
        }

        WorstError = FMath::Max(WorstError, (Expected - *Moved).Size());
    }

    TestEqual(TEXT("exactly the two axis vertices were skipped"), Skipped, 2);
    TestTrue(FString::Printf(
        TEXT("every position matches the closed form (worst error %g uu)"), WorstError),
        WorstError < 1.0e-6);

    return true;
}

// ============================================================================
// Gap safety
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryRadialDeformersGapSafetyTest,
    "PinWright.Geometry.Ops.Modeling.RadialDeformersSurviveGapsInTheVertexIDSpace",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryRadialDeformersGapSafetyTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(GeometryRadialOpsTest_NewBox(150.0, 118.0, 92.0));

    // Three unreferenced vertices appended, the middle one freed. That is the cheapest way to
    // put a hole in the ID space without disturbing the box's surface or its bounding box - the
    // three sit well inside it. A loop that walked 0..MaxVertexID would read the freed slot.
    int32 FreedVertexID = INDEX_NONE;
    int32 LowLooseID = INDEX_NONE;
    int32 HighLooseID = INDEX_NONE;
    Mesh->EditMesh([&](UE::Geometry::FDynamicMesh3& EditMesh)
    {
        LowLooseID = EditMesh.AppendVertex(FVector3d(10.0, 0.0, 0.0));
        FreedVertexID = EditMesh.AppendVertex(FVector3d(0.0, 20.0, 0.0));
        HighLooseID = EditMesh.AppendVertex(FVector3d(0.0, 0.0, 30.0));
        EditMesh.RemoveVertex(FreedVertexID);
    });

    int32 VertexCount = 0;
    int32 MaxVertexID = 0;
    bool bFreedSlotIsGone = true;
    Mesh->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        VertexCount = ReadMesh.VertexCount();
        MaxVertexID = ReadMesh.MaxVertexID();
        bFreedSlotIsGone = !ReadMesh.IsVertex(FreedVertexID);
    });

    TestTrue(TEXT("the freed slot really is freed"), bFreedSlotIsGone);
    TestTrue(FString::Printf(TEXT("the ID space really has a gap (count %d, max ID %d)"),
        VertexCount, MaxVertexID), MaxVertexID > VertexCount);
    TestEqual(TEXT("the box plus its two surviving loose vertices"), VertexCount, 84);

    const TMap<int32, FVector3d> Before = GeometryRadialOpsTest_SnapshotVertices(Mesh.Get());
    const FBox Bounds = GeometryRadialOpsTest_BoundsOf(Before);
    const FVector3d Center(Bounds.GetCenter());
    const double TargetRadius = Bounds.GetExtent().GetMax();
    TestEqual(TEXT("the loose vertices sit inside the box and do not move its bounds"),
        TargetRadius, 75.0);

    const double Factor = 0.6;
    GeometryOps::FSpherifyParams Params;
    Params.Factor = Factor;
    const GeometryOps::FOpResult Op = GeometryOps::Spherify(Mesh.Get(), Params);

    TestTrue(TEXT("spherify succeeds on a gapped mesh"), Op.bSuccess);
    TestEqual(TEXT("it neither resurrects the freed ID nor drops a live one"),
        Op.VerticesAfter, VertexCount);

    const TMap<int32, FVector3d> After = GeometryRadialOpsTest_SnapshotVertices(Mesh.Get());
    TestFalse(TEXT("the freed ID is still absent afterwards"), After.Contains(FreedVertexID));
    TestTrue(TEXT("the loose vertex BELOW the gap was visited"), After.Contains(LowLooseID));
    TestTrue(TEXT("the loose vertex ABOVE the gap was visited"), After.Contains(HighLooseID));

    double WorstError = 0.0;
    for (const TPair<int32, FVector3d>& Entry : Before)
    {
        const FVector3d* Moved = After.Find(Entry.Key);
        if (!Moved)
        {
            AddError(FString::Printf(TEXT("vertex %d vanished"), Entry.Key));
            continue;
        }
        FVector3d Direction = Entry.Value - Center;
        const double Distance = Direction.Size();
        Direction /= Distance;
        const FVector3d Expected =
            Center + Direction * ((1.0 - Factor) * Distance + Factor * TargetRadius);
        WorstError = FMath::Max(WorstError, (Expected - *Moved).Size());
    }
    TestTrue(FString::Printf(
        TEXT("every live vertex across the gap moved correctly (worst error %g uu)"), WorstError),
        WorstError < 1.0e-6);

    // Cylindrify walks the same iterator twice, so it gets the same input.
    TStrongObjectPtr<UDynamicMesh> Other(GeometryRadialOpsTest_NewBox(150.0, 118.0, 92.0));
    Other->EditMesh([](UE::Geometry::FDynamicMesh3& EditMesh)
    {
        EditMesh.AppendVertex(FVector3d(10.0, 0.0, 0.0));
        const int32 Doomed = EditMesh.AppendVertex(FVector3d(0.0, 20.0, 0.0));
        EditMesh.AppendVertex(FVector3d(0.0, 0.0, 30.0));
        EditMesh.RemoveVertex(Doomed);
    });

    GeometryOps::FCylindrifyParams CylParams;
    CylParams.Axis = GeometryOps::EMeshAxis::Z;
    CylParams.Factor = 0.6;
    GeometryOps::FCylindrifyOutcome Outcome;
    const GeometryOps::FOpResult CylOp = GeometryOps::Cylindrify(Other.Get(), CylParams, &Outcome);

    TestTrue(TEXT("cylindrify succeeds on a gapped mesh"), CylOp.bSuccess);
    TestEqual(TEXT("cylindrify keeps the live vertex count"), CylOp.VerticesAfter, VertexCount);
    // 84 live vertices, minus the two face centres on the axis, minus the loose vertex at
    // (0, 0, 30) whose perpendicular radius is also 0.
    TestEqual(TEXT("cylindrify skips exactly the three vertices on the axis"),
        Outcome.VerticesModified, 81);
    TestTrue(TEXT("cylindrify measures a positive average radius on a gapped mesh"),
        Outcome.AverageRadius > 0.0);

    return true;
}
