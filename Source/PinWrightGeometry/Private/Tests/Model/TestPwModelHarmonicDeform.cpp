// Copyright (c) 2026 Alexander Penkin. MIT License.

// harmonic_deform: the op that exists so INVERTED hand-authored models can be rebuilt from
// primitives.
//
// The motivating shape is a mesh whose rings are lobed by a sum of sinusoids in the azimuth.
// Written as a hand-rolled append_buffers block, such a model carries a negative signedVolume
// and hundreds of inconsistent edges, because its winding was decided by hand instead of
// coming from the compiler. The rebuild is a later step and is not attempted here.
// What is asserted here is the precondition for it: that this op moves the surface where it
// says it does, and does NOT hand back the same defect from the other direction.
//
// The file is deliberately two layers, because they can fail independently:
//
//  - Op level, against a transient UDynamicMesh with no world. This is where the DISPLACEMENT
//    is measured, vertex by vertex against the closed-form prediction, and where the refusals
//    are observed. A wire test cannot see a vertex.
//  - Document level, through the real dispatcher. The op is reachable only if the parser
//    publishes the parameters and the compiler routes them; a compiler that reads a parameter
//    the op table does not declare is unreachable in the opposite direction, because the parser
//    rejects the document before the compiler ever sees it.
#include "Misc/AutomationTest.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Geometry/GeometryOps_Modeling.h"
#include "Handlers/Geometry/GeometryUtils.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"

#include "Dispatch/RpcDispatcher.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/Infra/DispatcherTestHelpers.h"

#include "Model/PwModelParser.h"

#include "GeometryScript/MeshPrimitiveFunctions.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper
// with a common name would collide with a sibling test TU once Unity merges them.

// A closed lat/long sphere centred on the ORIGIN, which is where the op's default axis line is.
// 48 azimuth steps: comfortably above the Nyquist bound for the orders used below, so the
// aliasing warning is silent here and has its own test rather than contaminating these.
UDynamicMesh* PwHarmonicTest_NewSphere()
{
    UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage());
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSphereLatLong(
        Mesh, Options, FTransform::Identity, 100.0f, 24, 48,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);
    return Mesh;
}

// Every vertex position by ID, so a test can compare the SAME vertex before and after. The op
// changes no topology, so the IDs are stable across it - and if a later change makes them
// unstable, these tests fail rather than silently comparing different vertices.
TMap<int32, FVector3d> PwHarmonicTest_Snapshot(UDynamicMesh* Mesh)
{
    TMap<int32, FVector3d> Positions;
    Mesh->ProcessMesh([&Positions](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        for (int32 VertexID : ReadMesh.VertexIndicesItr())
        {
            Positions.Add(VertexID, ReadMesh.GetVertex(VertexID));
        }
    });
    return Positions;
}

GeometryOps::FHarmonicTerm PwHarmonicTest_Term(int32 Order, double Amplitude, double PhaseDegrees)
{
    GeometryOps::FHarmonicTerm Term;
    Term.Order = Order;
    Term.Amplitude = Amplitude;
    Term.PhaseDegrees = PhaseDegrees;
    return Term;
}

// The prediction, spelled independently of the op's own loop: the sum of a*sin(n*theta + phase)
// at the azimuth of a position about Z. Written out here rather than shared with the
// implementation on purpose - a test that calls the code under test to compute its own
// expectation asserts only that the function is deterministic.
double PwHarmonicTest_SumAboutZ(const FVector3d& Position,
                                const TArray<GeometryOps::FHarmonicTerm>& Terms)
{
    const double Theta = FMath::Atan2(Position.Y, Position.X);
    double Sum = 0.0;
    for (const GeometryOps::FHarmonicTerm& Term : Terms)
    {
        Sum += Term.Amplitude
            * FMath::Sin(Term.Order * Theta + FMath::DegreesToRadians(Term.PhaseDegrees));
    }
    return Sum;
}

double PwHarmonicTest_RadiusAboutZ(const FVector3d& Position)
{
    return FMath::Sqrt(Position.X * Position.X + Position.Y * Position.Y);
}

// One model.validate run, with the diagnostic shaping defaults turned off: the defaults (5,
// collapsed) can drop or fold the one diagnostic a test is looking for.
struct FPwHarmonicTest_Run
{
    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
};

FPwHarmonicTest_Run PwHarmonicTest_Validate(const TCHAR* Source)
{
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::FSinkPtr Sink;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("text"), Source);
    Payload->SetNumberField(TEXT("diagnosticLimit"), 0);
    Payload->SetBoolField(TEXT("collapseDiagnostics"), false);

    FPwHarmonicTest_Run Run;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("model.validate"), TEXT("t-harmonic"),
        Payload, Run.bSuccess, Run.Result, Run.ErrorCode);
    return Run;
}

