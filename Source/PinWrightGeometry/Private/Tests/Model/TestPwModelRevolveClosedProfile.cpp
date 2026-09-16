// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for `revolve` on a CLOSED SECTION profile - a ring, tube, rim or flange,
// written by repeating the first profile point as the last.
//
// THESE ASSERT ON GEOMETRY, DELIBERATELY, BECAUSE NO HEALTH FIELD MOVES ACROSS THE FIX.
// Before it, a full revolution of such a profile came back with a flat membrane spanning the
// bore - two triangle fans from the repeated endpoint to the revolve axis - and every published
// signal read clean: isClosed true, boundaryEdges 0, orientationConsistent true, degenerates 0,
// and signedVolume EXACTLY the correct annulus figure, because the two fans are coincident and
// oppositely wound so their volume contributions cancel. `model.compile`'s own documented gate
// is `isClosed && signedVolume > 0`, and it passed on the filled solid. A test written against
// those fields would have been green before the fix and green after, and would measure nothing.
//
// Two things do move, and both are asserted below:
//
//   1. NO VERTEX REACHES THE AXIS. The membrane's only distinguishing vertices are the two fan
//      apexes at radius 0. A ring's section never comes near the axis, so "the minimum radius
//      over every vertex is the section's inner radius" is the direct statement that the bore
//      is open - the ray-down-the-axis check, in the form the mesh can answer exactly.
//   2. THE TRIANGLE COUNT. A closed N-point section swept in S steps is exactly 2*N*S
//      triangles. Each axis fan adds S more, so the pre-fix count was 2*N*S + 2*S. That excess
//      is the only numeric tell the response ever carried, and nobody computes it by hand.
//
// The counter-case matters as much as the case: an OPEN profile - a lathed silhouette - is
// still capped to the axis and still comes out a solid. That is what `capped` is for, and the
// fix must not turn every lathe into a shell.
#include "Misc/AutomationTest.h"

#include "Handlers/Geometry/GeometryOps_Primitives.h"
#include "Model/PwModelCompiler.h"
#include "Model/PwModelDiagnostic.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper
// with a common name would collide with a sibling test TU once Unity merges them.

// A rectangular section standing off the axis - the simplest synthetic ring. Counter-clockwise
// in (radius, height), which is the engine's stated winding for a revolve profile ("+X is
// towards the outside of the revolve donut, +Y is up, polygon counter-clockwise or it will be
// inside-out"), so the solid comes out with positive volume.
constexpr double RevolveClosedTest_InnerRadius = 60.0;
constexpr double RevolveClosedTest_OuterRadius = 100.0;
constexpr double RevolveClosedTest_HalfHeight = 20.0;
constexpr int32 RevolveClosedTest_Steps = 12;

// 4 distinct points, so 4 section edges, so 4 * 12 * 2 = 96 triangles. With the two axis fans
// the same input measured 96 + 2 * 12 = 120.
constexpr int32 RevolveClosedTest_ExpectedTriangles = 96;
constexpr int32 RevolveClosedTest_AxisFanTriangles = 2 * RevolveClosedTest_Steps;

TArray<FVector2D> RevolveClosedTest_OpenSection()
{
    return { FVector2D(RevolveClosedTest_InnerRadius, -RevolveClosedTest_HalfHeight),
             FVector2D(RevolveClosedTest_OuterRadius, -RevolveClosedTest_HalfHeight),
             FVector2D(RevolveClosedTest_OuterRadius,  RevolveClosedTest_HalfHeight),
             FVector2D(RevolveClosedTest_InnerRadius,  RevolveClosedTest_HalfHeight) };
}

// The same section spelled as a closed loop: the first point repeated as the last.
TArray<FVector2D> RevolveClosedTest_ClosedSection()
{
    TArray<FVector2D> Profile = RevolveClosedTest_OpenSection();
    // Copy before Add: passing Profile[0] directly is a reference INTO the array Add is about to
    // reallocate, and UE's container-aliasing check fires an appError that takes the suite host
    // down with it (Array.h: "element which already comes from the container being modified").
    const FVector2D FirstPoint = Profile[0];
    Profile.Add(FirstPoint);
    return Profile;
}

