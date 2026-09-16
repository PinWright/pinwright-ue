// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for the extracted advanced ops (GeometryOps_Advanced.h).
//
// These run against a transient UDynamicMesh with no actor, no editor world, no spline
// component and no FHandlerContext. Three of the six verbs took their inputs from LEVEL ACTORS
// before the extraction, so this file existing at all is the evidence that the actor -> data
// adapter is gone: profiles arrive as locations plus bounding-box extents, spline paths as a
// TArray<FTransform>. If a later change reintroduces an actor lookup into one of these ops,
// this file stops compiling rather than failing at runtime.
//
// What each test guards, in the order a regression would appear:
//
//  1. append_buffers' INVALID_PARAMS message text. Those messages name array indices and
//     expected lengths, and callers match on them; they moved from inline SendError calls into
//     GeometryOps validators, which is exactly the kind of move that silently rewords a string.
//  2. The branch a verb takes on data that resolved to nothing. loft with profiles REQUESTED
//     but none resolved appends nothing - it must not fall through to the bounding-box
//     extrusion, which would silently substitute a different shape for the one asked for.
//  3. The status strings. sweep's and bridge's only report of which of their several internal
//     branches ran is a sentence in the response; nothing else distinguishes a spline sweep
//     from the linear fallback, or a bridge from a hole-fill.
//  4. Twist/scale living in ONE place. sweep applies them to both the supplied path samples and
//     its own fallback frames; the wrapper hands over raw location+rotation samples so the two
//     paths cannot drift.
//  5. The null-mesh contract. All five op families answer one authoring mistake with one code;
//     these ops reach it through GeometryOps::BeginOp now rather than a per-op literal, and the
//     RPC front-ends cannot produce the case at all (both resolvers reject a null mesh first),
//     so this file is the only place the pair is observed.
//  6. That a clamp reports itself. This family rewrites edgeGroupA, subdivisions and steps into
//     range and echoes the value the CALLER passed, so the FOpResult warning is the only record
//     that the number moved. Each case also asserts the in-range call stays warning-free, since
//     a clamp helper that warns unconditionally is the other way this regresses.
//  7. edge_split's hand-rolled replacement loop. It rebuilds triangles itself rather than
//     calling FDynamicMesh3::SplitEdge, so whether the triangle count grows depends on the
//     fixture's topology, not on the verb. Both outcomes are pinned so that replacing the loop
//     has to declare itself here.
#include "Misc/AutomationTest.h"

#include "Handlers/Geometry/GeometryOps_Advanced.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicMeshOverlay.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include "GeometryScript/MeshPrimitiveFunctions.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace
// helper with a common name would collide with a sibling test TU when Unity merges them.

UDynamicMesh* GeometryOpsAdvancedTest_NewMesh()
{
    return NewObject<UDynamicMesh>(GetTransientPackage());
}

// One 100x100 quad in the XY plane at OriginX/OriginY, as 4 vertices and 2 triangles. Built
// through AppendBuffers so the fixtures need nothing from a sibling op family. A quad has
// exactly one boundary loop, which is what the bridge cases count on.
GeometryOps::FOpResult GeometryOpsAdvancedTest_AppendQuad(UDynamicMesh* Mesh, double OriginX, double OriginY)
{
    GeometryOps::FAppendBuffersParams Params;
    Params.Vertices = {
        FVector(OriginX,         OriginY,         0.0),
        FVector(OriginX + 100.0, OriginY,         0.0),
        FVector(OriginX + 100.0, OriginY + 100.0, 0.0),
        FVector(OriginX,         OriginY + 100.0, 0.0) };
    Params.Triangles = { FIntVector(0, 1, 2), FIntVector(0, 2, 3) };

    GeometryOps::FAppendBuffersOutputs Outputs;
    return GeometryOps::AppendBuffers(Mesh, MoveTemp(Params), Outputs);
}

// A closed tetrahedron: 4 vertices, 4 outward-wound faces, ZERO boundary loops.
GeometryOps::FAppendBuffersParams GeometryOpsAdvancedTest_TetraParams()
{
    GeometryOps::FAppendBuffersParams Params;
    Params.Vertices = {
        FVector(0.0, 0.0, 0.0),
        FVector(100.0, 0.0, 0.0),
        FVector(50.0, 100.0, 0.0),
        FVector(50.0, 50.0, 100.0) };
    Params.Triangles = {
        FIntVector(0, 1, 2), FIntVector(0, 3, 1),
        FIntVector(0, 2, 3), FIntVector(1, 3, 2) };
    return Params;
}

// Uniform location+rotation samples along a straight vertical line, standing in for what the
// handler pulls off a USplineComponent.
TArray<FTransform> GeometryOpsAdvancedTest_StraightPath(int32 StepCount, double Height)
{
    TArray<FTransform> Samples;
    for (int32 i = 0; i <= StepCount; ++i)
    {
        const double T = (double)i / StepCount;
        Samples.Add(FTransform(FQuat::Identity, FVector(0.0, 0.0, Height * T)));
    }
    return Samples;
}

// The one code/message pair every family answers a null mesh with, spelled out here rather than
// read back off the op so a change to GeometryOps::NullMeshFailure cannot make the test agree
// with itself. Unreachable from the RPC front-ends - both resolvers reject a null mesh before
// the op is called - so nothing but this file and the .pwmodel compiler ever sees it.
void GeometryOpsAdvancedTest_ExpectNullMeshFailure(FAutomationTestBase& Test,
    const TCHAR* Label, const GeometryOps::FOpResult& Result)
{
    Test.TestFalse(FString::Printf(TEXT("%s rejects a null mesh"), Label), Result.bSuccess);
    Test.TestEqual(FString::Printf(TEXT("%s -> MESH_NOT_FOUND"), Label),
        Result.ErrorCode, FString(TEXT("MESH_NOT_FOUND")));
    Test.TestEqual(FString::Printf(TEXT("%s null-mesh message"), Label),
        Result.ErrorMessage, FString(TEXT("DynamicMesh not available")));
}