// Every failure message carries the whole diagnostic list; "expected true, got false" on a
// compiler test costs a rerun under a debugger to learn anything at all.
FString PwHarmonicTest_Describe(const FPwHarmonicTest_Run& Run)
{
    if (!Run.Result.IsValid())
    {
        return FString::Printf(TEXT("success=%d error='%s' <no result>"),
            Run.bSuccess ? 1 : 0, *Run.ErrorCode);
    }

    TArray<FString> Lines;
    const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
    if (Run.Result->TryGetArrayField(TEXT("diagnostics"), Diagnostics) && Diagnostics)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Diagnostics)
        {
            const TSharedPtr<FJsonObject>* Entry = nullptr;
            if (Value.IsValid() && Value->TryGetObject(Entry) && (*Entry).IsValid())
            {
                Lines.Add(FString::Printf(TEXT("%s [%s] line %d: %s"),
                    *(*Entry)->GetStringField(TEXT("severity")),
                    *(*Entry)->GetStringField(TEXT("code")),
                    static_cast<int32>((*Entry)->GetNumberField(TEXT("line"))),
                    *(*Entry)->GetStringField(TEXT("message"))));
            }
        }
    }
    return FString::Printf(TEXT("success=%d error='%s' diagnostics=[%s]"),
        Run.bSuccess ? 1 : 0, *Run.ErrorCode, *FString::Join(Lines, TEXT("; ")));
}

bool PwHarmonicTest_HasCode(const FPwHarmonicTest_Run& Run, const TCHAR* Code)
{
    const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
    if (!Run.Result.IsValid() || !Run.Result->TryGetArrayField(TEXT("diagnostics"), Diagnostics)
        || !Diagnostics)
    {
        return false;
    }
    for (const TSharedPtr<FJsonValue>& Value : *Diagnostics)
    {
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        if (Value.IsValid() && Value->TryGetObject(Entry) && (*Entry).IsValid()
            && (*Entry)->GetStringField(TEXT("code")) == Code)
        {
            return true;
        }
    }
    return false;
}
}

