// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the non-mutating mesh-inspection verbs geometry.measure and
// geometry.check_health (MeshMeasureHandler.cpp).
//
// Strategy: spawn real DynamicMeshActors through the production create verbs
// (geometry.create_box / create_procedural_mesh + append_triangle) via
// InvokeHandlerWithCapture, then route measure / check_health through the same
// direct-handler path and assert the reported facts. The UNKNOWN_PARAMS case goes
// through the real dispatcher because param validation is a dispatcher concern, not
// a handler-body concern. Every created actor is destroyed so the (mutated) host is
// left clean.
//
// The parity test at the bottom also routes model.validate, because the field set the
// .pwmodel `health` block publishes is the thing check_health is being compared AGAINST -
// reading it off the live response rather than restating it here is what keeps the two
// surfaces from drifting apart again without a test noticing.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"

#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/Geometry/GeometryTestHelpers.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Dispatch/RpcDispatcher.h"

#include "Editor.h"
#include "Engine/World.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

namespace
{
    // Create a box DynamicMeshActor via the real geometry.create_box handler and
    // return its actual actor label (empty on failure). Uniquely named to avoid a
    // Unity-build ODR clash with other test-file helpers.
    FString SpawnMeasureProbeBox(const FString& Label, double Width, double Height, double Depth)
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("name"), Label);
        P->SetNumberField(TEXT("width"), Width);
        P->SetNumberField(TEXT("height"), Height);
        P->SetNumberField(TEXT("depth"), Depth);

        FTestResponseCapture Capture;
        if (!InvokeHandlerWithCapture(TEXT("geometry.create_box"), P, Capture)
            || !Capture.bSuccess || !Capture.Result.IsValid())
        {
            return FString();
        }
        FString Actual;
        Capture.Result->TryGetStringField(TEXT("name"), Actual);
        return Actual;
    }

    TSharedPtr<FJsonValue> MeasureProbeTriple(double A, double B, double C)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        Values.Add(MakeShared<FJsonValueNumber>(A));
        Values.Add(MakeShared<FJsonValueNumber>(B));
        Values.Add(MakeShared<FJsonValueNumber>(C));
        return MakeShared<FJsonValueArray>(Values);
    }

    // ONE edge-connected surface that crosses itself transversally, built by hand through the
    // real create_procedural_mesh + append_buffers verbs so the crossing is arithmetic rather
    // than an emergent property of a generator. Same construction as the compiler-side fixture
    // in Tests/Model/TestPwModelSelfIntersection.cpp, restated here because that one builds a
    // bare UDynamicMesh and this verb needs a placed actor:
    //
    //   H is a flat two-quad strip at z = 0, x in [0, 40], y in [-10, 10].
    //   V hinges off H's x = 0 edge, rises to z = +20 at x = 10 and descends to z = -10 at
    //   x = 30, so it passes DOWN THROUGH z = 0 at x = 70/3 - inside H's SECOND quad.
    //
    // The crossing pairs are V's two far triangles against H's two far triangles, none of which
    // shares a vertex with its partner, so none is excluded as topologically connected.
    // Returns the actual actor label (empty on failure).
    FString SpawnMeasureProbeCrossingSheets(const FString& Label)
    {
        FString Actual;
        {
            TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
            P->SetStringField(TEXT("name"), Label);
            FTestResponseCapture Capture;
            if (!InvokeHandlerWithCapture(TEXT("geometry.create_procedural_mesh"), P, Capture)
                || !Capture.bSuccess || !Capture.Result.IsValid())
            {
                return FString();
            }
            Capture.Result->TryGetStringField(TEXT("name"), Actual);
        }
        if (Actual.IsEmpty())
        {
            return FString();
        }

        TArray<TSharedPtr<FJsonValue>> Vertices;
        Vertices.Add(MeasureProbeTriple(0.0, -10.0, 0.0));    // H0
        Vertices.Add(MeasureProbeTriple(0.0, 10.0, 0.0));     // H1
        Vertices.Add(MeasureProbeTriple(20.0, -10.0, 0.0));   // H2
        Vertices.Add(MeasureProbeTriple(20.0, 10.0, 0.0));    // H3
        Vertices.Add(MeasureProbeTriple(40.0, -10.0, 0.0));   // H4
        Vertices.Add(MeasureProbeTriple(40.0, 10.0, 0.0));    // H5
        Vertices.Add(MeasureProbeTriple(10.0, -10.0, 20.0));  // V0
        Vertices.Add(MeasureProbeTriple(10.0, 10.0, 20.0));   // V1
        Vertices.Add(MeasureProbeTriple(30.0, -10.0, -10.0)); // V2
        Vertices.Add(MeasureProbeTriple(30.0, 10.0, -10.0));  // V3

        TArray<TSharedPtr<FJsonValue>> Triangles;
        Triangles.Add(MeasureProbeTriple(0, 2, 3));
        Triangles.Add(MeasureProbeTriple(0, 3, 1));
        Triangles.Add(MeasureProbeTriple(2, 4, 5));
        Triangles.Add(MeasureProbeTriple(2, 5, 3));
        Triangles.Add(MeasureProbeTriple(0, 1, 7));
        Triangles.Add(MeasureProbeTriple(0, 7, 6));
        Triangles.Add(MeasureProbeTriple(6, 7, 9));
        Triangles.Add(MeasureProbeTriple(6, 9, 8));

        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("actorName"), Actual);
        P->SetArrayField(TEXT("vertices"), Vertices);
        P->SetArrayField(TEXT("triangles"), Triangles);

        FTestResponseCapture Capture;
        if (!InvokeHandlerWithCapture(TEXT("geometry.append_buffers"), P, Capture)
            || !Capture.bSuccess)
        {
            GeometryTestHelpers::DestroyActorsWithLabel(Actual);
            return FString();
        }
        return Actual;
    }

    // Route geometry.check_health at one actor, optionally opting into the self-intersection
    // measurement. Returns the result object (invalid on failure).
    TSharedPtr<FJsonObject> MeasureProbeCheckHealth(const FString& ActorName,
        bool bCheckSelfIntersection)
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("actorName"), ActorName);
        if (bCheckSelfIntersection)
        {
            P->SetBoolField(TEXT("checkSelfIntersection"), true);
        }

        FTestResponseCapture Capture;
        if (!InvokeHandlerWithCapture(TEXT("geometry.check_health"), P, Capture)
            || !Capture.bSuccess)
        {
            return nullptr;
        }
        return Capture.Result;
    }
}