// The smallest distance from the revolve axis any vertex reaches. On an open bore this is the
// section's inner radius; a membrane spanning the bore puts a vertex at 0.
double RevolveClosedTest_MinRadius(const UE::Geometry::FDynamicMesh3& Mesh)
{
    double MinRadius = TNumericLimits<double>::Max();
    for (const int32 VertexID : Mesh.VertexIndicesItr())
    {
        const FVector3d P = Mesh.GetVertex(VertexID);
        MinRadius = FMath::Min(MinRadius, FMath::Sqrt(P.X * P.X + P.Y * P.Y));
    }
    return MinRadius;
}

bool RevolveClosedTest_AnyWarningContains(const TArray<FString>& Warnings, const TCHAR* Needle)
{
    for (const FString& Warning : Warnings)
    {
        if (Warning.Contains(Needle))
        {
            return true;
        }
    }
    return false;
}

FString RevolveClosedTest_Join(const TArray<FString>& Warnings)
{
    return Warnings.Num() == 0 ? TEXT("<none>") : FString::Join(Warnings, TEXT(" | "));
}

bool RevolveClosedTest_AnyDiagnosticContains(const TArray<FPwDiagnostic>& Diagnostics,
                                             const TCHAR* Needle)
{
    for (const FPwDiagnostic& Diagnostic : Diagnostics)
    {
        if (Diagnostic.Message.Contains(Needle))
        {
            return true;
        }
    }
    return false;
}

FString RevolveClosedTest_JoinDiagnostics(const TArray<FPwDiagnostic>& Diagnostics)
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

// The code the compiler raises when a profile is walked the wrong way round. Spelled as a
// LITERAL rather than through PwModelDiagnosticCodes so this file still compiles - and
// therefore FAILS rather than refusing to build - against a tree that does not have the code
// yet. It is also what the wire carries, which is the contract under test.
const TCHAR* const RevolveClosedTest_ProfileReversed = TEXT("PWMODEL_REVOLVE_PROFILE_REVERSED");

const FPwDiagnostic* RevolveClosedTest_FindByCode(const TArray<FPwDiagnostic>& Diagnostics,
                                                  const TCHAR* Code)
{
    for (const FPwDiagnostic& Diagnostic : Diagnostics)
    {
        if (Diagnostic.Code == Code)
        {
            return &Diagnostic;
        }
    }
    return nullptr;
}

FPwModelCompileResult RevolveClosedTest_Validate(const TCHAR* Source)
{
    FPwModelCompileOptions Options;
    Options.bValidateOnly = true;
    Options.bSave = false;
    return FPwModelCompiler::Compile(Source, Options);
}

// The same closed ring section as everything above, spelled the two ways round. Nothing else
// differs: same four corners, same repeated endpoint, same `steps`, same line 3.
//
// (60, -20) -> (100, -20) -> (100, 20) -> (60, 20) is counter-clockwise in the profile's
// (x = radius, y = height) plane - out along the bottom, UP THE OUTER face, in along the top,
// back down the inner - which is the direction that leaves the enclosed material on the left
// and sweeps a solid with positive volume.
const TCHAR* const RevolveClosedTest_CounterClockwiseDoc =
    TEXT("pwmodel 0\n")
    TEXT("part ring {\n")
    TEXT("    revolve steps=12 profile=[(60, -20), (100, -20), (100, 20), (60, 20), (60, -20)]\n")
    TEXT("}\n");

const TCHAR* const RevolveClosedTest_ClockwiseDoc =
    TEXT("pwmodel 0\n")
    TEXT("part ring {\n")
    TEXT("    revolve steps=12 profile=[(60, -20), (60, 20), (100, 20), (100, -20), (60, -20)]\n")
    TEXT("}\n");
}

