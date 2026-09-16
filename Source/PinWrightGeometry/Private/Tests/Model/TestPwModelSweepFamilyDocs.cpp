// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the PUBLISHED TEXT of the sweep family's op-table entries.
//
// The defect these guard is a documentation defect, not a geometry one, so the assertions are on
// the string. `extrude_along_spline`'s summary read "the cross-section is swept as a CLOSED loop"
// long after the op stopped doing that unconditionally (GeometryOps_Advanced.cpp:726-731 derives
// bLoop from the path), and `model.describe_ops` serialises FPwModelOpSpec::Description verbatim
// (ModelCompileHandler.cpp:615) - so the false sentence reached callers as generated,
// authoritative output. One author took it at its word and shipped a stray plank.
//
// Why here and not in Tests/Geometry: the sibling
// Tests/Geometry/TestGeometrySweepPathAndCapGuards.cpp already pins the BEHAVIOUR - open path
// caps, returning path loops once, the `cap` warning fires. Nothing there reads the op table, so
// the description could go back to claiming an unconditional loop with the whole geometry suite
// green. That gap is what this file closes.
//
// Assertions are on distinctive SUBSTRINGS, never the whole string: rewording the prose must stay
// free, while dropping the claim must fail. The one exception is the warning literal, which is
// asserted as a shared constant against the text the op really emits - a quoted warning that
// drifts from the emitted one is the same defect in a smaller font.
#include "Misc/AutomationTest.h"

#include "Handlers/Geometry/GeometryOps_Advanced.h"
#include "Model/PwModelAst.h"
#include "Model/PwModelDiagnostic.h"
#include "Model/PwModelParser.h"

#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper
// with a common name would collide with a sibling test TU once Unity merges them.

// The exact sentence GeometryOps_Advanced.cpp:740-742 adds to Warnings when `cap` is set on a
// path that returns to its start. The op-table entry quotes it, and the second test below proves
// the quote still matches what the op says.
const TCHAR* const SweepDocsTest_CapWarningLead =
    TEXT("cap has no effect on a path that returns to its start");

// The verbatim clause the false summary carried. Asserted ABSENT rather than asserting the new
// wording letter for letter: any return to an unconditional-loop claim has to write this again.
const TCHAR* const SweepDocsTest_FalseLoopClaim =
    TEXT("the cross-section is swept as a CLOSED loop");

const FPwModelOpSpec* SweepDocsTest_FindOp(FAutomationTestBase& Test, const TCHAR* OpName)
{
    const FPwModelOpSpec* Op = PwModelOpTable::Find(FString(OpName), EPwModelOpContext::Part);
    if (!Op)
    {
        Test.AddError(FString::Printf(TEXT("op '%s' is not in the part-context op table"), OpName));
    }
    return Op;
}

const FPwModelParamSpec* SweepDocsTest_FindParam(
    FAutomationTestBase& Test, const FPwModelOpSpec& Op, const TCHAR* ParamName)
{
    const FPwModelParamSpec* Param = Op.FindParam(FString(ParamName));
    if (!Param)
    {
        Test.AddError(FString::Printf(TEXT("'%s' publishes no parameter named '%s'"),
            *Op.Name, ParamName));
    }
    return Param;
}

UDynamicMesh* SweepDocsTest_NewMesh()
{
    return NewObject<UDynamicMesh>(GetTransientPackage());
}

// The same octagonal ring TestGeometrySweepPathAndCapGuards.cpp sweeps: nine frames, the ninth
// repeating the first, each carrying yaw = theta + 90 so its local +X is the ring's tangent.
TArray<FTransform> SweepDocsTest_ReturningPath()
{
    TArray<FTransform> Ring;
    for (int32 Index = 0; Index <= 8; ++Index)
    {
        const double Theta = 45.0 * Index;
        const double Radians = FMath::DegreesToRadians(Theta);
        Ring.Add(FTransform(
            FRotator(0.0, Theta + 90.0, 0.0).Quaternion(),
            FVector(100.0 * FMath::Cos(Radians), 100.0 * FMath::Sin(Radians), 0.0),
            FVector::OneVector));
    }
    return Ring;
}

GeometryOps::FSweepProfile SweepDocsTest_Profile()
{
    GeometryOps::FSweepProfile Profile;
    Profile.Vertices = { FVector2D(10.0, 0.0), FVector2D(10.0, 4.0),
                         FVector2D(-10.0, 4.0), FVector2D(-10.0, 0.0) };
    return Profile;
}
}