// ============================================================================
// geometry.measure - a centered box reports its dimensions, volume and counts.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryMeasureBoxTest,
    "PinWright.geometry.measure.Box",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryMeasureBoxTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("geometry.measure needs a placed box probe"));
        return true;
    }

    const FString Label = FString::Printf(TEXT("PW_MeasureBox_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    // Distinct dims so the axis mapping (width->X, height->Y, depth->Z) is verified too.
    const FString Actual = SpawnMeasureProbeBox(Label, 200.0, 120.0, 60.0);
    if (!TestFalse(TEXT("geometry.create_box produced a probe actor"), Actual.IsEmpty()))
    {
        return true;
    }

    TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
    P->SetStringField(TEXT("actorName"), Actual);

    FTestResponseCapture Capture;
    TestTrue(TEXT("geometry.measure handler registered"),
        InvokeHandlerWithCapture(TEXT("geometry.measure"), P, Capture));
    TestTrue(TEXT("geometry.measure succeeded"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        double Tri = 0.0, Vert = 0.0, Edge = 0.0, Vol = 0.0, Area = 0.0;
        Capture.Result->TryGetNumberField(TEXT("triangleCount"), Tri);
        Capture.Result->TryGetNumberField(TEXT("vertexCount"), Vert);
        Capture.Result->TryGetNumberField(TEXT("edgeCount"), Edge);
        Capture.Result->TryGetNumberField(TEXT("volume"), Vol);
        Capture.Result->TryGetNumberField(TEXT("area"), Area);
        TestTrue(TEXT("triangleCount > 0"), Tri > 0.0);
        TestTrue(TEXT("vertexCount > 0"), Vert > 0.0);
        TestTrue(TEXT("edgeCount > 0"), Edge > 0.0);

        // volume and area are exactly knowable for a closed 200x120x60 box, and asserting
        // only `> 0` cannot discriminate the one failure that actually happens here: the
        // handler reads them through the engine's two-out-param
        // GetMeshVolumeArea(Mesh, SurfaceArea, Volume) (MeshMeasureHandler.cpp), whose
        // argument order is easy to transpose. Swapped, `volume` reports 86400 and `area`
        // reports 1440000 — both still positive, both still non-zero, and every loose
        // assertion still green. Pin the values instead (1% tolerance for float
        // accumulation in the divergence-theorem sum).
        const double ExpectedVolume = 200.0 * 120.0 * 60.0;                              // 1,440,000
        const double ExpectedArea = 2.0 * (200.0 * 120.0 + 200.0 * 60.0 + 120.0 * 60.0); // 86,400
        TestTrue(*FString::Printf(TEXT("volume ~= W*H*D = %.0f (got %.1f)"), ExpectedVolume, Vol),
            FMath::IsNearlyEqual(Vol, ExpectedVolume, ExpectedVolume * 0.01));
        TestTrue(*FString::Printf(TEXT("area ~= 2(WH+WD+HD) = %.0f (got %.1f)"), ExpectedArea, Area),
            FMath::IsNearlyEqual(Area, ExpectedArea, ExpectedArea * 0.01));

        FString Space, Units;
        Capture.Result->TryGetStringField(TEXT("space"), Space);
        Capture.Result->TryGetStringField(TEXT("units"), Units);
        TestEqual(TEXT("space defaults to local"), Space, FString(TEXT("local")));
        TestEqual(TEXT("units is cm"), Units, FString(TEXT("cm")));

        const TSharedPtr<FJsonObject>* BBox = nullptr;
        if (TestTrue(TEXT("result carries a bbox object"),
                Capture.Result->TryGetObjectField(TEXT("bbox"), BBox)) && BBox)
        {
            const TSharedPtr<FJsonObject>* Size = nullptr;
            if (TestTrue(TEXT("bbox carries a size object"),
                    (*BBox)->TryGetObjectField(TEXT("size"), Size)) && Size)
            {
                double SX = 0.0, SY = 0.0, SZ = 0.0;
                (*Size)->TryGetNumberField(TEXT("x"), SX);
                (*Size)->TryGetNumberField(TEXT("y"), SY);
                (*Size)->TryGetNumberField(TEXT("z"), SZ);
                TestTrue(TEXT("bbox size x ~= box width"), FMath::IsNearlyEqual(SX, 200.0, 1.0));
                TestTrue(TEXT("bbox size y ~= box height"), FMath::IsNearlyEqual(SY, 120.0, 1.0));
                TestTrue(TEXT("bbox size z ~= box depth"), FMath::IsNearlyEqual(SZ, 60.0, 1.0));
            }
        }
    }

    GeometryTestHelpers::DestroyActorsWithLabel(Actual);
    return true;
}

// ============================================================================
// geometry.measure - a missing actor yields the typed ACTOR_NOT_FOUND error.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryMeasureNotFoundTest,
    "PinWright.geometry.measure.NotFound",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryMeasureNotFoundTest::RunTest(const FString& Parameters)
{
    bSuppressLogWarnings = true;

    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("geometry.measure resolves its actor against the editor world, so the "
                 "not-found rejection could not be exercised"));
        return true;
    }

    TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
    P->SetStringField(TEXT("actorName"),
        FString::Printf(TEXT("PW_NoSuchMeshActor_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

    FTestResponseCapture Capture;
    TestTrue(TEXT("geometry.measure handler registered"),
        InvokeHandlerWithCapture(TEXT("geometry.measure"), P, Capture));
    TestFalse(TEXT("geometry.measure fails for a missing actor"), Capture.bSuccess);
    TestEqual(TEXT("missing actor yields ACTOR_NOT_FOUND"),
        Capture.ErrorCode, FString(TEXT("ACTOR_NOT_FOUND")));
    return true;
}

// ============================================================================
// geometry.measure - an unknown arg is rejected by the dispatcher (UNKNOWN_PARAMS).
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryMeasureRejectsUnknownParamTest,
    "PinWright.geometry.measure.RejectsUnknownParam",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryMeasureRejectsUnknownParamTest::RunTest(const FString& Parameters)
{
    bSuppressLogWarnings = true;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
    P->SetStringField(TEXT("actorName"), TEXT("PW_Whatever"));
    P->SetStringField(TEXT("bogusUnknownParam"), TEXT("x"));

    bool bSuccess = false;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("geometry.measure"),
        TEXT("req-measure-unknown"), P, bSuccess, ErrorCode);

    TestFalse(TEXT("geometry.measure rejects an unknown arg"), bSuccess);
    TestEqual(TEXT("unknown arg yields UNKNOWN_PARAMS"),
        ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
    return true;
}

// ============================================================================
// geometry.check_health - a closed box is watertight, defect-free and healthy.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCheckHealthClosedBoxTest,
    "PinWright.geometry.check_health.ClosedBox",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCheckHealthClosedBoxTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("geometry.check_health needs a placed DynamicMeshActor probe"));
        return true;
    }

    const FString Label = FString::Printf(TEXT("PW_HealthBox_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString Actual = SpawnMeasureProbeBox(Label, 100.0, 100.0, 100.0);
    if (!TestFalse(TEXT("geometry.create_box produced a probe actor"), Actual.IsEmpty()))
    {
        return true;
    }

    TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
    P->SetStringField(TEXT("actorName"), Actual);

    FTestResponseCapture Capture;
    TestTrue(TEXT("geometry.check_health handler registered"),
        InvokeHandlerWithCapture(TEXT("geometry.check_health"), P, Capture));
    TestTrue(TEXT("geometry.check_health succeeded"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        bool bClosed = false, bHealthy = false;
        Capture.Result->TryGetBoolField(TEXT("isClosed"), bClosed);
        Capture.Result->TryGetBoolField(TEXT("healthy"), bHealthy);
        TestTrue(TEXT("closed box is watertight"), bClosed);
        TestTrue(TEXT("closed box is healthy"), bHealthy);

        double Boundary = -1.0, Degen = -1.0, NonMan = -1.0, Comp = 0.0, Tri = 0.0;
        Capture.Result->TryGetNumberField(TEXT("boundaryEdges"), Boundary);
        Capture.Result->TryGetNumberField(TEXT("degenerateTriangles"), Degen);
        Capture.Result->TryGetNumberField(TEXT("nonManifoldVertices"), NonMan);
        Capture.Result->TryGetNumberField(TEXT("componentCount"), Comp);
        Capture.Result->TryGetNumberField(TEXT("triangleCount"), Tri);
        TestEqual(TEXT("no boundary edges"), Boundary, 0.0);
        TestEqual(TEXT("no degenerate triangles"), Degen, 0.0);
        TestEqual(TEXT("no non-manifold vertices"), NonMan, 0.0);
        TestTrue(TEXT("at least one connected component"), Comp >= 1.0);
        TestTrue(TEXT("triangleCount > 0"), Tri > 0.0);
    }

    GeometryTestHelpers::DestroyActorsWithLabel(Actual);
    return true;
}

// ============================================================================
// geometry.check_health - a single-triangle mesh is open (boundary edges, not closed).
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCheckHealthOpenMeshTest,
    "PinWright.geometry.check_health.OpenMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCheckHealthOpenMeshTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("geometry.check_health needs a placed DynamicMeshActor probe"));
        return true;
    }

    const FString Label = FString::Printf(TEXT("PW_HealthOpen_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    // Spawn an empty DynamicMeshActor...
    FString Actual;
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("name"), Label);
        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("geometry.create_procedural_mesh handler registered"),
                InvokeHandlerWithCapture(TEXT("geometry.create_procedural_mesh"), P, Capture))
            || !TestTrue(TEXT("create_procedural_mesh succeeded"), Capture.bSuccess)
            || !Capture.Result.IsValid())
        {
            return true;
        }
        Capture.Result->TryGetStringField(TEXT("name"), Actual);
    }
    if (!TestFalse(TEXT("procedural mesh actor has a label"), Actual.IsEmpty()))
    {
        return true;
    }

    // ...then append a single triangle: 3 boundary edges, an open (non-closed) mesh.
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("actorName"), Actual);
        FTestResponseCapture Capture;
        TestTrue(TEXT("geometry.append_triangle handler registered"),
            InvokeHandlerWithCapture(TEXT("geometry.append_triangle"), P, Capture));
        TestTrue(TEXT("append_triangle succeeded"), Capture.bSuccess);
    }

    TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
    P->SetStringField(TEXT("actorName"), Actual);

    FTestResponseCapture Capture;
    TestTrue(TEXT("geometry.check_health handler registered"),
        InvokeHandlerWithCapture(TEXT("geometry.check_health"), P, Capture));
    TestTrue(TEXT("geometry.check_health succeeded"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        bool bClosed = true, bHealthy = true;
        Capture.Result->TryGetBoolField(TEXT("isClosed"), bClosed);
        Capture.Result->TryGetBoolField(TEXT("healthy"), bHealthy);
        TestFalse(TEXT("single-triangle mesh is not closed"), bClosed);
        TestFalse(TEXT("open mesh is not healthy"), bHealthy);

        double Boundary = 0.0, Tri = 0.0;
        Capture.Result->TryGetNumberField(TEXT("boundaryEdges"), Boundary);
        Capture.Result->TryGetNumberField(TEXT("triangleCount"), Tri);
        TestTrue(TEXT("open mesh reports boundary edges"), Boundary > 0.0);
        TestTrue(TEXT("triangleCount > 0"), Tri > 0.0);
    }

    GeometryTestHelpers::DestroyActorsWithLabel(Actual);
    return true;
}