// ============================================================================
// The op, driven directly: the bore stays open and the mesh stays a solid.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelRevolveClosedProfileBoreOpenTest,
    "PinWright.Model.Revolve.ClosedProfileLeavesTheBoreOpen",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelRevolveClosedProfileBoreOpenTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(NewObject<UDynamicMesh>(GetTransientPackage()));

    GeometryOps::FRevolveParams Params;
    Params.Profile = RevolveClosedTest_ClosedSection();
    Params.Angle = 360.0;
    Params.Steps = RevolveClosedTest_Steps;
    // Left at the default the trap depends on. The whole defect was that an author reads
    // `capped` as "the ends of a partial revolution" and therefore never touches it.
    Params.bCapped = true;

    const GeometryOps::FOpResult Op =
        GeometryOps::GenerateRevolve(Mesh.Get(), Params, FTransform::Identity);

    TestTrue(TEXT("a closed-section revolve succeeds"), Op.bSuccess);

    // THE ASSERTION THE FIX EXISTS FOR. Pre-fix this was 0: the two fan apexes sat on the axis.
    const UE::Geometry::FDynamicMesh3& Ref = Mesh->GetMeshRef();
    const double MinRadius = RevolveClosedTest_MinRadius(Ref);
    TestTrue(*FString::Printf(
        TEXT("no vertex reaches the revolve axis, so a ray down the axis passes through the bore; ")
        TEXT("nearest vertex sits at radius %.4f, the section's inner radius is %.1f"),
        MinRadius, RevolveClosedTest_InnerRadius),
        MinRadius > RevolveClosedTest_InnerRadius - 1e-3);

    // The same fact counted rather than measured: no axis disc, so no fan excess.
    TestEqual(*FString::Printf(
        TEXT("the sweep is 4 section edges x %d steps x 2 and carries no axis fan (%d would be ")
        TEXT("the filled-bore count)"),
        RevolveClosedTest_Steps,
        RevolveClosedTest_ExpectedTriangles + RevolveClosedTest_AxisFanTriangles),
        Ref.TriangleCount(), RevolveClosedTest_ExpectedTriangles);

    // Green BEFORE the fix as well - asserted so the fix cannot be "fixed" into an open shell.
    TestTrue(TEXT("and the ring is still a closed solid"), Ref.IsClosed());

    // Honesty on the two things the op changed about the request. `profilePoints` is echoed off
    // this array by geometry.revolve, so the deduplication has to be visible in it.
    TestEqual(TEXT("the repeated endpoint is dropped from the profile the caller reads back"),
        Params.Profile.Num(), 4);
    TestTrue(*FString::Printf(TEXT("and the op says it read the profile as a closed section. warnings: %s"),
        *RevolveClosedTest_Join(Op.Warnings)),
        RevolveClosedTest_AnyWarningContains(Op.Warnings, TEXT("closed section")));

    return true;
}

// ============================================================================
// The counter-case: an OPEN profile is still capped to the axis.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelRevolveOpenProfileCapsToAxisTest,
    "PinWright.Model.Revolve.OpenProfileStillCapsToTheAxis",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelRevolveOpenProfileCapsToAxisTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(NewObject<UDynamicMesh>(GetTransientPackage()));

    GeometryOps::FRevolveParams Params;
    Params.Profile = RevolveClosedTest_OpenSection();   // no repeated endpoint
    Params.Angle = 360.0;
    Params.Steps = RevolveClosedTest_Steps;
    Params.bCapped = true;

    const GeometryOps::FOpResult Op =
        GeometryOps::GenerateRevolve(Mesh.Get(), Params, FTransform::Identity);

    TestTrue(TEXT("an open-profile revolve succeeds"), Op.bSuccess);

    // A lathed silhouette IS meant to be capped to the axis - that is what makes it a solid
    // rather than a shell. The closed-section branch must not reach this input.
    const UE::Geometry::FDynamicMesh3& Ref = Mesh->GetMeshRef();
    TestTrue(TEXT("an open profile still gets its axis caps"),
        RevolveClosedTest_MinRadius(Ref) < 1e-3);
    TestTrue(TEXT("and the lathed solid is closed"), Ref.IsClosed());

    TestEqual(TEXT("nothing is dropped from an open profile"), Params.Profile.Num(), 4);
    TestFalse(*FString::Printf(TEXT("and it is not reported as a closed section. warnings: %s"),
        *RevolveClosedTest_Join(Op.Warnings)),
        RevolveClosedTest_AnyWarningContains(Op.Warnings, TEXT("closed section")));

    return true;
}