// Asserts the op recorded exactly this one warning, matched in full. A count-only check would
// pass on a reworded sentence, and the sentence IS the record: every clamped parameter here is
// echoed back to the caller at the value it passed in, not at the value the op used.
void GeometryOpsAdvancedTest_ExpectOnlyWarning(FAutomationTestBase& Test,
    const TCHAR* Label, const GeometryOps::FOpResult& Result, const TCHAR* Expected)
{
    Test.TestEqual(FString::Printf(TEXT("%s records exactly one warning"), Label),
        Result.Warnings.Num(), 1);
    if (Result.Warnings.Num() == 1)
    {
        Test.TestEqual(FString::Printf(TEXT("%s names the parameter, both values and the range"), Label),
            Result.Warnings[0], FString(Expected));
    }
}

// "No warning opens with this word", for the cases that assert a parameter was NOT rewritten.
// Not a warning COUNT: the two sweep verbs also publish path diagnostics, so a count here would
// couple an assertion about `steps` to whether the fixture path happens to be a sound one.
bool GeometryOpsAdvancedTest_NoWarningStartingWith(
    const GeometryOps::FOpResult& Result, const TCHAR* Prefix)
{
    for (const FString& Warning : Result.Warnings)
    {
        if (Warning.StartsWith(Prefix))
        {
            return false;
        }
    }
    return true;
}
}

// ============================================================================
// append_buffers: the two data rules, with their exact wire messages.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsAdvancedAppendBuffersRejectionsTest,
    "PinWright.Geometry.Ops.Advanced.AppendBuffersRejectionsKeepTheirWireMessages",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsAdvancedAppendBuffersRejectionsTest::RunTest(const FString& Parameters)
{
    // Index 4 against 4 vertices - the exact case the dispatcher test drives.
    {
        GeometryOps::FAppendBuffersParams Params = GeometryOpsAdvancedTest_TetraParams();
        Params.Triangles = { FIntVector(0, 1, 4) };

        const GeometryOps::FOpResult Result = GeometryOps::ValidateAppendBuffers(Params);
        TestFalse(TEXT("out-of-range triangle index is rejected"), Result.bSuccess);
        TestEqual(TEXT("out-of-range index -> INVALID_PARAMS"), Result.ErrorCode, FString(TEXT("INVALID_PARAMS")));
        TestEqual(TEXT("out-of-range message names index, offender and range"), Result.ErrorMessage,
            FString(TEXT("triangle 0 references vertex index 4 out of range [0, 4)")));
    }

    // A present-but-short optional attribute array.
    {
        GeometryOps::FAppendBuffersParams Params = GeometryOpsAdvancedTest_TetraParams();
        Params.Normals = { FVector::UpVector, FVector::UpVector, FVector::UpVector };

        const GeometryOps::FOpResult Result = GeometryOps::ValidateAppendBuffers(Params);
        TestFalse(TEXT("short normals array is rejected"), Result.bSuccess);
        TestEqual(TEXT("count mismatch -> INVALID_PARAMS"), Result.ErrorCode, FString(TEXT("INVALID_PARAMS")));
        TestEqual(TEXT("count-mismatch message names both counts"), Result.ErrorMessage,
            FString(TEXT("normals count (3) must match vertices count (4)")));
    }

    // An absent optional array is legal, and so is an empty triangle list.
    {
        GeometryOps::FAppendBuffersParams Params = GeometryOpsAdvancedTest_TetraParams();
        Params.Triangles.Empty();
        TestTrue(TEXT("loose vertices with no triangles validate"),
            GeometryOps::ValidateAppendBuffers(Params).bSuccess);
    }

    // No vertices at all.
    {
        const GeometryOps::FOpResult Result = GeometryOps::ValidateAppendBuffers(GeometryOps::FAppendBuffersParams());
        TestFalse(TEXT("empty vertex list is rejected"), Result.bSuccess);
        TestEqual(TEXT("empty vertex list -> INVALID_PARAMS"), Result.ErrorCode,
            FString(TEXT("INVALID_PARAMS")));
        TestEqual(TEXT("empty vertex list message"), Result.ErrorMessage,
            FString(TEXT("vertices must be a non-empty array of [x,y,z] or {x,y,z}")));

        // BulkEditHandler rejects an absent or empty `vertices` array before it has parsed a
        // single element, so it reaches this rule through the standalone form. Both must answer
        // identically - the handler used to carry its own copy of the sentence, and two copies
        // of one wire message is how the two spellings start to drift.
        const GeometryOps::FOpResult Direct = GeometryOps::ValidateAppendBufferVertexCount(0);
        TestFalse(TEXT("the standalone vertex-count rule rejects zero"), Direct.bSuccess);
        TestEqual(TEXT("standalone rule matches the whole-payload rule's code"),
            Direct.ErrorCode, Result.ErrorCode);
        TestEqual(TEXT("standalone rule matches the whole-payload rule's message"),
            Direct.ErrorMessage, Result.ErrorMessage);
        TestTrue(TEXT("a non-empty vertex count passes the standalone rule"),
            GeometryOps::ValidateAppendBufferVertexCount(1).bSuccess);
    }

    return true;
}

// ============================================================================
// append_buffers: the counts the wrapper echoes come out of FOpResult.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsAdvancedAppendBuffersCountsTest,
    "PinWright.Geometry.Ops.Advanced.AppendBuffersReportsAppendedAndTotalCounts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsAdvancedAppendBuffersCountsTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsAdvancedTest_NewMesh());

    GeometryOps::FAppendBuffersOutputs Outputs;
    const GeometryOps::FOpResult Result =
        GeometryOps::AppendBuffers(Mesh.Get(), GeometryOpsAdvancedTest_TetraParams(), Outputs);

    TestTrue(TEXT("tetrahedron appends"), Result.bSuccess);
    TestEqual(TEXT("trianglesBefore is 0 on an empty mesh"), Result.TrianglesBefore, 0);
    TestEqual(TEXT("trianglesAfter is 4"), Result.TrianglesAfter, 4);
    TestEqual(TEXT("appendedVertices is 4"), Outputs.AppendedVertices, 4);
    TestEqual(TEXT("appendedTriangles is 4"), Outputs.AppendedTriangles, 4);
    TestTrue(TEXT("an append that added geometry reports bChanged"), Result.bChanged);

    // A second append onto a non-empty mesh: the appended counts stay a delta.
    GeometryOps::FAppendBuffersOutputs SecondOutputs;
    const GeometryOps::FOpResult Second =
        GeometryOps::AppendBuffers(Mesh.Get(), GeometryOpsAdvancedTest_TetraParams(), SecondOutputs);
    TestEqual(TEXT("second append starts from 4 triangles"), Second.TrianglesBefore, 4);
    TestEqual(TEXT("second append is still a delta, not a total"), SecondOutputs.AppendedTriangles, 4);

    return true;
}

