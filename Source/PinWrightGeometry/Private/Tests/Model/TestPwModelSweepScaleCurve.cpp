// Copyright (c) 2026 Alexander Penkin. MIT License.

// `scales=` on sweep / extrude_along_spline: the cross-section scale as a LAW along the path.
//
// WHAT THE PARAMETER IS FOR. Both ops build one FTransform per path frame and had only
// Lerp(ScaleStart, ScaleEnd, alpha) to put in its scale slot, so a swept tube could taper only
// STRAIGHT. A power-law taper, an exponential flare, a waist - every radius law that is not a
// straight line - was unreachable along a path, and the documented way round it was to split the
// shape into one sweep per span and match the radii at each join by hand. The engine never
// required that: the scale slot has always been per-frame, and only the caller's two scalars
// were not.
//
// Three layers, because they fail independently:
//
//  - The LAW itself, evaluated in isolation. A piecewise-linear curve is small enough that its
//    every arm can be asserted against a closed form, and one of those arms - holding rather
//    than extrapolating past the outermost knots - is the arm that keeps a curve stopping short
//    of alpha 1 from producing a NEGATIVE scale, which reflects the cross-section and reverses
//    the tube's facing normals while leaving every count and health field identical.
//  - The GEOMETRY, against a transient UDynamicMesh. This is where the tube is measured: a
//    waisted curve has to pinch the section where it says it does, and the two-knot curve that
//    restates scale_start / scale_end has to reproduce the scalar form exactly, because that
//    equivalence is the claim that the new parameter is a superset and not a second dialect.
//  - The DOCUMENT, through the real dispatcher. The op is reachable only if the parser
//    publishes the parameter and the compiler routes it; each is invisible to the other's test.
#include "Misc/AutomationTest.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Geometry/GeometryOps_Advanced.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include "Dispatch/RpcDispatcher.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/Infra/DispatcherTestHelpers.h"

#include "Model/PwModelAst.h"
#include "Model/PwModelDiagnostic.h"
#include "Model/PwModelParser.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper
// with a common name would collide with a sibling test TU once Unity merges them.

UDynamicMesh* SweepScaleTest_NewMesh()
{
    return NewObject<UDynamicMesh>(GetTransientPackage());
}

// A square of half-width 10 in the frame's local Y-Z plane. Square rather than circular so the
// half-width is an exact number a test can predict: at scale s the section reaches exactly
// 10 * s from the path, with no polygon-inscribed-in-a-circle factor to carry through.
GeometryOps::FSweepProfile SweepScaleTest_Profile()
{
    GeometryOps::FSweepProfile Profile;
    Profile.Vertices = { FVector2D(10.0, -10.0), FVector2D(10.0, 10.0),
                         FVector2D(-10.0, 10.0), FVector2D(-10.0, -10.0) };
    return Profile;
}

// A straight path along +X with `Count` evenly spaced frames, each frame's local +X along the
// path - which is what the ops sweep along, and what a frame left at rotation (0, 0, 0) on a
// path running up Z famously is not.
TArray<FTransform> SweepScaleTest_StraightPath(int32 Count, double Length)
{
    TArray<FTransform> Path;
    for (int32 Index = 0; Index < Count; ++Index)
    {
        const double Alpha = (double)Index / (double)(Count - 1);
        Path.Add(FTransform(FRotator::ZeroRotator, FVector(Length * Alpha, 0.0, 0.0)));
    }
    return Path;
}

// The section's reach in Y among the vertices standing at X == AtX. This is the measurement the
// whole geometry layer of this file rests on: it reads the cross-section the op actually built
// at one station, which a bounding box - the union over every station - cannot.
double SweepScaleTest_HalfWidthAt(UDynamicMesh* Mesh, double AtX)
{
    double Reach = 0.0;
    Mesh->ProcessMesh([AtX, &Reach](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        for (int32 VertexID : ReadMesh.VertexIndicesItr())
        {
            const FVector3d Position = ReadMesh.GetVertex(VertexID);
            if (FMath::Abs(Position.X - AtX) < 0.5)
            {
                Reach = FMath::Max(Reach, FMath::Abs(Position.Y));
            }
        }
    });
    return Reach;
}

