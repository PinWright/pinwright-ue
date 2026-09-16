// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the one health signal that can see a surface passing through ITSELF -
// a membrane spanning a solid's interior, and two walls pushed through each other.
//
// WHY A NEW SIGNAL AND NOT A NEW ASSERTION ON AN OLD ONE. The documented gate is
// `isClosed && signedVolume > 0`, and it is GREEN on both faults:
//
//   THE MEMBRANE (the case below). A `revolve` whose profile ends sit off the axis at the SAME
//   height caps both ends to the axis, so a disc spans the bore. The two fans are oppositely
//   wound, so their contributions to signedVolume CANCEL EXACTLY - the number that comes back is
//   the figure the intended ring would have had. boundaryEdges is 0, because the surface really
//   is closed. orientationConsistent is true. degenerateTriangles is 0. componentCount is 1.
//   Every published field is byte-identical to the ring that was wanted, and the ring has a lid.
//   The first test asserts that whole false green explicitly, and THEN the new field, so the
//   counterfactual is in the test rather than in a commit message.
//
//   THE CROSSING (the second test). Two walls of one shell pushed through each other. There is
//   no field at all for this: signedVolume degrades smoothly with no threshold anywhere on it.
//
// WHY THE CONTROLS MATTER AS MUCH AS THE CASES. A check that fires on correct geometry gets
// switched off wholesale, which is worse than no check. The third test holds four legitimate
// meshes at zero, and the LATHE among them is the one that would break a naive implementation: its
// axis cap is COPLANAR with, and edge-joined to, its own base annulus, so a coplanar-aware
// triangle test that did not exclude topologically connected pairs would report every lathe,
// cylinder, cone and sphere in the corpus.
#include "Misc/AutomationTest.h"

#include "Handlers/Geometry/GeometryOps_Primitives.h"
#include "Handlers/Geometry/GeometryUtils.h"
#include "Model/PwModelCompiler.h"
#include "Model/PwModelDiagnostic.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper
// with a common name would collide with a sibling test TU once Unity merges them.

constexpr double PwSelfIntersectTest_InnerRadius = 60.0;
constexpr double PwSelfIntersectTest_OuterRadius = 100.0;
constexpr double PwSelfIntersectTest_HalfHeight = 20.0;
constexpr int32 PwSelfIntersectTest_Steps = 12;

UDynamicMesh* PwSelfIntersectTest_NewMesh()
{
    return NewObject<UDynamicMesh>(GetTransientPackage());
}

// A rectangular ring section walked as a LOOP whose seam is left implicit - the two ends land at
// the same height without coinciding. `capped` then projects BOTH ends onto the axis
// (RevolveGenerator.cpp: the two projected points are appended to the profile curve and the curve
// is closed), so the bore is spanned by two coplanar, oppositely wound fans: one over
// radius 0..OuterRadius and one over radius 0..InnerRadius, both at -HalfHeight.
//
// The walk starts at the OUTER bottom corner rather than the inner one, which is not cosmetic: it
// is what makes the loop counter-clockwise in (radius, height), the engine's stated winding for a
// revolve profile, so the filled ring comes out with POSITIVE signed volume. Written the other way
// round the same shape is inside out, and would be caught by `signedVolume < 0` for a reason that
// has nothing to do with the membrane - which would make this test pass without measuring
// anything.
TArray<FVector2D> PwSelfIntersectTest_ImplicitSection()
{
    return { FVector2D(PwSelfIntersectTest_OuterRadius, -PwSelfIntersectTest_HalfHeight),
             FVector2D(PwSelfIntersectTest_OuterRadius,  PwSelfIntersectTest_HalfHeight),
             FVector2D(PwSelfIntersectTest_InnerRadius,  PwSelfIntersectTest_HalfHeight),
             FVector2D(PwSelfIntersectTest_InnerRadius, -PwSelfIntersectTest_HalfHeight) };
}

// The same section spelled so the op reads it as closed: the first point repeated as the last.
// This is the ring that was wanted, and it has no axis fan at all.
TArray<FVector2D> PwSelfIntersectTest_ClosedSection()
{
    TArray<FVector2D> Profile = {
        FVector2D(PwSelfIntersectTest_InnerRadius, -PwSelfIntersectTest_HalfHeight),
        FVector2D(PwSelfIntersectTest_OuterRadius, -PwSelfIntersectTest_HalfHeight),
        FVector2D(PwSelfIntersectTest_OuterRadius,  PwSelfIntersectTest_HalfHeight),
        FVector2D(PwSelfIntersectTest_InnerRadius,  PwSelfIntersectTest_HalfHeight) };
    // Copy before Add: passing Profile[0] directly is a reference INTO the array Add is about to
    // reallocate, and UE's container-aliasing check fires an appError that takes the suite host
    // down with it.
    const FVector2D FirstPoint = Profile[0];
    Profile.Add(FirstPoint);
    return Profile;
}

