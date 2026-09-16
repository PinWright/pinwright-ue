// Copyright (c) 2026 Alexander Penkin. MIT License.

// The acceptance test for landscape.audit_shape is SEPARATION, not a number: the metric has to
// score cell-aligned stamped terrain high and organically sculpted terrain low. The retired
// Python gate this ports the idea of (docs/tools/check_terrain.py, rule AXIS-LOCKED, in the host
// project) read 0.937 / 0.871 on its stamped fixtures and 0.060 / 0.025 on its traced ones.
//
// The fixtures are synthetic and built here, so the test needs no host content and no ALandscape:
//
//   StampedPlateaus  - rejected terrain #1, axis-aligned rectangular plateaus. Nested rectangles,
//                      one constant height per ring.
//   StampedPerCell   - rejected terrain #2, one tier value per heightfield cell from a radial
//                      region mask. Its boundary is a rasterised circle, so it is deliberately
//                      NOT axis-locked at stride 8. It is here to prove the stated blind spot is
//                      real AND that stepped_profile covers it.
//   Organic          - a sculpted surface: a smooth dome under a diagonal tilt with a low-
//                      amplitude ripple. The gradient never reaches zero, so no delta is an
//                      isolated riser.
//
// The stride-1 test is the guard against the failure this whole verb exists for. Measured per
// segment, all three fixtures score exactly 1.000 - a statistic that cannot fail, which is what
// on_step_fraction:0.918 was when it passed 90-degree blocks as green. Asserting the degenerate
// reading is how the stride stays justified rather than merely claimed.
#include "Misc/AutomationTest.h"
#include "Handlers/Environment/LandscapeShapeMetrics.h"

namespace PinWrightLandscapeShapeFixtures
{
    constexpr int32 N = 128;
    constexpr int32 Base = 32768;

    void StampedPlateaus(TArray<uint16>& Out)
    {
        Out.SetNumUninitialized(N * N);
        for (int32 Y = 0; Y < N; ++Y)
        {
            for (int32 X = 0; X < N; ++X)
            {
                const int32 Ring = FMath::Min(FMath::Min(X, Y), FMath::Min(N - 1 - X, N - 1 - Y)) / 12;
                Out[Y * N + X] = static_cast<uint16>(Base + Ring * 400);
            }
        }
    }

    void StampedPerCell(TArray<uint16>& Out)
    {
        Out.SetNumUninitialized(N * N);
        const double C = (N - 1) / 2.0;
        for (int32 Y = 0; Y < N; ++Y)
        {
            for (int32 X = 0; X < N; ++X)
            {
                const double R = FMath::Sqrt(FMath::Square(X - C) + FMath::Square(Y - C));
                Out[Y * N + X] = static_cast<uint16>(Base + FMath::FloorToInt32(R / 9.0) * 400);
            }
        }
    }

    void Organic(TArray<uint16>& Out)
    {
        Out.SetNumUninitialized(N * N);
        for (int32 Y = 0; Y < N; ++Y)
        {
            for (int32 X = 0; X < N; ++X)
            {
                const double V =
                      900.0 * FMath::Sin(UE_PI * X / (N - 1)) * FMath::Sin(UE_PI * Y / (N - 1))
                    + 6.0 * X + 3.5 * Y
                    + 120.0 * FMath::Sin(2.0 * UE_PI * X / 53.0)
                            * FMath::Sin(2.0 * UE_PI * Y / 61.0);
                Out[Y * N + X] = static_cast<uint16>(Base + FMath::RoundToInt32(V));
            }
        }
    }

    LandscapeShape::FMeasurement Run(const TArray<uint16>& Heights, int32 Stride)
    {
        LandscapeShape::FParams Params;
        Params.Stride = Stride;
        LandscapeShape::FMeasurement M;
        LandscapeShape::Measure(Heights, N, N, Params, M);
        return M;
    }
}