// ============================================================================
// sweep: path frames arrive as DATA, and the fallback is chosen by their absence.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsAdvancedSweepPathTest,
    "PinWright.Geometry.Ops.Advanced.SweepChoosesItsPathFromSuppliedSamples",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsAdvancedSweepPathTest::RunTest(const FString& Parameters)
{
    GeometryOps::FSweepParams Params;
    Params.Steps = 12;

    // No samples -> the linear fallback, reported as such.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsAdvancedTest_NewMesh());
        GeometryOpsAdvancedTest_AppendQuad(Mesh.Get(), 0.0, 0.0);
        const int32 TrisBefore = Mesh->GetTriangleCount();

        GeometryOps::FSweepOutputs Outputs;
        const GeometryOps::FOpResult Result =
            GeometryOps::Sweep(Mesh.Get(), Params, GeometryOps::FSweepPath(), Outputs);

        TestTrue(TEXT("sweep with no path succeeds"), Result.bSuccess);
        TestTrue(TEXT("the fallback path is reported, not a spline sweep"),
            Outputs.Status.StartsWith(TEXT("Linear sweep with 12 steps")));
        TestEqual(TEXT("fallback step count is the clamped Steps"), Outputs.PathSteps, 12);
        TestTrue(TEXT("the fallback actually swept geometry"), Result.TrianglesAfter > TrisBefore);
        TestTrue(TEXT("a sweep that added geometry reports bChanged"), Result.bChanged);
    }

    // Samples supplied -> the spline branch, step count derived from the samples.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsAdvancedTest_NewMesh());
        GeometryOpsAdvancedTest_AppendQuad(Mesh.Get(), 0.0, 0.0);
        const int32 TrisBefore = Mesh->GetTriangleCount();

        GeometryOps::FSweepPath Path;
        Path.Samples = GeometryOpsAdvancedTest_StraightPath(4, 400.0);
        Path.SplineLength = 400.0f;

        GeometryOps::FSweepOutputs Outputs;
        const GeometryOps::FOpResult Result = GeometryOps::Sweep(Mesh.Get(), Params, Path, Outputs);

        TestTrue(TEXT("sweep over supplied samples succeeds"), Result.bSuccess);
        TestEqual(TEXT("step count comes from the samples, not from Steps"), Outputs.PathSteps, 4);
        TestEqual(TEXT("the spline branch is reported with its length"), Outputs.Status,
            FString(TEXT("Swept along spline with 4 steps, length 400.0")));
        TestTrue(TEXT("the supplied path actually swept geometry"), Result.TrianglesAfter > TrisBefore);

        // WHERE it swept, not just that it swept. Status and PathSteps are both derived from
        // Path.Samples.Num(); drop Path.Samples[i].GetLocation() out of the frame build and both
        // still read exactly as they do here while the geometry collapses onto the fixture. The
        // samples run Z 0..400, so every frame origin is on the swept surface and the result
        // must reach at least Z=400 - against the roughly Z=100 the linear fallback produces on
        // this same 100x100 quad.
        TestTrue(TEXT("the swept surface follows the samples to the end of the path"),
            Mesh->GetMeshRef().GetBounds().Max.Z > 300.0);
    }

    // ScaleStart/ScaleEnd are applied by the op onto the frames it hands the engine, and
    // nothing else in this file passes a non-default value for either - so deleting the Lerp
    // and the FVector(Scale) on the frame is invisible. The same path swept with a 3x end scale
    // has to enclose strictly more space than one swept at unit scale.
    {
        GeometryOps::FSweepPath Path;
        Path.Samples = GeometryOpsAdvancedTest_StraightPath(4, 400.0);
        Path.SplineLength = 400.0f;

        TStrongObjectPtr<UDynamicMesh> UnitMesh(GeometryOpsAdvancedTest_NewMesh());
        GeometryOpsAdvancedTest_AppendQuad(UnitMesh.Get(), 0.0, 0.0);
        GeometryOps::FSweepOutputs UnitOutputs;
        const GeometryOps::FOpResult UnitResult =
            GeometryOps::Sweep(UnitMesh.Get(), Params, Path, UnitOutputs);
        TestTrue(TEXT("the unit-scale sweep succeeds"), UnitResult.bSuccess);

        TStrongObjectPtr<UDynamicMesh> GrownMesh(GeometryOpsAdvancedTest_NewMesh());
        GeometryOpsAdvancedTest_AppendQuad(GrownMesh.Get(), 0.0, 0.0);
        GeometryOps::FSweepParams Grown = Params;
        Grown.ScaleStart = 1.0;
        Grown.ScaleEnd = 3.0;
        GeometryOps::FSweepOutputs GrownOutputs;
        const GeometryOps::FOpResult GrownResult =
            GeometryOps::Sweep(GrownMesh.Get(), Grown, Path, GrownOutputs);
        TestTrue(TEXT("the scaled sweep succeeds"), GrownResult.bSuccess);

        // Same path, same profile, same step count: only the per-frame scale differs, so the
        // triangle counts match and only the enclosed volume can report it.
        TestEqual(TEXT("scaling the sweep does not change its triangulation"),
            GrownResult.TrianglesAfter, UnitResult.TrianglesAfter);
        TestTrue(TEXT("scaleEnd widens the swept profile along the path"),
            GrownMesh->GetMeshRef().GetBounds().Volume()
                > UnitMesh->GetMeshRef().GetBounds().Volume() * 1.5);
    }

    // A named actor that carries no spline: fallback geometry, spline-less wording.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsAdvancedTest_NewMesh());
        GeometryOpsAdvancedTest_AppendQuad(Mesh.Get(), 0.0, 0.0);

        GeometryOps::FSweepPath Path;
        Path.bActorHasNoSplineComponent = true;

        GeometryOps::FSweepOutputs Outputs;
        GeometryOps::Sweep(Mesh.Get(), Params, Path, Outputs);

        TestEqual(TEXT("the spline-less actor keeps its own wording"), Outputs.Status,
            FString(TEXT("Spline actor found but no USplineComponent - using linear sweep")));
    }

    return true;
}