// An ordinary lathed silhouette: ends at DIFFERENT heights, so each axis cap is a real cap of a
// real solid. Its bottom cap fan and its bottom annulus are coplanar and share the inner ring's
// vertices, which is exactly the arrangement the check must NOT report.
TArray<FVector2D> PwSelfIntersectTest_LatheSection()
{
    return { FVector2D(PwSelfIntersectTest_InnerRadius, -PwSelfIntersectTest_HalfHeight),
             FVector2D(PwSelfIntersectTest_OuterRadius, -PwSelfIntersectTest_HalfHeight),
             FVector2D(PwSelfIntersectTest_OuterRadius,  PwSelfIntersectTest_HalfHeight),
             FVector2D(PwSelfIntersectTest_InnerRadius,  PwSelfIntersectTest_HalfHeight) };
}

UDynamicMesh* PwSelfIntersectTest_Revolve(const TArray<FVector2D>& Profile)
{
    UDynamicMesh* Mesh = PwSelfIntersectTest_NewMesh();
    GeometryOps::FRevolveParams Params;
    Params.Profile = Profile;
    Params.Angle = 360.0;
    Params.Steps = PwSelfIntersectTest_Steps;
    Params.bCapped = true;
    GeometryOps::GenerateRevolve(Mesh, Params, FTransform::Identity);
    return Mesh;
}

UDynamicMesh* PwSelfIntersectTest_NestedBoxes()
{
    UDynamicMesh* Mesh = PwSelfIntersectTest_NewMesh();
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
        Mesh, Options, FTransform::Identity, 200.0f, 200.0f, 200.0f, 0, 0, 0,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
        Mesh, Options, FTransform::Identity, 100.0f, 100.0f, 100.0f, 0, 0, 0,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);
    return Mesh;
}

// One edge-connected surface that crosses itself TRANSVERSALLY, built by hand so the crossing is
// arithmetic rather than an emergent property of a generator.
//
//   H is a flat two-quad strip in the plane z = 0, spanning x in [0, 40], y in [-10, 10].
//   V hinges off H's x = 0 edge, rises to z = +20 at x = 10, and descends to z = -10 at x = 30 -
//   so it passes DOWN THROUGH the plane z = 0 at x = 70/3, inside H's SECOND quad (x in [20, 40]).
//
// The crossing pairs are V's two far triangles against H's two far triangles; none of those four
// pairs shares a vertex, so none of them is excluded as topologically connected. The hinge
// triangles touch z = 0 only along x = 0, twenty units away from H's second quad, so they add
// nothing.
UDynamicMesh* PwSelfIntersectTest_CrossingSheets()
{
    UDynamicMesh* Mesh = PwSelfIntersectTest_NewMesh();
    Mesh->EditMesh([](UE::Geometry::FDynamicMesh3& EditMesh)
    {
        const int32 H0 = EditMesh.AppendVertex(FVector3d(0, -10, 0));
        const int32 H1 = EditMesh.AppendVertex(FVector3d(0, 10, 0));
        const int32 H2 = EditMesh.AppendVertex(FVector3d(20, -10, 0));
        const int32 H3 = EditMesh.AppendVertex(FVector3d(20, 10, 0));
        const int32 H4 = EditMesh.AppendVertex(FVector3d(40, -10, 0));
        const int32 H5 = EditMesh.AppendVertex(FVector3d(40, 10, 0));
        const int32 V0 = EditMesh.AppendVertex(FVector3d(10, -10, 20));
        const int32 V1 = EditMesh.AppendVertex(FVector3d(10, 10, 20));
        const int32 V2 = EditMesh.AppendVertex(FVector3d(30, -10, -10));
        const int32 V3 = EditMesh.AppendVertex(FVector3d(30, 10, -10));

        EditMesh.AppendTriangle(H0, H2, H3);
        EditMesh.AppendTriangle(H0, H3, H1);
        EditMesh.AppendTriangle(H2, H4, H5);
        EditMesh.AppendTriangle(H2, H5, H3);
        EditMesh.AppendTriangle(H0, H1, V1);
        EditMesh.AppendTriangle(H0, V1, V0);
        EditMesh.AppendTriangle(V0, V1, V3);
        EditMesh.AppendTriangle(V0, V3, V2);
    }, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
    return Mesh;
}

FString PwSelfIntersectTest_JoinDiagnostics(const TArray<FPwDiagnostic>& Diagnostics)
{
    if (Diagnostics.Num() == 0)
    {
        return TEXT("<none>");
    }
    TArray<FString> Lines;
    for (const FPwDiagnostic& Diagnostic : Diagnostics)
    {
        Lines.Add(Diagnostic.ToString());
    }
    return FString::Join(Lines, TEXT(" | "));
}

bool PwSelfIntersectTest_HasCode(const TArray<FPwDiagnostic>& Diagnostics, const TCHAR* Code)
{
    for (const FPwDiagnostic& Diagnostic : Diagnostics)
    {
        if (Diagnostic.Code == Code)
        {
            return true;
        }
    }
    return false;
}
}