// ============================================================================
// The section written WITHOUT the repeat: named, not repaired.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelRevolveImplicitSectionIsNamedTest,
    "PinWright.Model.Revolve.ImplicitClosedSectionIsNamedNotRepaired",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelRevolveImplicitSectionIsNamedTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(NewObject<UDynamicMesh>(GetTransientPackage()));

    GeometryOps::FRevolveParams Params;
    // The same rectangular section, walked so that its two ends land at the SAME height without
    // coinciding - the spelling of a closed loop that leaves its seam implicit. The op cannot
    // tell it from a lathed solid whose ends happen to align, so it still caps to the axis and
    // still fills the bore; what it must not do is stay silent about it, because no health field
    // and no triangle count can tell the author.
    Params.Profile = {
        FVector2D(RevolveClosedTest_InnerRadius, -RevolveClosedTest_HalfHeight),
        FVector2D(RevolveClosedTest_InnerRadius,  RevolveClosedTest_HalfHeight),
        FVector2D(RevolveClosedTest_OuterRadius,  RevolveClosedTest_HalfHeight),
        FVector2D(RevolveClosedTest_OuterRadius, -RevolveClosedTest_HalfHeight) };
    Params.Angle = 360.0;
    Params.Steps = RevolveClosedTest_Steps;
    Params.bCapped = true;

    const GeometryOps::FOpResult Op =
        GeometryOps::GenerateRevolve(Mesh.Get(), Params, FTransform::Identity);

    TestTrue(TEXT("the revolve still succeeds"), Op.bSuccess);
    TestTrue(*FString::Printf(
        TEXT("and the coincident axis caps are named, with the one-character remedy. warnings: %s"),
        *RevolveClosedTest_Join(Op.Warnings)),
        RevolveClosedTest_AnyWarningContains(Op.Warnings, TEXT("repeat its first point")));

    return true;
}

// ============================================================================
// The same profile through the .pwmodel front end, which is where it was found.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelRevolveClosedProfileDocumentTest,
    "PinWright.Model.Revolve.ClosedProfileDocumentHasNoAxisFan",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelRevolveClosedProfileDocumentTest::RunTest(const FString& Parameters)
{
    FPwModelCompileOptions Options;
    Options.bValidateOnly = true;
    Options.bSave = false;

    const FPwModelCompileResult Result = FPwModelCompiler::Compile(
        TEXT("pwmodel 0\n")
        TEXT("part ring {\n")
        TEXT("    revolve steps=12 profile=[(60, -20), (100, -20), (100, 20), (60, 20), (60, -20)]\n")
        TEXT("}\n"),
        Options);

    TestTrue(*FString::Printf(TEXT("the document validates. %s"),
        *RevolveClosedTest_JoinDiagnostics(Result.Diagnostics)), Result.bSuccess);

    // The count is the whole assertion. Every health field below was already correct on the
    // filled-bore mesh, so this is the only number in the response that moved.
    TestEqual(*FString::Printf(
        TEXT("the merged mesh carries no axis fan (%d would be the filled-bore count)"),
        RevolveClosedTest_ExpectedTriangles + RevolveClosedTest_AxisFanTriangles),
        Result.MeshTriangleCount, RevolveClosedTest_ExpectedTriangles);

    // Recorded, not celebrated: these three are exactly the fields the published gate reads,
    // and they passed on the broken mesh too. They are asserted only so a future change cannot
    // trade the membrane for an open shell and call that a fix.
    TestEqual(TEXT("the ring is closed"), Result.MeshBoundaryEdges, 0);
    TestTrue(*FString::Printf(TEXT("and encloses positive volume (%g)"), Result.MeshSignedVolume),
        Result.MeshSignedVolume > 0.0);
    TestEqual(TEXT("with no degenerate triangles"), Result.MeshDegenerateTriangles, 0);

    // The author's only warning that their profile was reinterpreted.
    TestTrue(*FString::Printf(TEXT("and the document is told the section was read as closed. %s"),
        *RevolveClosedTest_JoinDiagnostics(Result.Diagnostics)),
        RevolveClosedTest_AnyDiagnosticContains(Result.Diagnostics, TEXT("closed section")));

    return true;
}