GeometryOps::FSweepScaleCurve SweepScaleTest_Curve(std::initializer_list<FVector2D> Knots)
{
    GeometryOps::FSweepScaleCurve Curve;
    Curve.Knots = Knots;
    return Curve;
}

// One model.validate run with the diagnostic shaping defaults off: the defaults (limit 5,
// collapsed) can drop or fold the one diagnostic a test is looking for.
struct FSweepScaleTest_Run
{
    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
};

FSweepScaleTest_Run SweepScaleTest_Validate(const TCHAR* Source)
{
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::FSinkPtr Sink;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("text"), Source);
    Payload->SetNumberField(TEXT("diagnosticLimit"), 0);
    Payload->SetBoolField(TEXT("collapseDiagnostics"), false);

    FSweepScaleTest_Run Run;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("model.validate"), TEXT("t-scales"),
        Payload, Run.bSuccess, Run.Result, Run.ErrorCode);
    return Run;
}

// Every failure message carries the whole diagnostic list; "expected true, got false" on a
// compiler test costs a rerun under a debugger to learn anything at all.
FString SweepScaleTest_Describe(const FSweepScaleTest_Run& Run)
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

// True when SOME diagnostic carries this code and its message contains Fragment. Both halves
// matter: the code alone would pass on any op failure in the document, and the fragment alone
// would pass on a warning that merely mentions the parameter.
bool SweepScaleTest_Refused(const FSweepScaleTest_Run& Run, const TCHAR* Fragment)
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
        if (!Value.IsValid() || !Value->TryGetObject(Entry) || !(*Entry).IsValid())
        {
            continue;
        }
        if ((*Entry)->GetStringField(TEXT("code")) == PwModelDiagnosticCodes::PWMODEL_OP_FAILED
            && (*Entry)->GetStringField(TEXT("message")).Contains(Fragment))
        {
            return true;
        }
    }
    return false;
}

const FPwModelParamSpec* SweepScaleTest_FindParam(
    FAutomationTestBase& Test, const TCHAR* OpName, const TCHAR* ParamName)
{
    const FPwModelOpSpec* Op = PwModelOpTable::Find(FString(OpName), EPwModelOpContext::Part);
    if (!Op)
    {
        Test.AddError(FString::Printf(TEXT("op '%s' is not in the part-context op table"), OpName));
        return nullptr;
    }
    const FPwModelParamSpec* Param = Op->FindParam(FString(ParamName));
    if (!Param)
    {
        Test.AddError(FString::Printf(TEXT("'%s' publishes no parameter named '%s'"),
            OpName, ParamName));
    }
    return Param;
}
}