// ============================================================================
// The membrane: every published field green, and one new field that is not.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelSelfIntersectionMembraneTest,
    "PinWright.Model.SelfIntersection.MembraneAcrossABoreIsCounted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelSelfIntersectionMembraneTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(
        PwSelfIntersectTest_Revolve(PwSelfIntersectTest_ImplicitSection()));

    // THE FALSE GREEN, asserted rather than described. Each of these was true before the new
    // field existed and is still true after it, on a ring whose bore is spanned by a disc. A test
    // written against them alone would have been green on both sides of this change.
    const GeometryUtils::FMeshHealth Health = GeometryUtils::MeasureMeshHealth(Mesh.Get());
    TestTrue(TEXT("the filled ring reports as CLOSED"), Health.IsClosed());
    TestEqual(TEXT("with no boundary edges"), Health.BoundaryEdges, 0);
    TestTrue(TEXT("and consistent orientation"), Health.IsOrientationConsistent());
    TestEqual(TEXT("and no degenerate triangles"), Health.DegenerateTriangles, 0);
    TestEqual(TEXT("and one connected component"), Health.ComponentCount, 1);
    TestTrue(*FString::Printf(
        TEXT("and the documented gate `isClosed && signedVolume > 0` PASSES on it (%g) - the two ")
        TEXT("axis fans are oppositely wound, so their volume contributions cancel exactly"),
        Health.SignedVolume),
        Health.IsClosed() && Health.SignedVolume > 0.0);

    // THE ASSERTION THE SIGNAL EXISTS FOR.
    const GeometryUtils::FMeshSelfIntersection Crossings =
        GeometryUtils::MeasureMeshSelfIntersection(Mesh.Get());
    TestTrue(TEXT("the mesh is small enough to be measured, so the answer is not 'declined'"),
        Crossings.bMeasured);
    TestTrue(*FString::Printf(
        TEXT("and the two coplanar, oppositely wound axis fans are counted as crossings (%d ")
        TEXT("pair(s)) - the only number in any response that moves"), Crossings.PairCount),
        Crossings.PairCount > 0);
    TestEqual(TEXT("all of them inside the one shell that carries the membrane"),
        Crossings.SelfIntersectingComponents, 1);
    TestFalse(TEXT("far below the recording cap, so the count is a total and not a floor"),
        Crossings.bTruncated);
    TestFalse(TEXT("and the verdict refuses the mesh"), Crossings.IsEmbedded());
    TestTrue(TEXT("with a coordinate for the crossing, which no render can show"),
        Crossings.bHasWitness);

    return true;
}

// ============================================================================
// The neighbouring fault: walls of one shell pushed through each other.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelSelfIntersectionCrossingWallsTest,
    "PinWright.Model.SelfIntersection.WallsCrossingInsideOneShellAreCounted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelSelfIntersectionCrossingWallsTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(PwSelfIntersectTest_CrossingSheets());

    // The fixture is worth pinning: if it ever stops being ONE component, the per-shell rule
    // would exclude the crossing and this test would pass for the wrong reason.
    const GeometryUtils::FMeshHealth Health = GeometryUtils::MeasureMeshHealth(Mesh.Get());
    TestEqual(TEXT("the two sheets and their hinge are one edge-connected surface"),
        Health.ComponentCount, 1);
    TestEqual(TEXT("built from the eight triangles the fixture writes"), Health.TriangleCount, 8);

    const GeometryUtils::FMeshSelfIntersection Crossings =
        GeometryUtils::MeasureMeshSelfIntersection(Mesh.Get());
    TestTrue(TEXT("the surface is measured"), Crossings.bMeasured);
    TestTrue(*FString::Printf(
        TEXT("and the wall passing through the floor is counted (%d pair(s)); nothing in ")
        TEXT("FMeshHealth moves for this at all"), Crossings.PairCount),
        Crossings.PairCount > 0);
    TestEqual(TEXT("in the single shell that crosses itself"),
        Crossings.SelfIntersectingComponents, 1);

    return true;
}