// ============================================================================
// loft: profiles arrive as data, and "requested but unresolved" is its own branch.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsAdvancedLoftProfilesTest,
    "PinWright.Geometry.Ops.Advanced.LoftSeparatesUnresolvedProfilesFromNoProfiles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsAdvancedLoftProfilesTest::RunTest(const FString& Parameters)
{
    // Profiles REQUESTED, none resolved: nothing is appended. Falling through to the
    // bounding-box extrusion here would silently build a shape nobody asked for.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsAdvancedTest_NewMesh());
        GeometryOpsAdvancedTest_AppendQuad(Mesh.Get(), 0.0, 0.0);

        GeometryOps::FLoftParams Params;
        Params.bUseProfiles = true;

        GeometryOps::FLoftOutputs Outputs;
        const GeometryOps::FOpResult Result =
            GeometryOps::Loft(Mesh.Get(), Params, TArray<GeometryOps::FLoftProfileSample>(), Outputs);

        TestTrue(TEXT("an unresolved-profile loft still succeeds"), Result.bSuccess);
        TestEqual(TEXT("no profiles were used"), Outputs.ProfilesUsed, 0);
        TestEqual(TEXT("nothing was appended"), Result.TrianglesAfter, Result.TrianglesBefore);
        TestFalse(TEXT("an op that altered nothing reports bChanged false"), Result.bChanged);
    }

    // No profiles requested: the bounding-box extrusion branch.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsAdvancedTest_NewMesh());
        GeometryOpsAdvancedTest_AppendQuad(Mesh.Get(), 0.0, 0.0);

        GeometryOps::FLoftParams Params;
        Params.bUseProfiles = false;

        GeometryOps::FLoftOutputs Outputs;
        const GeometryOps::FOpResult Result =
            GeometryOps::Loft(Mesh.Get(), Params, TArray<GeometryOps::FLoftProfileSample>(), Outputs);

        TestTrue(TEXT("the no-profile loft succeeds"), Result.bSuccess);
        TestTrue(TEXT("the no-profile loft appended geometry"), Result.TrianglesAfter > Result.TrianglesBefore);
    }

    // Two resolved profiles, supplied purely as data.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsAdvancedTest_NewMesh());
        GeometryOpsAdvancedTest_AppendQuad(Mesh.Get(), 0.0, 0.0);

        TArray<GeometryOps::FLoftProfileSample> Profiles;
        GeometryOps::FLoftProfileSample First;
        First.Location = FVector::ZeroVector;
        First.Extent = FVector(40.0, 40.0, 40.0);
        First.bHasMesh = true;
        GeometryOps::FLoftProfileSample Last;
        Last.Location = FVector(0.0, 0.0, 200.0);
        Last.Extent = FVector(40.0, 40.0, 40.0);
        Last.bHasMesh = true;
        Profiles.Add(First);
        Profiles.Add(Last);

        GeometryOps::FLoftParams Params;
        Params.bUseProfiles = true;

        GeometryOps::FLoftOutputs Outputs;
        const GeometryOps::FOpResult Result = GeometryOps::Loft(Mesh.Get(), Params, Profiles, Outputs);

        TestTrue(TEXT("the profile loft succeeds"), Result.bSuccess);
        TestEqual(TEXT("both profiles counted"), Outputs.ProfilesUsed, 2);
        TestTrue(TEXT("the profile loft appended geometry"), Result.TrianglesAfter > Result.TrianglesBefore);

        // The profile branch must loft along the PROFILE LOCATIONS, not along the mesh's own
        // bounding box: ProfilesUsed is set from Profiles.Num() and the triangle count grows on
        // either branch, so both assertions above hold with the two locations dropped. The
        // profiles run Z 0..200 while the bounding-box branch sweeps this flat quad over its
        // own default 100 uu of height centred on Z=0, which cannot reach past Z=100.
        TestTrue(TEXT("the loft path runs between the two profile locations"),
            Mesh->GetMeshRef().GetBounds().Max.Z > 150.0);
    }

    // Two profiles at the SAME location: zero path length, nothing swept.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsAdvancedTest_NewMesh());
        GeometryOpsAdvancedTest_AppendQuad(Mesh.Get(), 0.0, 0.0);

        GeometryOps::FLoftProfileSample Sample;
        Sample.Extent = FVector(40.0, 40.0, 40.0);
        Sample.bHasMesh = true;
        const TArray<GeometryOps::FLoftProfileSample> Profiles = { Sample, Sample };

        GeometryOps::FLoftParams Params;
        Params.bUseProfiles = true;
        Params.bSmooth = false;

        GeometryOps::FLoftOutputs Outputs;
        const GeometryOps::FOpResult Result = GeometryOps::Loft(Mesh.Get(), Params, Profiles, Outputs);

        TestEqual(TEXT("a zero-length loft path sweeps nothing"), Result.TrianglesAfter, Result.TrianglesBefore);
        TestEqual(TEXT("and reports no profiles used"), Outputs.ProfilesUsed, 0);
    }

    return true;
}

// ============================================================================
// extrude_along_spline: path frames arrive as data.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsAdvancedExtrudeAlongSplineTest,
    "PinWright.Geometry.Ops.Advanced.ExtrudeAlongSplineConsumesPathFramesAsData",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsAdvancedExtrudeAlongSplineTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsAdvancedTest_NewMesh());
    GeometryOpsAdvancedTest_AppendQuad(Mesh.Get(), 0.0, 0.0);

    GeometryOps::FExtrudeAlongSplineParams Params;
    Params.Segments = 16;

    const GeometryOps::FOpResult Result = GeometryOps::ExtrudeAlongSpline(
        Mesh.Get(), Params, GeometryOpsAdvancedTest_StraightPath(
            GeometryOps::SplinePathStepCount(Params.Segments), 500.0));

    TestTrue(TEXT("extrude over supplied frames succeeds"), Result.bSuccess);
    TestTrue(TEXT("extrude appended geometry"), Result.TrianglesAfter > Result.TrianglesBefore);
    TestTrue(TEXT("extrude reports bChanged"), Result.bChanged);

    // "Appended geometry" is true of an extrude that ignored PathSamples entirely and swept the
    // profile over frames of its own - this op has no fallback branch and no status string, so a
    // count is the only other thing it publishes. The supplied path runs Z 0..500 and every
    // frame origin sits on the swept surface, so the result has to reach the far end of it.
    TestTrue(TEXT("the extrusion follows the supplied frames to the end of the path"),
        Mesh->GetMeshRef().GetBounds().Max.Z > 400.0);

    return true;
}