// ============================================================================
// The acceptance test: the metric must tell the two rejected terrains apart
// from a sculpted one. Measured on these fixtures: plateaus 0.886, per-cell
// 0.055, organic 0.086 for axisFraction; 1.000 / 1.000 / 0.000 for stepFraction.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeShapeSeparationTest,
    "PinWright.landscape.audit_shape.MetricSeparatesStampedFromOrganic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLandscapeShapeSeparationTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightLandscapeShapeFixtures;

    TArray<uint16> Plateaus, PerCell, Smooth;
    StampedPlateaus(Plateaus);
    StampedPerCell(PerCell);
    Organic(Smooth);

    const LandscapeShape::FMeasurement MP = Run(Plateaus, 8);
    const LandscapeShape::FMeasurement MC = Run(PerCell, 8);
    const LandscapeShape::FMeasurement MO = Run(Smooth, 8);

    // Every fixture must actually be measurable, or the comparisons below compare nothing.
    if (!MP.AxisFraction.IsSet() || !MC.AxisFraction.IsSet() || !MO.AxisFraction.IsSet()
        || !MP.StepFraction.IsSet() || !MC.StepFraction.IsSet() || !MO.StepFraction.IsSet())
    {
        AddError(TEXT("A fixture produced no measurement, so the separation was not tested."));
        return false;
    }
    AddInfo(FString::Printf(
        TEXT("axisFraction: plateaus %.3f (%d chords), perCell %.3f (%d), organic %.3f (%d) | "
             "stepFraction: %.3f / %.3f / %.3f"),
        MP.AxisFraction.GetValue(), MP.Chords, MC.AxisFraction.GetValue(), MC.Chords,
        MO.AxisFraction.GetValue(), MO.Chords,
        MP.StepFraction.GetValue(), MC.StepFraction.GetValue(), MO.StepFraction.GetValue()));

    // ---- axisFraction: rectangular plateaus high, sculpted low ----
    TestTrue(FString::Printf(TEXT("axis-aligned rectangular plateaus score high (%.3f >= 0.85)"),
        MP.AxisFraction.GetValue()), MP.AxisFraction.GetValue() >= 0.85);
    TestTrue(FString::Printf(TEXT("a sculpted surface scores low (%.3f <= 0.25)"),
        MO.AxisFraction.GetValue()), MO.AxisFraction.GetValue() <= 0.25);
    TestTrue(FString::Printf(TEXT("the two are separated by a real gap (%.3f >= 0.50)"),
        MP.AxisFraction.GetValue() - MO.AxisFraction.GetValue()),
        (MP.AxisFraction.GetValue() - MO.AxisFraction.GetValue()) >= 0.50);

    // ---- the stated blind spot, asserted rather than only documented ----
    // A staircase whose risers are shorter than the stride reads LOW on axisFraction. If this
    // ever starts scoring high the limits[] text in the response has gone stale.
    TestTrue(FString::Printf(
        TEXT("a per-cell staircase is INVISIBLE to axisFraction at stride 8 (%.3f <= 0.25) - the "
             "documented blind spot"), MC.AxisFraction.GetValue()),
        MC.AxisFraction.GetValue() <= 0.25);

    // ---- stepFraction: both stamped terrains high, sculpted zero ----
    TestTrue(FString::Printf(TEXT("plateaus carry their rise in isolated risers (%.3f >= 0.95)"),
        MP.StepFraction.GetValue()), MP.StepFraction.GetValue() >= 0.95);
    TestTrue(FString::Printf(
        TEXT("the per-cell staircase axisFraction cannot see IS caught here (%.3f >= 0.95)"),
        MC.StepFraction.GetValue()), MC.StepFraction.GetValue() >= 0.95);
    TestTrue(FString::Printf(TEXT("a sculpted slope has no isolated risers (%.3f <= 0.05)"),
        MO.StepFraction.GetValue()), MO.StepFraction.GetValue() <= 0.05);

    // ---- the verdicts, through Evaluate, at the shipped defaults ----
    LandscapeShape::FParams Defaults;
    Defaults.Stride = 8;
    const uint32 All = LandscapeShape::DefaultCheckMask();
    TArray<LandscapeShape::FFinding> FP, FC, FO;
    LandscapeShape::Evaluate(MP, Defaults, All, FP);
    LandscapeShape::Evaluate(MC, Defaults, All, FC);
    LandscapeShape::Evaluate(MO, Defaults, All, FO);
    TestTrue(TEXT("rectangular plateaus are flagged"), FP.Num() > 0);
    TestTrue(TEXT("the per-cell staircase is flagged"), FC.Num() > 0);
    TestEqual(TEXT("the sculpted surface produces no finding at all"), FO.Num(), 0);
    for (const LandscapeShape::FFinding& F : FP)
    {
        TestEqual(TEXT("a plateau finding is Flagged, not Unrunnable"),
            static_cast<int32>(F.Status), static_cast<int32>(LandscapeShape::EFindingStatus::Flagged));
    }
    return true;
}

// ============================================================================
// Why the chords are measured at a stride. Per segment, EVERY fixture reads
// exactly 1.000 - a traced curve and a stamped mask are indistinguishable,
// because every boundary segment of a raster is a unit step on the cell
// lattice. That is a metric that cannot fail, which is precisely what the
// measurement this replaces was.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeShapePerSegmentTest,
    "PinWright.landscape.audit_shape.PerSegmentReadingCannotFail",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLandscapeShapePerSegmentTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightLandscapeShapeFixtures;
    TArray<uint16> Plateaus, PerCell, Smooth;
    StampedPlateaus(Plateaus);
    StampedPerCell(PerCell);
    Organic(Smooth);

    const TArray<uint16>* Cases[] = { &Plateaus, &PerCell, &Smooth };
    const TCHAR* Names[] = { TEXT("plateaus"), TEXT("perCell"), TEXT("organic") };
    for (int32 I = 0; I < 3; ++I)
    {
        const LandscapeShape::FMeasurement M = Run(*Cases[I], 1);
        if (!M.AxisFraction.IsSet())
        {
            AddError(FString::Printf(TEXT("%s produced no per-segment reading"), Names[I]));
            continue;
        }
        TestEqual(FString::Printf(
            TEXT("%s reads 1.000 per segment, so per-segment cannot discriminate"), Names[I]),
            M.AxisFraction.GetValue(), 1.0, 1.0e-9);
    }
    return true;
}