// ============================================================================
// The controls. Legitimate geometry stays at zero, or the check is worthless.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelSelfIntersectionControlsTest,
    "PinWright.Model.SelfIntersection.LegitimateGeometryStaysAtZero",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelSelfIntersectionControlsTest::RunTest(const FString& Parameters)
{
    // A hollow shell: two closed shells, one nested inside the other, appended and never welded.
    // This is how a void is carved by hand, it is two connected components, and a check counted
    // across components rather than inside each one would still hold it at zero - what would NOT
    // is a check that treated "more than one shell" as suspicious.
    {
        TStrongObjectPtr<UDynamicMesh> Nested(PwSelfIntersectTest_NestedBoxes());
        const GeometryUtils::FMeshHealth Health = GeometryUtils::MeasureMeshHealth(Nested.Get());
        TestEqual(TEXT("the hollow-shell fixture really is two shells"), Health.ComponentCount, 2);
        const GeometryUtils::FMeshSelfIntersection Crossings =
            GeometryUtils::MeasureMeshSelfIntersection(Nested.Get());
        TestTrue(TEXT("a hollow shell is measured"), Crossings.bMeasured);
        TestEqual(TEXT("and carries no crossings"), Crossings.PairCount, 0);
        TestTrue(TEXT("so its surface reads as embedded"), Crossings.IsEmbedded());
    }

    // The ring that was wanted: a closed section swept without any axis cap. Torus topology, one
    // shell, and the counter-case to the membrane above - the SAME section, spelled correctly.
    {
        TStrongObjectPtr<UDynamicMesh> Ring(
            PwSelfIntersectTest_Revolve(PwSelfIntersectTest_ClosedSection()));
        const GeometryUtils::FMeshSelfIntersection Crossings =
            GeometryUtils::MeasureMeshSelfIntersection(Ring.Get());
        TestEqual(TEXT("a ring with an open bore carries no crossings"), Crossings.PairCount, 0);
        TestEqual(TEXT("and no shell is at fault"), Crossings.SelfIntersectingComponents, 0);
    }

    // THE CONTROL THAT WOULD BREAK A NAIVE IMPLEMENTATION. A lathe IS capped to the axis, on
    // purpose, and each cap fan is coplanar with the annulus beside it. They meet along the
    // profile point's own ring, so every touching pair shares vertices - and excluding
    // topologically connected pairs is the whole reason this stays at zero. Without that
    // exclusion every lathe, cylinder, cone and sphere in the corpus reports crossings.
    {
        TStrongObjectPtr<UDynamicMesh> Lathe(
            PwSelfIntersectTest_Revolve(PwSelfIntersectTest_LatheSection()));
        const GeometryUtils::FMeshHealth Health = GeometryUtils::MeasureMeshHealth(Lathe.Get());
        TestTrue(TEXT("the lathe fixture is a closed solid"), Health.IsClosed());
        const GeometryUtils::FMeshSelfIntersection Crossings =
            GeometryUtils::MeasureMeshSelfIntersection(Lathe.Get());
        TestEqual(TEXT("and a coplanar axis cap joined to its own base is NOT a crossing"),
            Crossings.PairCount, 0);
    }

    // An open sheet is a legal .pwmodel output - a card, a plane, an append_buffers surface - and
    // the measurement is defined on it: crossing is a statement about a surface, not about a
    // solid, so it neither refuses an open mesh nor reports one.
    {
        TStrongObjectPtr<UDynamicMesh> Sheet(PwSelfIntersectTest_NewMesh());
        Sheet->EditMesh([](UE::Geometry::FDynamicMesh3& EditMesh)
        {
            EditMesh.AppendVertex(FVector3d(0, 0, 0));
            EditMesh.AppendVertex(FVector3d(100, 0, 0));
            EditMesh.AppendVertex(FVector3d(100, 100, 0));
            EditMesh.AppendVertex(FVector3d(0, 100, 0));
            EditMesh.AppendTriangle(0, 1, 2);
            EditMesh.AppendTriangle(0, 2, 3);
        }, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
        const GeometryUtils::FMeshSelfIntersection Crossings =
            GeometryUtils::MeasureMeshSelfIntersection(Sheet.Get());
        TestTrue(TEXT("an open sheet is measured rather than skipped"), Crossings.bMeasured);
        TestEqual(TEXT("and carries no crossings"), Crossings.PairCount, 0);
    }

    // Null-safe, and DECLINED rather than clean: an unmeasured mesh must never read as embedded,
    // which is the same distinction the -1 sentinels on the compile result carry.
    {
        const GeometryUtils::FMeshSelfIntersection None =
            GeometryUtils::MeasureMeshSelfIntersection(nullptr);
        TestFalse(TEXT("a null mesh is not measured"), None.bMeasured);
        TestFalse(TEXT("and does not read as embedded"), None.IsEmbedded());
    }

    return true;
}

// ============================================================================
// The same pair of documents through the .pwmodel front end, which is the
// surface the gate is documented on.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelSelfIntersectionDocumentTest,
    "PinWright.Model.SelfIntersection.ValidateReportsAndNamesTheMembrane",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelSelfIntersectionDocumentTest::RunTest(const FString& Parameters)
{
    FPwModelCompileOptions Options;
    Options.bValidateOnly = true;
    Options.bSave = false;

    const FPwModelCompileResult Filled = FPwModelCompiler::Compile(
        TEXT("pwmodel 0\n")
        TEXT("part ring {\n")
        TEXT("    revolve steps=12 profile=[(100, -20), (100, 20), (60, 20), (60, -20)]\n")
        TEXT("}\n"),
        Options);

    TestTrue(*FString::Printf(TEXT("the document validates - the warning does not fail it. %s"),
        *PwSelfIntersectTest_JoinDiagnostics(Filled.Diagnostics)), Filled.bSuccess);

    // The published gate, restated on the response: it passes, on a ring with a lid.
    TestEqual(TEXT("the merged mesh is closed"), Filled.MeshBoundaryEdges, 0);
    TestTrue(*FString::Printf(TEXT("and encloses positive volume (%g)"), Filled.MeshSignedVolume),
        Filled.MeshSignedVolume > 0.0);

    TestTrue(*FString::Printf(
        TEXT("and the response now carries the crossings that make the gate wrong (%d)"),
        Filled.MeshSelfIntersections),
        Filled.MeshSelfIntersections > 0);
    TestEqual(TEXT("in one shell"), Filled.MeshSelfIntersectingComponents, 1);
    TestTrue(*FString::Printf(TEXT("and the author is told, by code. %s"),
        *PwSelfIntersectTest_JoinDiagnostics(Filled.Diagnostics)),
        PwSelfIntersectTest_HasCode(Filled.Diagnostics,
            PwModelDiagnosticCodes::PWMODEL_SELF_INTERSECTING_SURFACE));

    // The same section spelled with its seam closed. Nothing about the gate changes; the new
    // field is what separates the two documents.
    const FPwModelCompileResult Ring = FPwModelCompiler::Compile(
        TEXT("pwmodel 0\n")
        TEXT("part ring {\n")
        TEXT("    revolve steps=12 profile=[(60, -20), (100, -20), (100, 20), (60, 20), (60, -20)]\n")
        TEXT("}\n"),
        Options);

    TestTrue(*FString::Printf(TEXT("the correctly spelled ring validates too. %s"),
        *PwSelfIntersectTest_JoinDiagnostics(Ring.Diagnostics)), Ring.bSuccess);
    TestEqual(TEXT("and reports zero crossings - measured, not omitted"),
        Ring.MeshSelfIntersections, 0);
    TestEqual(TEXT("with no shell at fault"), Ring.MeshSelfIntersectingComponents, 0);
    TestFalse(*FString::Printf(TEXT("and raises no self-intersection warning. %s"),
        *PwSelfIntersectTest_JoinDiagnostics(Ring.Diagnostics)),
        PwSelfIntersectTest_HasCode(Ring.Diagnostics,
            PwModelDiagnosticCodes::PWMODEL_SELF_INTERSECTING_SURFACE));

    return true;
}