// ============================================================================
// Winding: the same section walked the other way is a silently inside-out solid.
// ============================================================================
//
// A SECOND defect on the same op, found on the same shape. `revolve` never stated which way a
// profile has to be traversed, and the wrong direction sweeps a solid whose every triangle
// faces inwards. Backface culling then shows whichever wall faces the camera, so the two
// render identically from every angle and no capture-based check can find it; offline
// consumers read winding rather than shading, so the mesh distance field inverts and
// Lumen / DFAO light the part as though the camera were inside it.
//
// The first assertion below is the PREMISE, and it is why the diagnostic has to exist at all:
// every published count is IDENTICAL between the two spellings. Only signedVolume moves.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelRevolveReversedProfileIsNamedTest,
    "PinWright.Model.Revolve.ReversedProfileIsNamedAtItsLine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelRevolveReversedProfileIsNamedTest::RunTest(const FString& Parameters)
{
    const FPwModelCompileResult Correct =
        RevolveClosedTest_Validate(RevolveClosedTest_CounterClockwiseDoc);
    const FPwModelCompileResult Reversed =
        RevolveClosedTest_Validate(RevolveClosedTest_ClockwiseDoc);

    if (!TestTrue(*FString::Printf(TEXT("the counter-clockwise document validates. %s"),
            *RevolveClosedTest_JoinDiagnostics(Correct.Diagnostics)), Correct.bSuccess)
        || !TestTrue(*FString::Printf(TEXT("the clockwise document validates too - it is legal ")
                TEXT("geometry, not a parse error. %s"),
            *RevolveClosedTest_JoinDiagnostics(Reversed.Diagnostics)), Reversed.bSuccess))
    {
        return false;
    }

    // THE PREMISE. Each of these is a field an author might reasonably gate on, and not one of
    // them separates a correct ring from an inside-out one. If any of them starts differing,
    // the reasoning behind this diagnostic needs revisiting, so they are asserted rather than
    // assumed.
    TestEqual(TEXT("the triangle count cannot separate the two windings"),
        Reversed.MeshTriangleCount, Correct.MeshTriangleCount);
    TestEqual(TEXT("nor can the vertex count"),
        Reversed.MeshVertexCount, Correct.MeshVertexCount);
    TestEqual(TEXT("nor boundaryEdges - both are closed solids"),
        Reversed.MeshBoundaryEdges, Correct.MeshBoundaryEdges);
    TestEqual(TEXT("nor degenerateTriangles"),
        Reversed.MeshDegenerateTriangles, Correct.MeshDegenerateTriangles);
    TestEqual(TEXT("nor nonManifoldVertices"),
        Reversed.MeshNonManifoldVertices, Correct.MeshNonManifoldVertices);
    TestEqual(TEXT("nor componentCount"),
        Reversed.MeshComponentCount, Correct.MeshComponentCount);
    TestEqual(TEXT("nor inconsistentEdges - a UNIFORM inversion is perfectly consistent"),
        Reversed.MeshInconsistentEdges, Correct.MeshInconsistentEdges);

    // The one field that does move, and the sign convention the whole diagnostic rests on.
    TestTrue(*FString::Printf(
        TEXT("the counter-clockwise section encloses POSITIVE volume (%g)"),
        Correct.MeshSignedVolume), Correct.MeshSignedVolume > 0.0);
    TestTrue(*FString::Printf(
        TEXT("and reversing the same four points makes it NEGATIVE (%g)"),
        Reversed.MeshSignedVolume), Reversed.MeshSignedVolume < 0.0);

    // THE ASSERTION THE FIX EXISTS FOR.
    const FPwDiagnostic* Raised =
        RevolveClosedTest_FindByCode(Reversed.Diagnostics, RevolveClosedTest_ProfileReversed);
    if (!TestNotNull(*FString::Printf(
            TEXT("the reversed profile is named by '%s' rather than left to a compile cycle. %s"),
            RevolveClosedTest_ProfileReversed,
            *RevolveClosedTest_JoinDiagnostics(Reversed.Diagnostics)), Raised))
    {
        return false;
    }

    // Anchored on the `revolve` line, not on the part or the merge stage: the point of raising
    // it at the call site is that the author is told which op to edit.
    TestEqual(TEXT("and it is anchored on the revolve's own source line"), Raised->Line, 3);

    // The remedy has to travel with the message - the code alone does not say which way to walk.
    TestTrue(*FString::Printf(TEXT("and the message names the direction to walk. message: %s"),
        *Raised->Message),
        Raised->Message.Contains(TEXT("COUNTER-CLOCKWISE"), ESearchCase::CaseSensitive));

    // The other half, and the half that keeps the code worth reading: correct work is silent.
    TestNull(*FString::Printf(
        TEXT("the counter-clockwise section is NOT warned about. %s"),
        *RevolveClosedTest_JoinDiagnostics(Correct.Diagnostics)),
        RevolveClosedTest_FindByCode(Correct.Diagnostics, RevolveClosedTest_ProfileReversed));

    return true;
}