// ============================================================================
// edge_split: stale ids are skipped, live ones split.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsAdvancedEdgeSplitTest,
    "PinWright.Geometry.Ops.Advanced.EdgeSplitSkipsStaleIdsAndSplitsLiveOnes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsAdvancedEdgeSplitTest::RunTest(const FString& Parameters)
{
    // Ids that are not live edges are skipped, not rejected.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsAdvancedTest_NewMesh());
        GeometryOpsAdvancedTest_AppendQuad(Mesh.Get(), 0.0, 0.0);

        GeometryOps::FEdgeSplitParams Params;
        Params.EdgeIndices = { 9999, 10000 };
        Params.bWeldVertices = false;

        GeometryOps::FEdgeSplitOutputs Outputs;
        const GeometryOps::FOpResult Result = GeometryOps::EdgeSplit(Mesh.Get(), Params, Outputs);

        TestTrue(TEXT("stale edge ids are a successful no-op"), Result.bSuccess);
        TestEqual(TEXT("nothing was split"), Outputs.EdgesSplit, 0);
        TestFalse(TEXT("a no-op edge split reports bChanged false"), Result.bChanged);
    }

    // A live edge splits and grows the triangle count - on a fixture that lets both
    // replacement triangles land.
    //
    // The verb does NOT call FDynamicMesh3::SplitEdge. It removes the triangle bordering the
    // edge and appends two replacements through a new midpoint vertex, so a replacement only
    // lands when every vertex it names is still alive AFTER the removal. RemoveTriangle
    // defaults to bRemoveIsolatedVertices=true, which makes that condition "each corner of the
    // removed triangle is also used by some other triangle". The closed tetra satisfies it -
    // every vertex sits in three faces - so nothing is dropped and the count grows.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsAdvancedTest_NewMesh());
        GeometryOps::FAppendBuffersOutputs Ignored;
        GeometryOps::AppendBuffers(Mesh.Get(), GeometryOpsAdvancedTest_TetraParams(), Ignored);

        // Edge 0 is (v0,v1), created by the first appended face. It is interior, so it borders
        // two triangles and the loop replaces both.
        GeometryOps::FEdgeSplitParams Params;
        Params.EdgeIndices = { 0 };
        Params.bWeldVertices = false;

        GeometryOps::FEdgeSplitOutputs Outputs;
        const GeometryOps::FOpResult Result = GeometryOps::EdgeSplit(Mesh.Get(), Params, Outputs);

        TestTrue(TEXT("splitting a live edge succeeds"), Result.bSuccess);
        TestEqual(TEXT("an interior edge replaces both of its triangles"), Outputs.EdgesSplit, 2);
        TestTrue(TEXT("the split grew the triangle count"), Result.TrianglesAfter > Result.TrianglesBefore);
        TestTrue(TEXT("the midpoint vertex was added"), Result.VerticesAfter > Result.VerticesBefore);
        TestTrue(TEXT("a split that grew the mesh reports bChanged true"), Result.bChanged);
    }

    // The same call on a LONE QUAD replaces its one triangle and still comes back with the
    // counts it started with. Quad edge 0 is (v0,v1) and borders only triangle (0,1,2);
    // removing that triangle leaves v1 in no triangle at all, so RemoveTriangle deletes it and
    // the second replacement - the one that keeps v1 - is rejected for naming a dead vertex.
    // One replacement lands, the corner is lost, and the midpoint vertex takes its place
    // one-for-one.
    //
    // Pinned, not endorsed: trianglesBefore/trianglesAfter are the only numbers
    // geometry.edge_split reports, so a caller sees this edit as a no-op. Anything that makes
    // the verb split properly here - FDynamicMesh3::SplitEdge, or passing
    // bRemoveIsolatedVertices=false - breaks this block, which is the point of it.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsAdvancedTest_NewMesh());
        GeometryOpsAdvancedTest_AppendQuad(Mesh.Get(), 0.0, 0.0);

        GeometryOps::FEdgeSplitParams Params;
        Params.EdgeIndices = { 0 };
        Params.bWeldVertices = false;

        GeometryOps::FEdgeSplitOutputs Outputs;
        const GeometryOps::FOpResult Result = GeometryOps::EdgeSplit(Mesh.Get(), Params, Outputs);

        TestTrue(TEXT("splitting a boundary edge succeeds"), Result.bSuccess);
        TestEqual(TEXT("a boundary edge replaces its one triangle"), Outputs.EdgesSplit, 1);
        TestEqual(TEXT("the rejected replacement leaves the triangle count where it started"),
            Result.TrianglesAfter, Result.TrianglesBefore);
        TestEqual(TEXT("the midpoint vertex replaces the isolated corner one-for-one"),
            Result.VerticesAfter, Result.VerticesBefore);
        TestFalse(TEXT("so the op's own counters cannot see the edit"), Result.bChanged);
    }

    return true;
}