// ============================================================================
// The published summary states the path-derived rule, not an unconditional loop
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelExtrudeAlongSplineDocTest,
    "PinWright.Model.Parser.ExtrudeAlongSplineDocStatesPathDerivedLoop",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelExtrudeAlongSplineDocTest::RunTest(const FString& Parameters)
{
    const FPwModelOpSpec* Op = SweepDocsTest_FindOp(*this, TEXT("extrude_along_spline"));
    if (!Op)
    {
        return false;
    }

    const FString& Doc = Op->Description;

    // The defect itself. The op has not swept every path as a loop since bLoop stopped being
    // hardcoded, and this clause is what said otherwise.
    TestFalse(*FString::Printf(
            TEXT("extrude_along_spline no longer claims an unconditional closed loop. summary: %s"),
            *Doc),
        Doc.Contains(SweepDocsTest_FalseLoopClaim));

    // What replaced it: the shape is READ OFF THE PATH.
    TestTrue(*FString::Printf(TEXT("the summary says the loop is derived from the path. summary: %s"), *Doc),
        Doc.Contains(TEXT("READ OFF THE PATH")));

    // The measurable form of that rule, which is the half an author can act on: a path is closed
    // when its last frame comes back within 0.01 uu of the first
    // (GeometryOpsAdvanced_PathCloseTolerance, GeometryOps_Advanced.cpp:213).
    TestTrue(*FString::Printf(TEXT("the summary gives the 0.01 uu close tolerance. summary: %s"), *Doc),
        Doc.Contains(TEXT("0.01 uu")));

    // And that the other branch exists at all - an entry that only described loops would be the
    // same defect stated in the opposite direction.
    TestTrue(*FString::Printf(TEXT("the summary says every other path is swept open. summary: %s"), *Doc),
        Doc.Contains(TEXT("swept OPEN")));

    // The `cap` consequence, quoted from the warning the op emits rather than paraphrased.
    TestTrue(*FString::Printf(TEXT("the summary quotes the cap warning. summary: %s"), *Doc),
        Doc.Contains(SweepDocsTest_CapWarningLead));

    // The `cap` parameter's OWN description carried the same silence - "Cap both ends." says
    // nothing about the path length at which it stops meaning anything - and it is the string a
    // caller reading one parameter sees.
    if (const FPwModelParamSpec* Cap = SweepDocsTest_FindParam(*this, *Op, TEXT("cap")))
    {
        TestTrue(*FString::Printf(TEXT("cap's own description names the open-path condition. cap: %s"),
                *Cap->Description),
            Cap->Description.Contains(TEXT("OPEN path")));
        TestTrue(*FString::Printf(TEXT("cap's own description says a returning path loops. cap: %s"),
                *Cap->Description),
            Cap->Description.Contains(TEXT("returns to its start")));
    }

    // `sweep` never loops - GeometryOps_Advanced.cpp:657 passes bLoop false unconditionally - so
    // its entry must not inherit the neighbour's condition by implication.
    if (const FPwModelOpSpec* SweepOp = SweepDocsTest_FindOp(*this, TEXT("sweep")))
    {
        if (const FPwModelParamSpec* Cap = SweepDocsTest_FindParam(*this, *SweepOp, TEXT("cap")))
        {
            TestTrue(*FString::Printf(TEXT("sweep's cap says sweep never loops. cap: %s"),
                    *Cap->Description),
                Cap->Description.Contains(TEXT("never loops")));
        }
    }

    return true;
}

// ============================================================================
// The published text agrees with what the op does
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelSweepFamilyDocBehaviourTest,
    "PinWright.Model.Parser.SweepFamilyDocsMatchTheOpBehaviour",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelSweepFamilyDocBehaviourTest::RunTest(const FString& Parameters)
{
    // 1. The published default is the one the op runs on. `cap` is the parameter the summary now
    //    makes claims about, so a table default that drifted from FExtrudeAlongSplineParams would
    //    make every one of those claims describe a different run than the author gets.
    const GeometryOps::FExtrudeAlongSplineParams Defaults;
    const GeometryOps::FSweepParams SweepDefaults;

    if (const FPwModelOpSpec* Op = SweepDocsTest_FindOp(*this, TEXT("extrude_along_spline")))
    {
        if (const FPwModelParamSpec* Cap = SweepDocsTest_FindParam(*this, *Op, TEXT("cap")))
        {
            TestEqual(TEXT("extrude_along_spline publishes the cap default the op holds"),
                Cap->Default, Defaults.bCap ? FString(TEXT("true")) : FString(TEXT("false")));
        }
    }
    if (const FPwModelOpSpec* Op = SweepDocsTest_FindOp(*this, TEXT("sweep")))
    {
        if (const FPwModelParamSpec* Cap = SweepDocsTest_FindParam(*this, *Op, TEXT("cap")))
        {
            TestEqual(TEXT("sweep publishes the cap default the op holds"),
                Cap->Default, SweepDefaults.bCap ? FString(TEXT("true")) : FString(TEXT("false")));
        }
    }

    // 2. The quoted warning is the emitted warning. The summary puts this sentence in backticks,
    //    which reads as a literal quotation; if the op reworded it, the quotation would be a
    //    fabrication an author could search the logs for and never find.
    TStrongObjectPtr<UDynamicMesh> Mesh(SweepDocsTest_NewMesh());

    GeometryOps::FExtrudeAlongSplineParams Params;
    Params.Profile = SweepDocsTest_Profile();
    Params.bCap = true;

    const GeometryOps::FOpResult Result =
        GeometryOps::ExtrudeAlongSpline(Mesh.Get(), Params, SweepDocsTest_ReturningPath());

    TestTrue(TEXT("the returning path sweeps"), Result.bSuccess);

    bool bFound = false;
    for (const FString& Warning : Result.Warnings)
    {
        if (Warning.StartsWith(SweepDocsTest_CapWarningLead))
        {
            bFound = true;
            break;
        }
    }
    TestTrue(*FString::Printf(
            TEXT("the op emits the warning the op table quotes ('%s'). warnings: %s"),
            SweepDocsTest_CapWarningLead, *FString::Join(Result.Warnings, TEXT(" | "))),
        bFound);

    return true;
}
