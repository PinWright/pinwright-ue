// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for the ONE number `sphere` does not measure: at `subdivisions=2` its
// half-extent is `radius / sqrt(3)`, not `radius`.
//
// AppendSphereBox's EdgeVertices counts VERTICES per cube edge, so the floor value 2 leaves only
// the 8 cube corners in the mesh, and FBoxSphereGenerator projects a corner to sqrt(1/3) per axis
// before scaling by Radius. Every axis therefore lands 42% short of the number the author wrote.
// Nothing lies - the compile response's `bounds` reports the real, small box - but `radius=` is
// the only number in the source, and geometry placed against it lands OUTSIDE the mesh. That is
// how a caudal fin root ended up 0.566 uu clear of the peduncle it was meant to be embedded in,
// on a compile that was green with no warning naming the size.
//
// The remedy is published text rather than a warning: the floor is the cheap 12-triangle
// primitive callers ask for deliberately, so warning at it would fire on every correct use. This
// test is what keeps the text honest. It MEASURES the two extents first and asserts the doc
// quotes the ratio it measured, so the doc cannot drift from the geometry in either direction -
// a reworded description is free, a dropped or wrong factor fails here.
//
// Why here and not in Tests/Geometry: the geometry side is already covered - the op builds the
// corners-only cube whatever the text says - and no test in that tree reads the op table, so the
// factor could vanish from the published description with the whole geometry suite green.
#include "Misc/AutomationTest.h"

#include "Handlers/Geometry/GeometryOps_Primitives.h"
#include "Model/PwModelParser.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper
// with a common name would collide with a sibling test TU once Unity merges them.

// A round number, so the ratio the assertions derive is the factor and nothing else.
constexpr double SphereExtentDocsTest_Radius = 100.0;

// The largest half-extent over the three axes of a freshly generated sphere, or -1.0 when the op
// failed. The box-sphere is symmetric about the origin at every subdivision count, so one number
// describes the whole extent; taking the MAX is what makes "reaches radius" answerable - a mean
// would hide an axis that falls short.
double SphereExtentDocsTest_MaxHalfExtent(FAutomationTestBase& Test, int32 Subdivisions)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(NewObject<UDynamicMesh>(GetTransientPackage()));

    GeometryOps::FSphereParams Params;
    Params.Radius = SphereExtentDocsTest_Radius;
    Params.Subdivisions = Subdivisions;

    const GeometryOps::FOpResult Op =
        GeometryOps::GenerateSphere(Mesh.Get(), Params, FTransform::Identity);
    if (!Op.bSuccess)
    {
        Test.AddError(FString::Printf(
            TEXT("sphere subdivisions=%d failed to build"), Subdivisions));
        return -1.0;
    }

    const UE::Geometry::FAxisAlignedBox3d Bounds = Mesh->GetMeshRef().GetBounds();
    const FVector3d Half = Bounds.Diagonal() * 0.5;
    return FMath::Max3(Half.X, Half.Y, Half.Z);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelSphereFloorExtentDocTest,
    "PinWright.Model.Parser.SphereSubdivisionsDocPublishesTheFloorHalfExtent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelSphereFloorExtentDocTest::RunTest(const FString& Parameters)
{
    // ---- the geometry, measured ------------------------------------------------------------

    const double FloorHalfExtent = SphereExtentDocsTest_MaxHalfExtent(*this, 2);
    const double NextHalfExtent = SphereExtentDocsTest_MaxHalfExtent(*this, 3);
    if (FloorHalfExtent < 0.0 || NextHalfExtent < 0.0)
    {
        return false;
    }

    const double Expected = SphereExtentDocsTest_Radius / FMath::Sqrt(3.0);
    TestTrue(FString::Printf(TEXT(
            "subdivisions=2 measures radius/sqrt(3) = %.4f, not radius: measured %.4f"),
            Expected, FloorHalfExtent),
        FMath::IsNearlyEqual(FloorHalfExtent, Expected, 0.001));

    // The trap is CONFINED TO THE FLOOR, which is the other half of what the doc claims. If this
    // ever failed, the published "from 3 up the bounding box matches radius" would be the wrong
    // remedy and a warning at every subdivision count would be the right one.
    TestTrue(FString::Printf(TEXT(
            "subdivisions=3 reaches the full radius %.4f: measured %.4f"),
            SphereExtentDocsTest_Radius, NextHalfExtent),
        FMath::IsNearlyEqual(NextHalfExtent, SphereExtentDocsTest_Radius, 0.001));

    // ---- the text, against that measurement ------------------------------------------------

    const FPwModelOpSpec* Op = PwModelOpTable::Find(FString(TEXT("sphere")), EPwModelOpContext::Part);
    if (!TestNotNull(TEXT("'sphere' is a .pwmodel part op"), Op))
    {
        return false;
    }

    const FPwModelParamSpec* Param = Op->FindParam(FString(TEXT("subdivisions")));
    if (!TestNotNull(TEXT("'sphere' publishes a 'subdivisions' parameter"), Param))
    {
        return false;
    }

    const FString& Doc = Param->Description;

    // The ratio is FORMATTED FROM THE MEASUREMENT, never typed in: a doc quoting a factor the
    // geometry does not produce fails here exactly as a doc that quotes none.
    const FString MeasuredFactor = FString::Printf(TEXT("%.4f"), FloorHalfExtent / SphereExtentDocsTest_Radius);
    TestTrue(FString::Printf(TEXT(
            "the 'subdivisions' text quotes the measured factor %s. Text: %s"),
            *MeasuredFactor, *Doc),
        Doc.Contains(MeasuredFactor));

    // And says what that factor is a factor OF, in the closed form an author can apply to any
    // radius. The number alone reads as one more count in a paragraph full of them.
    TestTrue(FString::Printf(TEXT("and names it as radius/sqrt(3). Text: %s"), *Doc),
        Doc.Contains(TEXT("radius/sqrt(3)")));
    TestTrue(FString::Printf(TEXT("and calls it the half-extent. Text: %s"), *Doc),
        Doc.Contains(TEXT("half-extent")));

    // The floor is still published beside it - the factor is only actionable once the reader
    // knows which subdivision count carries it. This is also the one clause
    // Tests/Geometry/TestGeometryClampDomains.cpp's named sphere exception reads.
    TestTrue(FString::Printf(TEXT("and still publishes the floor. Text: %s"), *Doc),
        Doc.Contains(TEXT("Minimum 2")));

    return true;
}