// ============================================================================
// bridge: which of its two branches ran is reported only in the status string.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsAdvancedBridgeTest,
    "PinWright.Geometry.Ops.Advanced.BridgeReportsStitchOrHoleFillInItsStatus",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsAdvancedBridgeTest::RunTest(const FString& Parameters)
{
    // A closed tetrahedron has no boundary loops: the verb fills holes instead and says so.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsAdvancedTest_NewMesh());
        GeometryOps::FAppendBuffersOutputs Ignored;
        GeometryOps::AppendBuffers(Mesh.Get(), GeometryOpsAdvancedTest_TetraParams(), Ignored);

        GeometryOps::FBridgeOutputs Outputs;
        const GeometryOps::FOpResult Result =
            GeometryOps::Bridge(Mesh.Get(), GeometryOps::FBridgeParams(), Outputs);

        TestTrue(TEXT("bridge on a closed mesh still succeeds"), Result.bSuccess);
        TestTrue(TEXT("the hole-fill fallback is named in the status"),
            Outputs.Status.Contains(TEXT("Filling holes instead.")));
        TestEqual(TEXT("nothing was bridged"), Outputs.TrianglesCreated, 0);
    }

    // Two disjoint quads carry one boundary loop each, so the strip builder runs.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsAdvancedTest_NewMesh());
        GeometryOpsAdvancedTest_AppendQuad(Mesh.Get(), 0.0, 0.0);
        GeometryOpsAdvancedTest_AppendQuad(Mesh.Get(), 500.0, 0.0);

        GeometryOps::FBridgeOutputs Outputs;
        const GeometryOps::FOpResult Result =
            GeometryOps::Bridge(Mesh.Get(), GeometryOps::FBridgeParams(), Outputs);

        TestTrue(TEXT("bridging two loops succeeds"), Result.bSuccess);
        TestTrue(TEXT("the stitch branch is named in the status"),
            Outputs.Status.StartsWith(TEXT("Bridged loop ")));
        TestTrue(TEXT("triangles were created"), Outputs.TrianglesCreated > 0);
        TestTrue(TEXT("the mesh grew by the created triangles"),
            Result.TrianglesAfter == Result.TrianglesBefore + Outputs.TrianglesCreated);
        TestEqual(TEXT("in-range loop indices warn about nothing"), Result.Warnings.Num(), 0);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsAdvancedBridgeHoleFillUVLayerTest,
    "PinWright.Geometry.Ops.Advanced.BridgeHoleFillCreatesMissingUVLayer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsAdvancedBridgeHoleFillUVLayerTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsAdvancedTest_NewMesh());
    const GeometryOps::FOpResult Append =
        GeometryOpsAdvancedTest_AppendQuad(Mesh.Get(), 0.0, 0.0);
    TestTrue(TEXT("the one-loop fixture was created"), Append.bSuccess);
    TestTrue(TEXT("the one-loop fixture keeps its attribute set"),
        Mesh->GetMeshRef().HasAttributes());
    TestEqual(TEXT("append_buffers without uvs leaves zero UV layers"),
        Mesh->GetMeshRef().Attributes()->NumUVLayers(), 0);

    GeometryOps::FBridgeOutputs Outputs;
    const GeometryOps::FOpResult Result =
        GeometryOps::Bridge(Mesh.Get(), GeometryOps::FBridgeParams(), Outputs);

    TestTrue(TEXT("bridge's one-loop hole-fill fallback succeeds without a UV layer"),
        Result.bSuccess);
    TestTrue(TEXT("the one-loop fallback is named in the status"),
        Outputs.Status.Contains(TEXT("Filling holes instead.")));
    TestEqual(TEXT("the fallback creates only the UV0 layer the engine requires"),
        Mesh->GetMeshRef().Attributes()->NumUVLayers(), 1);
    TestNotNull(TEXT("the filled triangles have a real UV overlay to project into"),
        Mesh->GetMeshRef().Attributes()->PrimaryUV());
    TestTrue(TEXT("the fallback filled the quad's boundary"),
        Result.TrianglesAfter > Result.TrianglesBefore);

    return true;
}

// ============================================================================
// The null-mesh contract: one code and one sentence, for all six entry points.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsAdvancedNullMeshTest,
    "PinWright.Geometry.Ops.Advanced.EveryOpAnswersANullMeshWithTheSharedFailure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsAdvancedNullMeshTest::RunTest(const FString& Parameters)
{
    {
        GeometryOps::FBridgeOutputs Outputs;
        GeometryOpsAdvancedTest_ExpectNullMeshFailure(*this, TEXT("bridge"),
            GeometryOps::Bridge(nullptr, GeometryOps::FBridgeParams(), Outputs));
    }
    {
        GeometryOps::FEdgeSplitOutputs Outputs;
        GeometryOpsAdvancedTest_ExpectNullMeshFailure(*this, TEXT("edge_split"),
            GeometryOps::EdgeSplit(nullptr, GeometryOps::FEdgeSplitParams(), Outputs));
    }
    {
        GeometryOps::FLoftOutputs Outputs;
        GeometryOpsAdvancedTest_ExpectNullMeshFailure(*this, TEXT("loft"),
            GeometryOps::Loft(nullptr, GeometryOps::FLoftParams(),
                TArray<GeometryOps::FLoftProfileSample>(), Outputs));
    }
    {
        GeometryOps::FSweepOutputs Outputs;
        GeometryOpsAdvancedTest_ExpectNullMeshFailure(*this, TEXT("sweep"),
            GeometryOps::Sweep(nullptr, GeometryOps::FSweepParams(),
                GeometryOps::FSweepPath(), Outputs));
    }
    GeometryOpsAdvancedTest_ExpectNullMeshFailure(*this, TEXT("extrude_along_spline"),
        GeometryOps::ExtrudeAlongSpline(nullptr, GeometryOps::FExtrudeAlongSplineParams(),
            GeometryOpsAdvancedTest_StraightPath(4, 400.0)));

    // append_buffers guards the mesh BEFORE it validates the buffers, so a call that is wrong
    // on both counts still reports the mesh. That order is the shipped one and the wrappers
    // rely on nothing else, but a migration that moved the null guard behind the validator
    // would silently swap which fault a caller sees.
    {
        GeometryOps::FAppendBuffersOutputs Outputs;
        GeometryOpsAdvancedTest_ExpectNullMeshFailure(*this, TEXT("append_buffers"),
            GeometryOps::AppendBuffers(nullptr, GeometryOpsAdvancedTest_TetraParams(), Outputs));
    }
    {
        GeometryOps::FAppendBuffersOutputs Outputs;
        GeometryOpsAdvancedTest_ExpectNullMeshFailure(*this, TEXT("append_buffers with bad buffers"),
            GeometryOps::AppendBuffers(nullptr, GeometryOps::FAppendBuffersParams(), Outputs));
    }

    return true;
}