// ============================================================================
// (a) The law itself
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSweepScaleCurveEvaluatesPiecewiseLinearTest,
    "PinWright.Model.SweepScaleCurve.EvaluatesPiecewiseLinearAndHoldsPastItsEnds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSweepScaleCurveEvaluatesPiecewiseLinearTest::RunTest(const FString& Parameters)
{
    // Deliberately does NOT start at 0 or reach 1: holding outside the outermost knots is the
    // arm being asserted, and a curve spanning the whole [0, 1] never exercises it.
    const GeometryOps::FSweepScaleCurve Curve = SweepScaleTest_Curve(
        { FVector2D(0.2, 1.0), FVector2D(0.6, 0.5), FVector2D(0.8, 0.2) });

    TestTrue(TEXT("three knots is a curve"), Curve.IsSet());

    // Exact at every knot.
    TestEqual(TEXT("first knot"), Curve.Evaluate(0.2), 1.0, 1e-9);
    TestEqual(TEXT("middle knot"), Curve.Evaluate(0.6), 0.5, 1e-9);
    TestEqual(TEXT("last knot"), Curve.Evaluate(0.8), 0.2, 1e-9);

    // Linear inside each span, and the two spans have different slopes - which is the whole
    // point, and what a single Lerp between the endpoints could not produce. A straight ramp
    // from (0.2, 1.0) to (0.8, 0.2) would read 0.6666… at alpha 0.45, not 0.6875.
    TestEqual(TEXT("midway through the first span"), Curve.Evaluate(0.4), 0.75, 1e-9);
    TestEqual(TEXT("midway through the second span"), Curve.Evaluate(0.7), 0.35, 1e-9);
    TestEqual(TEXT("a quarter through the first span"), Curve.Evaluate(0.3), 0.875, 1e-9);

    // HELD, not extrapolated. Extrapolating the second span's slope to alpha 1.0 would give
    // -0.10 - a reflected cross-section, and a swept tube whose facing normals are reversed at
    // one end while isClosed, boundaryEdges and the triangle count all stay identical.
    TestEqual(TEXT("before the first knot"), Curve.Evaluate(0.0), 1.0, 1e-9);
    TestEqual(TEXT("after the last knot"), Curve.Evaluate(1.0), 0.2, 1e-9);
    TestTrue(TEXT("no reachable alpha produces a non-positive scale"),
        Curve.Evaluate(0.0) > 0.0 && Curve.Evaluate(1.0) > 0.0 && Curve.Evaluate(0.5) > 0.0);

    // An unset curve answers with the identity rather than a zero-scale collapse, so a caller
    // that forgets IsSet() gets an unscaled section instead of a degenerate one.
    GeometryOps::FSweepScaleCurve Unset;
    TestFalse(TEXT("an empty curve is not set"), Unset.IsSet());
    TestEqual(TEXT("an unset curve is the identity"), Unset.Evaluate(0.5), 1.0, 1e-9);

    GeometryOps::FSweepScaleCurve OneKnot = SweepScaleTest_Curve({ FVector2D(0.5, 0.25) });
    TestFalse(TEXT("one knot is not a law"), OneKnot.IsSet());
    TestEqual(TEXT("one knot still answers with the identity"), OneKnot.Evaluate(0.5), 1.0, 1e-9);

    return true;
}