// ============================================================================
// (a) The shape change actually occurs, and zero amplitude does not
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwHarmonicZeroAmplitudeMovesNothingTest,
    "PinWright.Model.HarmonicDeform.ZeroAmplitudeMovesNothing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwHarmonicZeroAmplitudeMovesNothingTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Mesh = PwHarmonicTest_NewSphere();
    const TMap<int32, FVector3d> Before = PwHarmonicTest_Snapshot(Mesh);

    GeometryOps::FHarmonicDeformParams Params;
    Params.Terms.Add(PwHarmonicTest_Term(3, 0.0, 45.0));
    Params.Terms.Add(PwHarmonicTest_Term(5, 0.0, 0.0));

    const GeometryOps::FOpResult Op = GeometryOps::HarmonicDeform(Mesh, Params);
    TestTrue(*FString::Printf(TEXT("amplitude 0 succeeds. %s: %s"),
        *Op.ErrorCode, *Op.ErrorMessage), Op.bSuccess);

    // bChanged is the OBSERVABLE half. A no-op that reports itself as a change is how an author
    // concludes an op did something, and the whole point of asserting the positions too is that
    // the flag cannot be right by accident.
    TestFalse(TEXT("amplitude 0 reports no change"), Op.bChanged);

    const TMap<int32, FVector3d> After = PwHarmonicTest_Snapshot(Mesh);
    TestEqual(TEXT("no vertex was added or removed"), After.Num(), Before.Num());

    int32 Moved = 0;
    for (const TPair<int32, FVector3d>& Pair : Before)
    {
        const FVector3d* Now = After.Find(Pair.Key);
        if (!Now || !Now->Equals(Pair.Value, UE_DOUBLE_KINDA_SMALL_NUMBER))
        {
            ++Moved;
        }
    }
    TestEqual(TEXT("amplitude 0 moved no vertex at all"), Moved, 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwHarmonicRadialScalesRadiusTest,
    "PinWright.Model.HarmonicDeform.RadialTermScalesRadiusByThePredictedFactor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwHarmonicRadialScalesRadiusTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Mesh = PwHarmonicTest_NewSphere();
    const TMap<int32, FVector3d> Before = PwHarmonicTest_Snapshot(Mesh);

    // Two superposed radial terms: k = 1 + 0.20*sin(3t + phase) + 0.11*sin(5t + phase).
    // This is the sum case, not one wave.
    GeometryOps::FHarmonicDeformParams Params;
    Params.Terms.Add(PwHarmonicTest_Term(3, 0.20, 30.0));
    Params.Terms.Add(PwHarmonicTest_Term(5, 0.11, 30.0));

    const GeometryOps::FOpResult Op = GeometryOps::HarmonicDeform(Mesh, Params);
    if (!TestTrue(*FString::Printf(TEXT("the radial deform succeeds. %s: %s"),
            *Op.ErrorCode, *Op.ErrorMessage), Op.bSuccess))
    {
        return false;
    }
    TestTrue(TEXT("a non-zero amplitude reports a change"), Op.bChanged);
    TestEqual(TEXT("no triangle was added or removed"), Op.TrianglesAfter, Op.TrianglesBefore);

    const TMap<int32, FVector3d> After = PwHarmonicTest_Snapshot(Mesh);

    int32 Checked = 0;
    int32 MovedOffAxis = 0;
    for (const TPair<int32, FVector3d>& Pair : Before)
    {
        const FVector3d* Now = After.Find(Pair.Key);
        if (!Now)
        {
            AddError(FString::Printf(TEXT("vertex %d disappeared"), Pair.Key));
            return false;
        }

        const double OldRadius = PwHarmonicTest_RadiusAboutZ(Pair.Value);
        if (OldRadius <= KINDA_SMALL_NUMBER)
        {
            // A pole. Its azimuth is undefined, so the op leaves it alone by contract.
            TestTrue(TEXT("a vertex on the axis is left where it was"),
                Now->Equals(Pair.Value, UE_DOUBLE_KINDA_SMALL_NUMBER));
            continue;
        }

        const double Predicted = OldRadius * (1.0 + PwHarmonicTest_SumAboutZ(Pair.Value, Params.Terms));
        const double Actual = PwHarmonicTest_RadiusAboutZ(*Now);
        if (FMath::Abs(Actual - Predicted) > 1e-6)
        {
            AddError(FString::Printf(TEXT(
                "vertex %d: radius %.9g predicted, %.9g measured (was %.9g)"),
                Pair.Key, Predicted, Actual, OldRadius));
            return false;
        }

        // The axis coordinate and the azimuth are UNTOUCHED - that is what makes the map a pure
        // radial scale, and it is the property the winding argument rests on.
        TestEqual(TEXT("a radial deform does not move a vertex along the axis"), Now->Z, Pair.Value.Z);
        TestTrue(TEXT("a radial deform does not rotate a vertex"),
            FMath::IsNearlyEqual(FMath::Atan2(Now->Y, Now->X),
                FMath::Atan2(Pair.Value.Y, Pair.Value.X), 1e-9));

        ++Checked;
        if (!Now->Equals(Pair.Value, UE_DOUBLE_KINDA_SMALL_NUMBER))
        {
            ++MovedOffAxis;
        }
    }

    TestTrue(TEXT("the sphere had off-axis vertices to check"), Checked > 100);
    // Without this the test would pass on an op that moved nothing: every prediction would be
    // satisfied by the identity at amplitudes of zero, and the terms above are not zero.
    TestTrue(TEXT("the deform actually moved most off-axis vertices"), MovedOffAxis > Checked / 2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwHarmonicAxialShiftsAlongAxisTest,
    "PinWright.Model.HarmonicDeform.AxialTermShiftsAlongTheAxisByThePredictedAmount",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwHarmonicAxialShiftsAlongAxisTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Mesh = PwHarmonicTest_NewSphere();
    const TMap<int32, FVector3d> Before = PwHarmonicTest_Snapshot(Mesh);

    // The other half of a lobed rim: it alternates in HEIGHT as well as in radius.
    // Amplitude is in Unreal units here, not a fraction.
    GeometryOps::FHarmonicDeformParams Params;
    Params.Target = GeometryOps::EHarmonicTarget::Axial;
    Params.Terms.Add(PwHarmonicTest_Term(2, 12.0, 0.0));

    const GeometryOps::FOpResult Op = GeometryOps::HarmonicDeform(Mesh, Params);
    if (!TestTrue(*FString::Printf(TEXT("the axial deform succeeds. %s: %s"),
            *Op.ErrorCode, *Op.ErrorMessage), Op.bSuccess))
    {
        return false;
    }
    TestTrue(TEXT("a non-zero amplitude reports a change"), Op.bChanged);

    const TMap<int32, FVector3d> After = PwHarmonicTest_Snapshot(Mesh);

    int32 Checked = 0;
    for (const TPair<int32, FVector3d>& Pair : Before)
    {
        const FVector3d* Now = After.Find(Pair.Key);
        if (!Now)
        {
            AddError(FString::Printf(TEXT("vertex %d disappeared"), Pair.Key));
            return false;
        }
        if (PwHarmonicTest_RadiusAboutZ(Pair.Value) <= KINDA_SMALL_NUMBER)
        {
            continue;
        }

        const double Predicted = Pair.Value.Z + PwHarmonicTest_SumAboutZ(Pair.Value, Params.Terms);
        if (FMath::Abs(Now->Z - Predicted) > 1e-6)
        {
            AddError(FString::Printf(TEXT("vertex %d: z %.9g predicted, %.9g measured"),
                Pair.Key, Predicted, Now->Z));
            return false;
        }
        // Axial displacement is along the axis and nowhere else.
        TestEqual(TEXT("an axial deform does not move a vertex in x"), Now->X, Pair.Value.X);
        TestEqual(TEXT("an axial deform does not move a vertex in y"), Now->Y, Pair.Value.Y);
        ++Checked;
    }

    TestTrue(TEXT("the sphere had off-axis vertices to check"), Checked > 100);
    return true;
}

// ============================================================================
// (b) Winding survives the modifier
//
// This is the whole point of the exercise. Unreal's facing normal is
// (V2-V0) x (V1-V0) - the NEGATION of the right-hand rule, because Unreal is left-handed -
// and GeometryUtils::MeasureMeshHealth measures its signed volume in that same convention, so
// "positive" here means "facing outward" with nothing left to convert.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwHarmonicClosedShapeStaysWoundOutwardTest,
    "PinWright.Model.HarmonicDeform.ClosedShapeStaysWoundOutward",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwHarmonicClosedShapeStaysWoundOutwardTest::RunTest(const FString& Parameters)
{
    // Both targets and both extremes of the radial domain in one sweep. The last row sits at
    // 0.98 - just inside the ceiling - because the amplitude nearest the refusal is where a fold
    // would appear first.
    struct FCase
    {
        const TCHAR* Label;
        GeometryOps::EHarmonicTarget Target;
        int32 OrderA;
        double AmplitudeA;
        int32 OrderB;
        double AmplitudeB;
    };
    const FCase Cases[] =
    {
        { TEXT("radial, orders 3+5 sum"),          GeometryOps::EHarmonicTarget::Radial, 3, 0.20, 5, 0.11 },
        { TEXT("radial, single order 2"),          GeometryOps::EHarmonicTarget::Radial, 2, 0.18, 0, 0.0 },
        // 8 uu, not 20: an axial displacement's gradient is worst NEAREST THE AXIS, where the
        // spacing between adjacent vertices shrinks toward zero while the displacement stays
        // absolute. 8 keeps the per-step shift under the pole-adjacent ring's own spacing, so
        // this row exercises the op rather than that limitation - which the op header records.
        { TEXT("axial rim drop, order 2"),         GeometryOps::EHarmonicTarget::Axial,  2, 8.0, 0, 0.0 },
        { TEXT("radial, just inside the ceiling"), GeometryOps::EHarmonicTarget::Radial, 3, 0.98, 0, 0.0 },
    };

    for (const FCase& Case : Cases)
    {
        UDynamicMesh* Mesh = PwHarmonicTest_NewSphere();

        // The premise: the base is closed and wound outward BEFORE the op runs. Without it the
        // test could pass on a mesh that was never healthy, which proves nothing about the op.
        const GeometryUtils::FMeshHealth Base = GeometryUtils::MeasureMeshHealth(Mesh);
        if (!TestTrue(*FString::Printf(TEXT("[%s] the base sphere is closed"), Case.Label),
                Base.IsClosed()))
        {
            return false;
        }
        if (!TestTrue(*FString::Printf(TEXT("[%s] the base sphere is wound outward (%.6g)"),
                Case.Label, Base.SignedVolume), Base.SignedVolume > 0.0))
        {
            return false;
        }

        GeometryOps::FHarmonicDeformParams Params;
        Params.Target = Case.Target;
        Params.Terms.Add(PwHarmonicTest_Term(Case.OrderA, Case.AmplitudeA, 30.0));
        if (Case.OrderB > 0)
        {
            Params.Terms.Add(PwHarmonicTest_Term(Case.OrderB, Case.AmplitudeB, 30.0));
        }

        const GeometryOps::FOpResult Op = GeometryOps::HarmonicDeform(Mesh, Params);
        if (!TestTrue(*FString::Printf(TEXT("[%s] the deform succeeds. %s: %s"),
                Case.Label, *Op.ErrorCode, *Op.ErrorMessage), Op.bSuccess))
        {
            continue;
        }
        TestTrue(*FString::Printf(TEXT("[%s] the deform moved something"), Case.Label), Op.bChanged);

        const GeometryUtils::FMeshHealth After = GeometryUtils::MeasureMeshHealth(Mesh);

        // Closed first: a signed volume means nothing on an open mesh, so an op that tore the
        // shell open would make the volume assertion below meaningless rather than failing.
        TestTrue(*FString::Printf(TEXT("[%s] the shell is still closed (%d boundary edges)"),
            Case.Label, After.BoundaryEdges), After.IsClosed());

        // The two signals this whole feature exists to protect. orientationConsistent catches the
        // partly inverted case a signed volume averages away; the volume catches the uniform
        // inversion no adjacency check can see.
        TestTrue(*FString::Printf(TEXT("[%s] adjacent triangles still agree (%d inconsistent edges)"),
            Case.Label, After.InconsistentEdges), After.IsOrientationConsistent());
        TestTrue(*FString::Printf(TEXT("[%s] the shell is still wound outward (signedVolume %.6g)"),
            Case.Label, After.SignedVolume), After.SignedVolume > 0.0);
        TestFalse(*FString::Printf(TEXT("[%s] the shell did not invert"), Case.Label),
            After.IsInverted());

        // No new triangles, so the op cannot emit geometry into a material slot it never
        // resolved - the shape of B-pwmodel-modifier-output-takes-slot-zero, which is about
        // ADDITIVE modifiers (sweep, extrude_along_spline, bridge) and not about a vertex map.
        TestEqual(*FString::Printf(TEXT("[%s] the triangle count is untouched"), Case.Label),
            After.TriangleCount, Base.TriangleCount);
        TestEqual(*FString::Printf(TEXT("[%s] the vertex count is untouched"), Case.Label),
            After.VertexCount, Base.VertexCount);
    }
    return true;
}

// ============================================================================
// (c) Parameters are validated, not silently ignored
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwHarmonicBadOrderIsRefusedTest,
    "PinWright.Model.HarmonicDeform.OrderBelowOneIsRefusedByTheOp",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwHarmonicBadOrderIsRefusedTest::RunTest(const FString& Parameters)
{
    const int32 BadOrders[] = { 0, -3 };
    for (int32 Order : BadOrders)
    {
        UDynamicMesh* Mesh = PwHarmonicTest_NewSphere();
        const TMap<int32, FVector3d> Before = PwHarmonicTest_Snapshot(Mesh);

        GeometryOps::FHarmonicDeformParams Params;
        Params.Terms.Add(PwHarmonicTest_Term(Order, 0.2, 0.0));

        const GeometryOps::FOpResult Op = GeometryOps::HarmonicDeform(Mesh, Params);
        TestFalse(*FString::Printf(TEXT("order %d is refused"), Order), Op.bSuccess);
        TestEqual(*FString::Printf(TEXT("order %d refuses with INVALID_ARGUMENT"), Order),
            Op.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));

        // A refusal that has already moved half the mesh is worse than no refusal: the author
        // gets an error AND a changed mesh. The check runs before any vertex is touched.
        const TMap<int32, FVector3d> After = PwHarmonicTest_Snapshot(Mesh);
        for (const TPair<int32, FVector3d>& Pair : Before)
        {
            const FVector3d* Now = After.Find(Pair.Key);
            if (!Now || !Now->Equals(Pair.Value, UE_DOUBLE_KINDA_SMALL_NUMBER))
            {
                AddError(FString::Printf(TEXT("order %d refused but moved vertex %d"), Order, Pair.Key));
                return false;
            }
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwHarmonicFoldingAmplitudeIsRefusedTest,
    "PinWright.Model.HarmonicDeform.RadialAmplitudesThatWouldFoldAreRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwHarmonicFoldingAmplitudeIsRefusedTest::RunTest(const FString& Parameters)
{
    // Two terms of 0.6 total 1.2. Neither alone would fold the surface, which is exactly why the
    // check is on the SUM: a per-term bound would let this through.
    UDynamicMesh* Mesh = PwHarmonicTest_NewSphere();
    GeometryOps::FHarmonicDeformParams Params;
    Params.Terms.Add(PwHarmonicTest_Term(3, 0.6, 0.0));
    Params.Terms.Add(PwHarmonicTest_Term(5, 0.6, 0.0));

    const GeometryOps::FOpResult Radial = GeometryOps::HarmonicDeform(Mesh, Params);
    TestFalse(TEXT("a radial amplitude total of 1.2 is refused"), Radial.bSuccess);
    TestEqual(TEXT("it refuses with INVALID_ARGUMENT"),
        Radial.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    TestTrue(*FString::Printf(TEXT("the message names the total it measured: %s"),
        *Radial.ErrorMessage), Radial.ErrorMessage.Contains(TEXT("1.2")));

    // The SAME amplitudes are legal on the axial target, because there they are absolute
    // displacements rather than a factor on the radius - so the ceiling is a property of the
    // radial map, not of the numbers. A bound applied to both targets is a bug this catches.
    UDynamicMesh* AxialMesh = PwHarmonicTest_NewSphere();
    Params.Target = GeometryOps::EHarmonicTarget::Axial;
    const GeometryOps::FOpResult Axial = GeometryOps::HarmonicDeform(AxialMesh, Params);
    TestTrue(*FString::Printf(TEXT("the same amplitudes are legal on the axial target. %s: %s"),
        *Axial.ErrorCode, *Axial.ErrorMessage), Axial.bSuccess);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwHarmonicUnderSampledOrderWarnsTest,
    "PinWright.Model.HarmonicDeform.UnderSampledOrderWarns",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwHarmonicUnderSampledOrderWarnsTest::RunTest(const FString& Parameters)
{
    // A coarse sphere: 8 azimuth steps, so the Nyquist bound is order 4. Order 6 aliases - the
    // surface gets a different wave from the one requested, and the triangles between two badly
    // separated samples can cross. It is the one way a within-bounds radial request still
    // damages the mesh, so it reports itself.
    UDynamicMesh* Coarse = NewObject<UDynamicMesh>(GetTransientPackage());
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSphereLatLong(
        Coarse, Options, FTransform::Identity, 100.0f, 8, 8,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);

    GeometryOps::FHarmonicDeformParams Params;
    Params.Terms.Add(PwHarmonicTest_Term(6, 0.15, 0.0));

    const GeometryOps::FOpResult Op = GeometryOps::HarmonicDeform(Coarse, Params);
    TestTrue(*FString::Printf(TEXT("the deform still runs. %s: %s"),
        *Op.ErrorCode, *Op.ErrorMessage), Op.bSuccess);

    bool bWarned = false;
    for (const FString& Warning : Op.Warnings)
    {
        bWarned |= Warning.Contains(TEXT("Nyquist"));
    }
    TestTrue(*FString::Printf(TEXT("an under-sampled order warns. Warnings: %s"),
        *FString::Join(Op.Warnings, TEXT(" | "))), bWarned);

    // The same order on a mesh that CAN carry it is silent. Without this the test would pass on
    // an op that warned unconditionally, which is a warning nobody reads.
    UDynamicMesh* Fine = PwHarmonicTest_NewSphere();
    const GeometryOps::FOpResult FineOp = GeometryOps::HarmonicDeform(Fine, Params);
    bool bFineWarned = false;
    for (const FString& Warning : FineOp.Warnings)
    {
        bFineWarned |= Warning.Contains(TEXT("Nyquist"));
    }
    TestFalse(TEXT("a well-sampled order does not warn"), bFineWarned);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwHarmonicEmptyTermsReportsItselfTest,
    "PinWright.Model.HarmonicDeform.EmptyTermListReportsItself",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwHarmonicEmptyTermsReportsItselfTest::RunTest(const FString& Parameters)
{
    UDynamicMesh* Mesh = PwHarmonicTest_NewSphere();
    const GeometryOps::FOpResult Op =
        GeometryOps::HarmonicDeform(Mesh, GeometryOps::FHarmonicDeformParams());

    TestTrue(TEXT("an empty term list is not an error"), Op.bSuccess);
    TestFalse(TEXT("an empty term list changes nothing"), Op.bChanged);

    bool bWarned = false;
    for (const FString& Warning : Op.Warnings)
    {
        bWarned |= Warning.Contains(TEXT("terms is empty"));
    }
    TestTrue(*FString::Printf(TEXT("an empty term list says so. Warnings: %s"),
        *FString::Join(Op.Warnings, TEXT(" | "))), bWarned);
    return true;
}

// ============================================================================
// Document level: the op is reachable, and a malformed term never reaches it
//
// An op is only usable if the parser PUBLISHES its parameters and the compiler routes them. A
// parameter the compiler reads but the table does not declare is unreachable in the opposite
// direction - the parser rejects the document with PWSRC_UNKNOWN_PARAM before the compiler
// ever sees it - and an op-level test still passes, because it calls the op directly.
// ============================================================================

namespace
{
// The diagnostic codes, spelled as LITERALS rather than through PwModelDiagnosticCodes, on the
// TestPwModelOrientationSignals precedent: what the wire carries is the contract under test, and
// a shared constant would make this file compile against a tree whose code had been renamed.
const TCHAR* const PwHarmonicTest_BadValue = TEXT("PWSRC_BAD_VALUE");
const TCHAR* const PwHarmonicTest_BadArity = TEXT("PWSRC_BAD_TUPLE_ARITY");
const TCHAR* const PwHarmonicTest_MissingParam = TEXT("PWSRC_MISSING_PARAM");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwHarmonicDocumentKeepsItsWindingTest,
    "PinWright.Model.HarmonicDeform.CompiledDocumentKeepsItsWinding",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwHarmonicDocumentKeepsItsWindingTest::RunTest(const FString& Parameters)
{
    // A `sphere` base, whose winding comes from the compiler rather than from the author. That is
    // the whole contrast this op is being added for: the two models it will rebuild are inverted
    // precisely because they are the only two whose winding was hand-rolled.
    const TCHAR* const Source =
        TEXT("pwmodel 0\n")
        TEXT("part crown {\n")
        TEXT("    sphere radius=100 subdivisions=24\n")
        TEXT("    harmonic_deform terms=[(3, 0.2, 30), (5, 0.11, 30)]\n")
        TEXT("}\n");

    const FPwHarmonicTest_Run Run = PwHarmonicTest_Validate(Source);
    if (!TestTrue(*FString::Printf(TEXT("the document validates. %s"),
            *PwHarmonicTest_Describe(Run)), Run.bSuccess))
    {
        return false;
    }

    const TSharedPtr<FJsonObject>* Health = nullptr;
    if (!TestTrue(TEXT("the run reports a health block"),
            Run.Result.IsValid() && Run.Result->TryGetObjectField(TEXT("health"), Health)
                && (*Health).IsValid()))
    {
        return false;
    }

    // TryGet rather than GetNumberField / GetBoolField: a missing field answers 0 and false, and
    // 0 is a legal signed volume - so a defaulting read turns "the signal does not exist" into
    // "the signal says nothing is wrong", which is the exact failure this family guards against.
    double SignedVolume = 0.0;
    bool bConsistent = false;
    bool bClosed = false;
    TestTrue(TEXT("health carries signedVolume"),
        (*Health)->TryGetNumberField(TEXT("signedVolume"), SignedVolume));
    TestTrue(TEXT("health carries orientationConsistent"),
        (*Health)->TryGetBoolField(TEXT("orientationConsistent"), bConsistent));
    TestTrue(TEXT("health carries isClosed"),
        (*Health)->TryGetBoolField(TEXT("isClosed"), bClosed));

    TestTrue(*FString::Printf(TEXT("the deformed sphere is still closed. %s"),
        *PwHarmonicTest_Describe(Run)), bClosed);
    TestTrue(*FString::Printf(TEXT("adjacent triangles still agree. %s"),
        *PwHarmonicTest_Describe(Run)), bConsistent);
    TestTrue(*FString::Printf(TEXT("the mesh is wound outward (signedVolume %.6g). %s"),
        SignedVolume, *PwHarmonicTest_Describe(Run)), SignedVolume > 0.0);

    // The op adds no geometry, so it allocates no material slot and cannot land untagged
    // triangles on another part's slot - the shape of B-pwmodel-modifier-output-takes-slot-zero,
    // which is about ADDITIVE modifiers (sweep, extrude_along_spline, bridge). Asserted rather
    // than argued, because "this one is fine" is exactly what was said about the others.
    const FPwHarmonicTest_Run Undeformed = PwHarmonicTest_Validate(
        TEXT("pwmodel 0\npart crown {\n    sphere radius=100 subdivisions=24\n}\n"));
    if (TestTrue(*FString::Printf(TEXT("the undeformed control validates. %s"),
            *PwHarmonicTest_Describe(Undeformed)), Undeformed.bSuccess))
    {
        TestEqual(TEXT("the deform adds no triangle"),
            Run.Result->GetNumberField(TEXT("meshTriangleCount")),
            Undeformed.Result->GetNumberField(TEXT("meshTriangleCount")));
        TestEqual(TEXT("the deform allocates no material slot"),
            Run.Result->GetNumberField(TEXT("materialSlots")),
            Undeformed.Result->GetNumberField(TEXT("materialSlots")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwHarmonicMalformedTermsRejectedTest,
    "PinWright.Model.HarmonicDeform.MalformedTermsAreRejectedByTheParser",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwHarmonicMalformedTermsRejectedTest::RunTest(const FString& Parameters)
{
    // The wire spelling, asserted directly on the frame_list precedent: model.describe_ops
    // publishes it verbatim and docs/pwmodel-format.md's value-kind table names it, so a rename
    // that updated neither would leave an author reading a type that does not exist.
    TestEqual(TEXT("the harmonic list type spells itself harmonic_list on the wire"),
        FString(PwModelParamTypeToString(EPwModelParamType::HarmonicList)),
        FString(TEXT("harmonic_list")));

    struct FCase
    {
        const TCHAR* Label;
        const TCHAR* Terms;
        const TCHAR* ExpectedCode;
    };
    const FCase Cases[] =
    {
        // A fractional order cannot be made safe by shrinking the amplitude: the wave does not
        // close on itself after a full turn, so the surface tears at the wrap however small it is.
        { TEXT("fractional order"), TEXT("terms=[(2.5, 0.2, 0)]"),  PwHarmonicTest_BadValue },
        { TEXT("order zero"),       TEXT("terms=[(0, 0.2, 0)]"),    PwHarmonicTest_BadValue },
        { TEXT("negative order"),   TEXT("terms=[(-3, 0.2, 0)]"),   PwHarmonicTest_BadValue },
        // One bad entry among good ones is still reported: a check that stopped at the first
        // entry, or looked only at the first, would pass this.
        { TEXT("one bad entry in a good list"),
          TEXT("terms=[(3, 0.2, 0), (1.5, 0.1, 0)]"),               PwHarmonicTest_BadValue },
        // Arity, which is the type's own check rather than the order domain.
        { TEXT("two components"),   TEXT("terms=[(3, 0.2)]"),       PwHarmonicTest_BadArity },
        { TEXT("four components"),  TEXT("terms=[(3, 0.2, 0, 1)]"), PwHarmonicTest_BadArity },
        // A bare tuple where a list of tuples is required.
        { TEXT("a tuple, not a list"), TEXT("terms=(3, 0.2, 0)"),   PwHarmonicTest_BadValue },
    };

    for (const FCase& Case : Cases)
    {
        const FString Source = FString::Printf(
            TEXT("pwmodel 0\npart p {\n    sphere radius=50\n    harmonic_deform %s\n}\n"),
            Case.Terms);

        const FPwHarmonicTest_Run Run = PwHarmonicTest_Validate(*Source);
        TestFalse(*FString::Printf(TEXT("[%s] is rejected. %s"),
            Case.Label, *PwHarmonicTest_Describe(Run)), Run.bSuccess);
        TestTrue(*FString::Printf(TEXT("[%s] reports %s. %s"),
            Case.Label, Case.ExpectedCode, *PwHarmonicTest_Describe(Run)),
            PwHarmonicTest_HasCode(Run, Case.ExpectedCode));
    }

    // The arity message must name the ORDERING, for the reason frame_list's does: three numbers
    // whose components are cycles, a fraction and degrees cannot be corrected from "expects 3
    // components" alone. That hint is the only thing distinguishing this diagnostic from
    // point_list3's, and it is what makes the type worth naming.
    const FPwHarmonicTest_Run ShortEntry = PwHarmonicTest_Validate(
        TEXT("pwmodel 0\npart p {\n    sphere radius=50\n    harmonic_deform terms=[(3, 0.2)]\n}\n"));
    TestTrue(*FString::Printf(TEXT("the arity message names the components. %s"),
        *PwHarmonicTest_Describe(ShortEntry)),
        PwHarmonicTest_Describe(ShortEntry).Contains(TEXT("(order, amplitude, phase)")));

    // `terms` is required, so omitting it is a named diagnostic rather than a silent no-op.
    const FPwHarmonicTest_Run Missing = PwHarmonicTest_Validate(
        TEXT("pwmodel 0\npart p {\n    sphere radius=50\n    harmonic_deform axis=z\n}\n"));
    TestFalse(*FString::Printf(TEXT("harmonic_deform with no terms is rejected. %s"),
        *PwHarmonicTest_Describe(Missing)), Missing.bSuccess);
    TestTrue(*FString::Printf(TEXT("it reports the missing parameter. %s"),
        *PwHarmonicTest_Describe(Missing)),
        PwHarmonicTest_HasCode(Missing, PwHarmonicTest_MissingParam));

    // The control: the same document with a legal term list compiles, with every parameter set.
    // Without it every row above would be satisfied by an op that rejects everything, including
    // what it should accept - and the four parameter names would go unasserted.
    const FPwHarmonicTest_Run Good = PwHarmonicTest_Validate(
        TEXT("pwmodel 0\npart p {\n    sphere radius=50 subdivisions=24\n")
        TEXT("    harmonic_deform terms=[(3, 0.2, 30)] axis=z target=radial center=(0, 0, 0)\n}\n"));
    TestTrue(*FString::Printf(TEXT("a legal harmonic_deform compiles. %s"),
        *PwHarmonicTest_Describe(Good)), Good.bSuccess);
    return true;
}