// ============================================================================
// The four clamps that used to be silent. Each one rewrites a caller value the
// response still echoes unclamped, so the warning is the whole record of it.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsAdvancedClampWarningsTest,
    "PinWright.Geometry.Ops.Advanced.ClampedCallerValuesReportThemselves",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsAdvancedClampWarningsTest::RunTest(const FString& Parameters)
{
    // bridge: the loop count is discovered inside the op, so an out-of-range edgeGroupA is
    // knowable nowhere else. Two disjoint quads give two loops, i.e. a valid range of 0-1.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsAdvancedTest_NewMesh());
        GeometryOpsAdvancedTest_AppendQuad(Mesh.Get(), 0.0, 0.0);
        GeometryOpsAdvancedTest_AppendQuad(Mesh.Get(), 500.0, 0.0);

        GeometryOps::FBridgeParams Params;
        Params.EdgeGroupA = 7;

        GeometryOps::FBridgeOutputs Outputs;
        const GeometryOps::FOpResult Result = GeometryOps::Bridge(Mesh.Get(), Params, Outputs);

        TestTrue(TEXT("an out-of-range edgeGroupA still succeeds"), Result.bSuccess);
        GeometryOpsAdvancedTest_ExpectOnlyWarning(*this, TEXT("bridge edgeGroupA"), Result,
            TEXT("edgeGroupA clamped from 7 to 1 (valid range 0-1)"));
        TestTrue(TEXT("and the bridge still ran on the clamped loop"),
            Outputs.Status.StartsWith(TEXT("Bridged loop 1 ")));
    }

    // loft, bounding-box branch: subdivisions is capped at 32 here.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsAdvancedTest_NewMesh());
        GeometryOpsAdvancedTest_AppendQuad(Mesh.Get(), 0.0, 0.0);

        GeometryOps::FLoftParams Params;
        Params.bUseProfiles = false;
        Params.Subdivisions = 100;

        GeometryOps::FLoftOutputs Outputs;
        const GeometryOps::FOpResult Result =
            GeometryOps::Loft(Mesh.Get(), Params, TArray<GeometryOps::FLoftProfileSample>(), Outputs);

        TestTrue(TEXT("an over-large subdivisions still lofts"), Result.bSuccess);
        GeometryOpsAdvancedTest_ExpectOnlyWarning(*this, TEXT("loft bounding-box subdivisions"),
            Result, TEXT("subdivisions clamped from 100 to 32 (valid range 2-32)"));
    }

    // loft, profile branch: the SAME parameter, a different ceiling. Which branch ran is not
    // derivable from the response, so the two clamps must be distinguishable in the warning.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsAdvancedTest_NewMesh());
        GeometryOpsAdvancedTest_AppendQuad(Mesh.Get(), 0.0, 0.0);

        GeometryOps::FLoftProfileSample First;
        First.Location = FVector::ZeroVector;
        First.Extent = FVector(40.0, 40.0, 40.0);
        First.bHasMesh = true;
        GeometryOps::FLoftProfileSample Last;
        Last.Location = FVector(0.0, 0.0, 200.0);
        Last.Extent = FVector(40.0, 40.0, 40.0);
        Last.bHasMesh = true;
        const TArray<GeometryOps::FLoftProfileSample> Profiles = { First, Last };

        GeometryOps::FLoftParams Params;
        Params.bUseProfiles = true;
        Params.Subdivisions = 100;

        GeometryOps::FLoftOutputs Outputs;
        const GeometryOps::FOpResult Result = GeometryOps::Loft(Mesh.Get(), Params, Profiles, Outputs);

        TestTrue(TEXT("an over-large subdivisions still lofts over profiles"), Result.bSuccess);
        GeometryOpsAdvancedTest_ExpectOnlyWarning(*this, TEXT("loft profile subdivisions"),
            Result, TEXT("subdivisions clamped from 100 to 64 (valid range 2-64)"));
    }

    // A subdivisions inside both ranges warns about nothing.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsAdvancedTest_NewMesh());
        GeometryOpsAdvancedTest_AppendQuad(Mesh.Get(), 0.0, 0.0);

        GeometryOps::FLoftParams Params;
        Params.bUseProfiles = false;
        Params.Subdivisions = 8;

        GeometryOps::FLoftOutputs Outputs;
        const GeometryOps::FOpResult Result =
            GeometryOps::Loft(Mesh.Get(), Params, TArray<GeometryOps::FLoftProfileSample>(), Outputs);

        TestEqual(TEXT("an in-range subdivisions warns about nothing"), Result.Warnings.Num(), 0);
    }

    // sweep: steps is rewritten only on the linear fallback, which is the only branch that
    // consumes it - pathSteps comes off the sample count when a path was supplied.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsAdvancedTest_NewMesh());
        GeometryOpsAdvancedTest_AppendQuad(Mesh.Get(), 0.0, 0.0);

        GeometryOps::FSweepParams Params;
        Params.Steps = 500;

        GeometryOps::FSweepOutputs Outputs;
        const GeometryOps::FOpResult Result =
            GeometryOps::Sweep(Mesh.Get(), Params, GeometryOps::FSweepPath(), Outputs);

        TestTrue(TEXT("an over-large steps still sweeps"), Result.bSuccess);
        GeometryOpsAdvancedTest_ExpectOnlyWarning(*this, TEXT("sweep steps"), Result,
            TEXT("steps clamped from 500 to 256 (valid range 2-256)"));
        TestEqual(TEXT("the clamped value is the one reported"), Outputs.PathSteps, 256);
        TestEqual(TEXT("and it is the same bound the wrapper sizes its sample list with"),
            GeometryOps::SplinePathStepCount(500), 256);
    }

    // The same over-large steps with a supplied path: the fallback never runs, so nothing is
    // rewritten and nothing is warned about.
    {
        TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsAdvancedTest_NewMesh());
        GeometryOpsAdvancedTest_AppendQuad(Mesh.Get(), 0.0, 0.0);

        GeometryOps::FSweepParams Params;
        Params.Steps = 500;

        GeometryOps::FSweepPath Path;
        Path.Samples = GeometryOpsAdvancedTest_StraightPath(4, 400.0);
        Path.SplineLength = 400.0f;

        GeometryOps::FSweepOutputs Outputs;
        const GeometryOps::FOpResult Result = GeometryOps::Sweep(Mesh.Get(), Params, Path, Outputs);

        // Specifically that `steps` was not rewritten, not that the op said nothing at all: this
        // fixture's frames are all identity while the path runs up +Z, which is exactly the
        // section-plane-aligned path the op now reports. That warning is correct and unrelated
        // to the count under test here.
        TestTrue(TEXT("steps is not consumed on the spline branch, so it is not clamped"),
            GeometryOpsAdvancedTest_NoWarningStartingWith(Result, TEXT("steps clamped")));
        TestEqual(TEXT("pathSteps still comes from the samples"), Outputs.PathSteps, 4);
    }

    return true;
}