// ============================================================================
// Unmeasurable is a real answer, never a pass. A flat region has no boundary
// and no rise; both checks must report Unrunnable rather than 0.000, which is
// an answer over an empty denominator.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeShapeUnmeasurableTest,
    "PinWright.landscape.audit_shape.UnmeasurableIsNotClean",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLandscapeShapeUnmeasurableTest::RunTest(const FString& Parameters)
{
    TArray<uint16> Flat;
    Flat.Init(32768, 64 * 64);
    LandscapeShape::FParams Params;
    LandscapeShape::FMeasurement M;
    LandscapeShape::Measure(Flat, 64, 64, Params, M);

    TestFalse(TEXT("a flat region yields no axisFraction at all"), M.AxisFraction.IsSet());
    TestFalse(TEXT("a flat region yields no stepFraction at all"), M.StepFraction.IsSet());

    TArray<LandscapeShape::FFinding> Findings;
    LandscapeShape::Evaluate(M, Params, LandscapeShape::DefaultCheckMask(), Findings);
    TestEqual(TEXT("both checks report on a flat region"), Findings.Num(), 2);
    for (const LandscapeShape::FFinding& F : Findings)
    {
        TestEqual(TEXT("the report is Unrunnable, not Flagged and not silence"),
            static_cast<int32>(F.Status),
            static_cast<int32>(LandscapeShape::EFindingStatus::Unrunnable));
        TestFalse(TEXT("the Unrunnable row names a reason code"), F.Code.IsEmpty());
    }

    // A region smaller than one chord is the other way a reading can be vacuous.
    TArray<uint16> Tiny;
    Tiny.SetNumUninitialized(4 * 4);
    for (int32 I = 0; I < 16; ++I) { Tiny[I] = static_cast<uint16>(32768 + (I % 2) * 500); }
    LandscapeShape::FMeasurement MT;
    LandscapeShape::Measure(Tiny, 4, 4, Params, MT);
    TArray<LandscapeShape::FFinding> TinyFindings;
    LandscapeShape::Evaluate(MT, Params, LandscapeShape::CheckBit(LandscapeShape::ECheck::AxisLocked),
                             TinyFindings);
    TestEqual(TEXT("a 4x4 region at stride 8 reports one row"), TinyFindings.Num(), 1);
    if (TinyFindings.Num() == 1)
    {
        TestEqual(TEXT("and that row is Unrunnable, not a clean pass"),
            static_cast<int32>(TinyFindings[0].Status),
            static_cast<int32>(LandscapeShape::EFindingStatus::Unrunnable));
    }
    return true;
}

// ============================================================================
// An unknown check id must be rejected, not skipped: a typo that silently ran
// nothing is indistinguishable from terrain that passed every check.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeShapeCheckIdTest,
    "PinWright.landscape.audit_shape.UnknownCheckIdIsRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLandscapeShapeCheckIdTest::RunTest(const FString& Parameters)
{
    LandscapeShape::ECheck Check = LandscapeShape::ECheck::Count;
    TestFalse(TEXT("an unknown id is refused"),
        LandscapeShape::ParseCheckId(TEXT("axis_lockd"), Check));
    TestFalse(TEXT("an empty id is refused"), LandscapeShape::ParseCheckId(TEXT(""), Check));
    TestTrue(TEXT("axis_locked parses"), LandscapeShape::ParseCheckId(TEXT("axis_locked"), Check));
    TestEqual(TEXT("to the right check"), static_cast<int32>(Check),
        static_cast<int32>(LandscapeShape::ECheck::AxisLocked));
    TestTrue(TEXT("stepped_profile parses, case-insensitively"),
        LandscapeShape::ParseCheckId(TEXT("STEPPED_PROFILE"), Check));
    TestEqual(TEXT("to the right check"), static_cast<int32>(Check),
        static_cast<int32>(LandscapeShape::ECheck::SteppedProfile));

    TestEqual(TEXT("every ECheck has a table row"),
        LandscapeShape::AllChecks().Num(), LandscapeShape::CheckCount);
    for (const LandscapeShape::FCheckInfo& Info : LandscapeShape::AllChecks())
    {
        // A row out of ECheck order would make CheckInfo() return a different check's code.
        TestEqual(TEXT("the table is in ECheck order"),
            static_cast<int32>(LandscapeShape::CheckInfo(Info.Check).Check),
            static_cast<int32>(Info.Check));
        TestTrue(TEXT("every check ships on by default"), Info.bDefaultOn);
    }
    return true;
}