// ============================================================================
// (b) The geometry: the tube is the shape the law describes
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSweepScaleCurvePinchesTheSectionTest,
    "PinWright.Model.SweepScaleCurve.SweepHonoursTheLawAtEveryFrame",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSweepScaleCurvePinchesTheSectionTest::RunTest(const FString& Parameters)
{
    // Five frames at x = 0, 50, 100, 150, 200, so alpha 0.5 lands ON a frame and the assertion
    // is against a section the op really built rather than one interpolated between two.
    const TArray<FTransform> Path = SweepScaleTest_StraightPath(5, 200.0);

    TStrongObjectPtr<UDynamicMesh> Waisted(SweepScaleTest_NewMesh());
    GeometryOps::FSweepParams WaistParams;
    WaistParams.Profile = SweepScaleTest_Profile();
    WaistParams.bCap = true;
    // Both ends full size, pinched to a quarter in the middle. A linear ramp cannot describe
    // this at all: no pair of endpoint scalars produces a non-monotonic radius.
    WaistParams.ScaleCurve = SweepScaleTest_Curve(
        { FVector2D(0.0, 1.0), FVector2D(0.5, 0.25), FVector2D(1.0, 1.0) });

    GeometryOps::FSweepOutputs WaistOut;
    const GeometryOps::FOpResult WaistResult =
        GeometryOps::Sweep(Waisted.Get(), WaistParams, GeometryOps::FSweepPath{ Path, 0.0f, false },
                           WaistOut);
    TestTrue(TEXT("a curved sweep succeeds"), WaistResult.bSuccess);

    TestEqual(TEXT("the first frame's section is full size"),
        SweepScaleTest_HalfWidthAt(Waisted.Get(), 0.0), 10.0, 1e-3);
    TestEqual(TEXT("the middle frame's section is a quarter"),
        SweepScaleTest_HalfWidthAt(Waisted.Get(), 100.0), 2.5, 1e-3);
    TestEqual(TEXT("the last frame's section is full size again"),
        SweepScaleTest_HalfWidthAt(Waisted.Get(), 200.0), 10.0, 1e-3);
    // Halfway through the first span, which pins the INTERPOLATION and not just the knots.
    TestEqual(TEXT("the section between two knots is on the line between them"),
        SweepScaleTest_HalfWidthAt(Waisted.Get(), 50.0), 6.25, 1e-3);

    // The tube still closes. A scale law moves vertices and creates none, so a curve must not
    // be able to open a shell that the scalar form closes.
    Waisted->ProcessMesh([this](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        TestTrue(TEXT("a curved sweep is still a closed shell"), ReadMesh.IsClosed());
    });

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSweepScaleCurveRestatesTheScalarsTest,
    "PinWright.Model.SweepScaleCurve.TwoKnotsReproduceScaleStartAndScaleEndExactly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSweepScaleCurveRestatesTheScalarsTest::RunTest(const FString& Parameters)
{
    // The equivalence that makes `scales` a SUPERSET rather than a second dialect: the two-knot
    // curve [(0, a), (1, b)] must be the same geometry as scale_start=a scale_end=b, vertex for
    // vertex. If it is not, the two spellings are two behaviours and the parameter is a trap.
    const TArray<FTransform> Path = SweepScaleTest_StraightPath(5, 200.0);

    TStrongObjectPtr<UDynamicMesh> Scalars(SweepScaleTest_NewMesh());
    GeometryOps::FSweepParams ScalarParams;
    ScalarParams.Profile = SweepScaleTest_Profile();
    ScalarParams.bCap = true;
    ScalarParams.ScaleStart = 1.0;
    ScalarParams.ScaleEnd = 0.3;

    GeometryOps::FSweepOutputs ScalarOut;
    const GeometryOps::FOpResult ScalarResult = GeometryOps::Sweep(
        Scalars.Get(), ScalarParams, GeometryOps::FSweepPath{ Path, 0.0f, false }, ScalarOut);
    TestTrue(TEXT("the scalar sweep succeeds"), ScalarResult.bSuccess);

    TStrongObjectPtr<UDynamicMesh> Curved(SweepScaleTest_NewMesh());
    GeometryOps::FSweepParams CurveParams;
    CurveParams.Profile = SweepScaleTest_Profile();
    CurveParams.bCap = true;
    // Left at their defaults on purpose: the curve has to win on its own, not by agreeing with
    // scalars that were also set to the same numbers.
    CurveParams.ScaleCurve = SweepScaleTest_Curve({ FVector2D(0.0, 1.0), FVector2D(1.0, 0.3) });

    GeometryOps::FSweepOutputs CurveOut;
    const GeometryOps::FOpResult CurveResult = GeometryOps::Sweep(
        Curved.Get(), CurveParams, GeometryOps::FSweepPath{ Path, 0.0f, false }, CurveOut);
    TestTrue(TEXT("the curved sweep succeeds"), CurveResult.bSuccess);

    TestEqual(TEXT("same triangle count"), Curved->GetTriangleCount(), Scalars->GetTriangleCount());

    for (const double AtX : { 0.0, 50.0, 100.0, 150.0, 200.0 })
    {
        TestEqual(*FString::Printf(TEXT("same section half-width at x = %g"), AtX),
            SweepScaleTest_HalfWidthAt(Curved.Get(), AtX),
            SweepScaleTest_HalfWidthAt(Scalars.Get(), AtX), 1e-6);
    }

    return true;
}

// ============================================================================
// (c) The document: published, routed, and refusing what it cannot honour
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSweepScaleCurveIsPublishedOnBothOpsTest,
    "PinWright.Model.SweepScaleCurve.BothPathOpsPublishScalesAsAPointList",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSweepScaleCurveIsPublishedOnBothOpsTest::RunTest(const FString& Parameters)
{
    // A compiler that reads a parameter the op table does not declare is unreachable: the parser
    // rejects the document before the compiler ever sees it. This is the half of that pair that
    // a geometry test cannot see.
    for (const TCHAR* OpName : { TEXT("sweep"), TEXT("extrude_along_spline") })
    {
        const FPwModelParamSpec* Param = SweepScaleTest_FindParam(*this, OpName, TEXT("scales"));
        if (!Param)
        {
            continue;
        }
        TestEqual(*FString::Printf(TEXT("%s's scales is a point list"), OpName),
            (int32)Param->Type, (int32)EPwModelParamType::PointList2);
        TestFalse(*FString::Printf(TEXT("%s's scales is optional"), OpName), Param->bRequired);

        // The description has to say what alpha MEANS. A caller who reads it as a distance in
        // uu writes knots the sweep never reaches, and every one of them is refused at a line
        // whose message then has to teach what the parameter doc should have.
        TestTrue(*FString::Printf(TEXT("%s's scales description defines alpha"), OpName),
            Param->Description.Contains(TEXT("normalised")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSweepScaleCurveDocumentRoundTripTest,
    "PinWright.Model.SweepScaleCurve.ADocumentUsingScalesCompilesClean",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSweepScaleCurveDocumentRoundTripTest::RunTest(const FString& Parameters)
{
    const FSweepScaleTest_Run Run = SweepScaleTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part tapered {\n")
        TEXT("    box size=(4, 4, 4) at=(0, 0, -400)\n")
        TEXT("    sweep profile=[(10, -10), (10, 10), (-10, 10), (-10, -10)] ")
        TEXT("path=[(0, 0, 0, 0, 0, 0), (100, 0, 0, 0, 0, 0), (200, 0, 0, 0, 0, 0)] ")
        TEXT("scales=[(0, 1), (0.5, 0.25), (1, 0.6)] cap=true\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("a document using scales validates. %s"),
        *SweepScaleTest_Describe(Run)), Run.bSuccess);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSweepScaleCurveRefusalsTest,
    "PinWright.Model.SweepScaleCurve.RefusesTheLawsItCannotHonour",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSweepScaleCurveRefusalsTest::RunTest(const FString& Parameters)
{
    struct FCase
    {
        const TCHAR* Label;
        const TCHAR* Scales;
        const TCHAR* Extra;
        const TCHAR* Fragment;
    };

    // Each row is a shape whose REPAIR would be a guess about which half the author meant, which
    // is why every one of them refuses instead of clamping. The fragment asserted is the part of
    // the message that names the offence, never the whole sentence: rewording must stay free.
    const FCase Cases[] = {
        { TEXT("scales beside scale_start"), TEXT("scales=[(0, 1), (1, 0.3)]"),
          TEXT("scale_start=1 "), TEXT("only one can be") },
        { TEXT("scales beside scale_end"), TEXT("scales=[(0, 1), (1, 0.3)]"),
          TEXT("scale_end=0.3 "), TEXT("only one can be") },
        { TEXT("a single knot"), TEXT("scales=[(0.5, 0.4)]"), TEXT(""),
          TEXT("at least 2 knots") },
        { TEXT("alpha above 1"), TEXT("scales=[(0, 1), (1.5, 0.3)]"), TEXT(""),
          TEXT("outside [0, 1]") },
        { TEXT("alpha below 0"), TEXT("scales=[(-0.2, 1), (1, 0.3)]"), TEXT(""),
          TEXT("outside [0, 1]") },
        { TEXT("alpha not ascending"), TEXT("scales=[(0, 1), (0.6, 0.5), (0.4, 0.3)]"), TEXT(""),
          TEXT("does not come after") },
        { TEXT("two knots at one alpha"), TEXT("scales=[(0, 1), (0.5, 0.5), (0.5, 0.2)]"), TEXT(""),
          TEXT("does not come after") },
        { TEXT("a zero scale"), TEXT("scales=[(0, 1), (1, 0)]"), TEXT(""),
          TEXT("must be positive") },
        { TEXT("a negative scale"), TEXT("scales=[(0, 1), (1, -0.5)]"), TEXT(""),
          TEXT("must be positive") },
    };

    for (const FCase& Case : Cases)
    {
        const FString Source = FString::Printf(
            TEXT("pwmodel 0\n")
            TEXT("part bad {\n")
            TEXT("    box size=(4, 4, 4) at=(0, 0, -400)\n")
            TEXT("    sweep profile=[(10, -10), (10, 10), (-10, 10), (-10, -10)] ")
            TEXT("path=[(0, 0, 0, 0, 0, 0), (200, 0, 0, 0, 0, 0)] %s%s cap=true\n")
            TEXT("}\n"), Case.Extra, Case.Scales);

        const FSweepScaleTest_Run Run = SweepScaleTest_Validate(*Source);

        TestFalse(*FString::Printf(TEXT("%s is refused. %s"), Case.Label,
            *SweepScaleTest_Describe(Run)), Run.bSuccess);
        TestTrue(*FString::Printf(TEXT("%s is refused for the stated reason. %s"), Case.Label,
            *SweepScaleTest_Describe(Run)), SweepScaleTest_Refused(Run, Case.Fragment));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSweepScaleCurveDroppedOnALoopIsReportedTest,
    "PinWright.Model.SweepScaleCurve.ExtrudeAlongSplineSaysWhenALoopDropsTheLaw",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSweepScaleCurveDroppedOnALoopIsReportedTest::RunTest(const FString& Parameters)
{
    // The engine's loop branch gates path scaling off entirely, so a scale law on a returning
    // path is discarded. The existing `cap` warning does not cover it - that one fires only when
    // `cap` was ALSO set - so an author who wrote a radius law and no cap would get a uniform
    // tube with nothing said. That is the failure mode this op has already been through once,
    // on `cap` itself, which is why it gets its own warning rather than a doc sentence.
    TArray<FTransform> Ring;
    for (int32 Index = 0; Index <= 8; ++Index)
    {
        const double Theta = 45.0 * Index;
        const double Radians = FMath::DegreesToRadians(Theta);
        Ring.Add(FTransform(FRotator(0.0, Theta + 90.0, 0.0),
            FVector(100.0 * FMath::Cos(Radians), 100.0 * FMath::Sin(Radians), 0.0)));
    }

    TStrongObjectPtr<UDynamicMesh> Mesh(SweepScaleTest_NewMesh());

    GeometryOps::FExtrudeAlongSplineParams Params;
    Params.Profile = SweepScaleTest_Profile();
    Params.bCap = false;   // so the cap warning cannot stand in for the one being asserted
    Params.ScaleCurve = SweepScaleTest_Curve({ FVector2D(0.0, 1.0), FVector2D(1.0, 0.3) });

    const GeometryOps::FOpResult Result =
        GeometryOps::ExtrudeAlongSpline(Mesh.Get(), Params, Ring);

    TestTrue(TEXT("a closing path with a scale law still succeeds"), Result.bSuccess);

    bool bSaidSo = false;
    for (const FString& Warning : Result.Warnings)
    {
        bSaidSo = bSaidSo || Warning.StartsWith(TEXT("scales is dropped"));
    }
    TestTrue(*FString::Printf(TEXT("the dropped law is reported. warnings: %s"),
        *FString::Join(Result.Warnings, TEXT(" | "))), bSaidSo);

    return true;
}