// ============================================================================
// append_buffers must not delete the TARGET mesh's UVs
// ============================================================================
//
// The engine's AppendBuffersToMesh does not merge UV layers, it REPLACES the count: it counts
// the UV sets in the incoming buffers and calls SetNumUVLayers(NumUVLayers) on the TARGET
// (MeshBasicEditFunctions.cpp:973-983), and the shrink path is RemoveAt, which destroys the
// overlay objects and every element in them (DynamicMeshAttributeSet.cpp:719-740). So a `box`
// followed by an `append_buffers` with no `uvs=` used to lose the BOX's UVs - silently, on
// geometry the append never touched, with no error and no warning anywhere.
//
// This is data loss, not a crash, which is why nothing caught it: every count the verb reports
// stays correct while the UVs go. The op pads the incoming buffers with placeholder UV sets so
// the engine is never asked to shrink.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOpsAdvancedAppendBuffersPreservesUVsTest,
    "PinWright.Geometry.Ops.Advanced.AppendBuffersKeepsTheTargetMeshUVs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryOpsAdvancedAppendBuffersPreservesUVsTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UDynamicMesh> Mesh(GeometryOpsAdvancedTest_NewMesh());
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
        Mesh.Get(), Options, FTransform::Identity, 100.0f, 100.0f, 100.0f, 0, 0, 0,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);

    const auto UVElementCount = [&Mesh]() -> int32
    {
        const UE::Geometry::FDynamicMeshAttributeSet* Attributes = Mesh->GetMeshRef().Attributes();
        const UE::Geometry::FDynamicMeshUVOverlay* Layer =
            (Attributes && Attributes->NumUVLayers() > 0) ? Attributes->GetUVLayer(0) : nullptr;
        return Layer ? Layer->ElementCount() : 0;
    };
    const auto UVLayerCount = [&Mesh]() -> int32
    {
        const UE::Geometry::FDynamicMeshAttributeSet* Attributes = Mesh->GetMeshRef().Attributes();
        return Attributes ? Attributes->NumUVLayers() : 0;
    };

    const int32 BoxUVLayers = UVLayerCount();
    const int32 BoxUVElements = UVElementCount();
    TestEqual(TEXT("a primitive box arrives with one UV layer"), BoxUVLayers, 1);
    TestTrue(TEXT("and that layer carries elements"), BoxUVElements > 0);

    // The exact document that used to lose them: an append with no uvs=.
    const GeometryOps::FOpResult Op = GeometryOpsAdvancedTest_AppendQuad(Mesh.Get(), 500.0, 500.0);
    TestTrue(*FString::Printf(TEXT("the UV-less append still succeeds. code='%s' msg='%s'"),
            *Op.ErrorCode, *Op.ErrorMessage), Op.bSuccess);

    TestEqual(TEXT("the box's UV LAYER survives an append that carried no uvs="),
        UVLayerCount(), BoxUVLayers);
    TestTrue(*FString::Printf(
            TEXT("and so do its UV ELEMENTS - restoring the layer count alone would give an empty "
                 "overlay, which is not preservation. had %d, now %d"),
            BoxUVElements, UVElementCount()),
        UVElementCount() >= BoxUVElements);

    // Preserving is not silent: the appended geometry got placeholder UVs it did not ask for,
    // and the author has to be able to find out. The RPC wrapper does not surface Warnings, so
    // the response shape is unchanged; the .pwmodel compiler reports them as STAGE_WARNING.
    bool bWarned = false;
    for (const FString& Warning : Op.Warnings)
    {
        bWarned = bWarned || Warning.Contains(TEXT("UV"));
    }
    TestTrue(TEXT("padding the incoming buffers is reported, not done behind the caller's back"),
        bWarned);

    // Every triangle - the box's and the appended quad's - must end up with set UV elements.
    // A padded layer that left the new triangles unset would trade this data-loss bug for the
    // partially-set overlay that crashes the boundary stitcher.
    bool bEveryTriangleSet = true;
    Mesh->ProcessMesh([&bEveryTriangleSet](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        const UE::Geometry::FDynamicMeshUVOverlay* Layer = ReadMesh.Attributes()->GetUVLayer(0);
        for (const int32 TriangleID : ReadMesh.TriangleIndicesItr())
        {
            bEveryTriangleSet = bEveryTriangleSet && Layer->IsSetTriangle(TriangleID);
        }
    });
    TestTrue(TEXT("the padded append leaves no triangle unset in UV channel 0"), bEveryTriangleSet);

    // The scope boundary, pinned deliberately. A mesh whose UV layer holds NO elements has no
    // UV data to lose, so it is NOT padded - `procedural_mesh` + `append_buffers` still ends
    // with the layer count the engine chooses, which is the state the shell and bevel crash
    // guards in GeometryOps_Modeling.cpp are written against and tested on. Changing this line
    // means those guards' repro documents stop reproducing.
    TStrongObjectPtr<UDynamicMesh> Bare(GeometryOpsAdvancedTest_NewMesh());
    const GeometryOps::FOpResult BareOp = GeometryOpsAdvancedTest_AppendQuad(Bare.Get(), 0.0, 0.0);
    TestTrue(TEXT("appending to a bare mesh succeeds"), BareOp.bSuccess);
    TestEqual(TEXT("an element-less UV layer is not padded, so nothing warns"),
        BareOp.Warnings.Num(), 0);

    return true;
}