// ============================================================================
// geometry.check_health - the self-intersection measurement is opt-in, and its
// absence is STATED rather than reported as a clean zero.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCheckHealthSelfIntersectionOptInTest,
    "PinWright.geometry.check_health.SelfIntersectionIsOptInAndNeverSilentlyZero",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCheckHealthSelfIntersectionOptInTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("geometry.check_health needs a placed DynamicMeshActor probe"));
        return true;
    }

    const FString Label = FString::Printf(TEXT("PW_HealthOptIn_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString Actual = SpawnMeasureProbeBox(Label, 100.0, 100.0, 100.0);
    if (!TestFalse(TEXT("geometry.create_box produced a probe actor"), Actual.IsEmpty()))
    {
        return true;
    }

    // DEFAULT: not measured, and SAID so. The three counts must be ABSENT, not zero - a `0`
    // here would answer "no crossings" to a caller who learned the three-term gate from the
    // model.* docs, which is the exact cannot-fail reading this pair exists to remove.
    if (const TSharedPtr<FJsonObject> Default = MeasureProbeCheckHealth(Actual, false))
    {
        bool bMeasured = true;
        TestTrue(TEXT("the response states whether self-intersection was measured"),
            Default->TryGetBoolField(TEXT("selfIntersectionsMeasured"), bMeasured));
        TestFalse(TEXT("and it was not, because the caller did not ask"), bMeasured);
        TestFalse(TEXT("so selfIntersections is absent rather than a clean-looking 0"),
            Default->HasField(TEXT("selfIntersections")));
        TestFalse(TEXT("and selfIntersectingComponents with it"),
            Default->HasField(TEXT("selfIntersectingComponents")));
        TestFalse(TEXT("and selfIntersectionsTruncated with it"),
            Default->HasField(TEXT("selfIntersectionsTruncated")));
    }
    else
    {
        AddError(TEXT("geometry.check_health failed on the default (no-flag) call"));
    }

    // OPT-IN on a box: measured, and measured CLEAN. This is the answer the default case must be
    // distinguishable from, so both halves of the pair are asserted against one mesh.
    if (const TSharedPtr<FJsonObject> OptedIn = MeasureProbeCheckHealth(Actual, true))
    {
        bool bMeasured = false;
        OptedIn->TryGetBoolField(TEXT("selfIntersectionsMeasured"), bMeasured);
        TestTrue(TEXT("checkSelfIntersection=true measures a box-sized mesh"), bMeasured);

        double Pairs = -1.0, Components = -1.0;
        bool bTruncated = true;
        TestTrue(TEXT("and publishes the crossing count"),
            OptedIn->TryGetNumberField(TEXT("selfIntersections"), Pairs));
        TestTrue(TEXT("and the count of shells at fault"),
            OptedIn->TryGetNumberField(TEXT("selfIntersectingComponents"), Components));
        TestTrue(TEXT("and whether that count is a floor"),
            OptedIn->TryGetBoolField(TEXT("selfIntersectionsTruncated"), bTruncated));
        TestEqual(TEXT("a box does not cross itself"), Pairs, 0.0);
        TestEqual(TEXT("so no shell is at fault"), Components, 0.0);
        TestFalse(TEXT("and the zero is a total, not a truncated floor"), bTruncated);
    }
    else
    {
        AddError(TEXT("geometry.check_health failed on the checkSelfIntersection call"));
    }

    GeometryTestHelpers::DestroyActorsWithLabel(Actual);
    return true;
}

// ============================================================================
// geometry.check_health - the opted-in measurement actually reports a surface
// that passes through itself, which no other field on the response can see.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCheckHealthSelfIntersectionCrossingTest,
    "PinWright.geometry.check_health.SelfIntersectionCountsACrossingSurface",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCheckHealthSelfIntersectionCrossingTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("the crossing-sheets probe is built through the real create/append verbs"));
        return true;
    }

    const FString Label = FString::Printf(TEXT("PW_HealthCross_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString Actual = SpawnMeasureProbeCrossingSheets(Label);
    if (!TestFalse(TEXT("the crossing-sheets probe actor was built"), Actual.IsEmpty()))
    {
        return true;
    }

    if (const TSharedPtr<FJsonObject> Result = MeasureProbeCheckHealth(Actual, true))
    {
        // Pin the fixture first. If it ever stops being ONE edge-connected component the
        // per-shell rule would exclude the crossing, and the assertion below would go green for
        // a reason that has nothing to do with the verb.
        double Components = 0.0, Triangles = 0.0;
        Result->TryGetNumberField(TEXT("componentCount"), Components);
        Result->TryGetNumberField(TEXT("triangleCount"), Triangles);
        TestEqual(TEXT("the two sheets and their hinge are one edge-connected surface"),
            Components, 1.0);
        TestEqual(TEXT("built from the eight triangles the fixture uploads"), Triangles, 8.0);

        bool bMeasured = false;
        double Pairs = 0.0, Faulty = 0.0;
        Result->TryGetBoolField(TEXT("selfIntersectionsMeasured"), bMeasured);
        Result->TryGetNumberField(TEXT("selfIntersections"), Pairs);
        Result->TryGetNumberField(TEXT("selfIntersectingComponents"), Faulty);
        TestTrue(TEXT("the surface is measured"), bMeasured);
        TestTrue(*FString::Printf(
            TEXT("and the wall passing through the floor is counted (%.0f pair(s)) - nothing ")
            TEXT("else on this response moves for it at all"), Pairs), Pairs > 0.0);
        TestEqual(TEXT("in the single shell that crosses itself"), Faulty, 1.0);
    }
    else
    {
        AddError(TEXT("geometry.check_health failed on the crossing-sheets probe"));
    }

    GeometryTestHelpers::DestroyActorsWithLabel(Actual);
    return true;
}

// ============================================================================
// geometry.check_health publishes every field the .pwmodel `health` block does.
//
// THE DEFECT THIS GUARDS. Both surfaces read one GeometryUtils::FMeshHealth from one walk and
// answer about the same mesh, but they serialize it in two different files. Twice now a field
// was added to the struct and emitted on only one of them - `unreferencedVertices`, and the
// self-intersection trio - leaving a caller who learned the field set from the model.* docs to
// find it missing on check_health, with nothing in the suite comparing the two.
//
// The model side is read off a LIVE model.validate response rather than restated as a literal
// list here, so a field added there and forgotten here fails this test instead of quietly
// agreeing with a stale copy.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCheckHealthFieldSetParityTest,
    "PinWright.geometry.check_health.FieldSetMatchesModelHealthBlock",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCheckHealthFieldSetParityTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("the check_health half of the comparison needs a placed probe actor"));
        return true;
    }

    // The model side, from a one-part document that reaches the merge stage.
    TSharedPtr<FJsonObject> ModelHealth;
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("text"),
            TEXT("pwmodel 0\n")
            TEXT("part body {\n")
            TEXT("    box size=(100, 100, 100)\n")
            TEXT("}\n"));

        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("model.validate handler registered"),
                InvokeHandlerWithCapture(TEXT("model.validate"), P, Capture))
            || !TestTrue(TEXT("model.validate succeeded"), Capture.bSuccess)
            || !Capture.Result.IsValid())
        {
            return true;
        }

        const TSharedPtr<FJsonObject>* HealthPtr = nullptr;
        if (!TestTrue(TEXT("model.validate published a health block"),
                Capture.Result->TryGetObjectField(TEXT("health"), HealthPtr)) || !HealthPtr)
        {
            return true;
        }
        ModelHealth = *HealthPtr;
    }

    // Guard against a vacuous pass: if the model side ever stops publishing the two fields this
    // comparison exists for, the loop below would compare a shrunken set and report green.
    TestTrue(TEXT("the model health block carries unreferencedVertices"),
        ModelHealth->HasField(TEXT("unreferencedVertices")));
    TestTrue(TEXT("and the self-intersection count a box is small enough to be measured for"),
        ModelHealth->HasField(TEXT("selfIntersections")));

    const FString Label = FString::Printf(TEXT("PW_HealthParity_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString Actual = SpawnMeasureProbeBox(Label, 100.0, 100.0, 100.0);
    if (!TestFalse(TEXT("geometry.create_box produced a probe actor"), Actual.IsEmpty()))
    {
        return true;
    }

    // Opted in, because the self-intersection trio is opt-in on this verb - that is the one
    // stated difference between the two surfaces, and it is a cost decision, not a field-set one.
    if (const TSharedPtr<FJsonObject> Health = MeasureProbeCheckHealth(Actual, true))
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>> Field : ModelHealth->Values)
        {
            TestTrue(*FString::Printf(
                TEXT("geometry.check_health publishes '%s', which the .pwmodel health block ")
                TEXT("publishes for the same mesh"), *Field.Key),
                Health->HasField(Field.Key));
        }
    }
    else
    {
        AddError(TEXT("geometry.check_health failed on the parity probe"));
    }

    GeometryTestHelpers::DestroyActorsWithLabel(Actual);
    return true;
}
