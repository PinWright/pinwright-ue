// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the landscape.sculpt path brush and its reporting honesty.
//
// The defect these exist for: two terrains were rejected on sight for 90-degree
// staircased outlines. The diagnosis was "a per-cell mask instead of a curve", but
// landscape.sculpt imposed the SAME lattice — `CenterX = FMath::RoundToInt(LocalPos.X)`
// snapped every brush centre to a heightfield vertex before the falloff was evaluated, so
// a traced curve was quantised to the grid whichever verb drew it. Alongside it sat a set
// of reporting defects of the class rpc-design.md §1 calls the worst in the plugin:
// `modifiedVertices` was `HeightData.Num()`, the AREA OF THE CLAMPED RECTANGLE, so a stamp
// that moved nothing reported the same number as one that worked; and an unrecognised
// `toolMode` matched no branch, left the delta at zero, and returned success.
//
// Counterfactuals, one per test, so each of these breaks if the fix is reverted:
//
//  * SubCellCentreIsNotRounded — restore the `RoundToInt` and the reported fractional
//    centre becomes an integer, failing the first assertion. The snapToVertex branch keeps
//    passing, which is the point: the rounding is now a caller's choice, not a fact.
//  * PolylineBoundaryFollowsTheCurve — the swept stroke's boundary is the exact offset line
//    of the stroke (max deviation 0.00000 cells) and slides by exactly the sub-cell amount
//    the stroke slides. The same path drawn as lattice-snapped stamps — the pre-`path`
//    form, reconstructed inside the test — bends by 0.40 cells and mis-tracks a 0.37-cell
//    shift by 0.63 on its worst row. Both are measured, so the assertion has a negative
//    control on the same metric rather than a threshold chosen to pass.
//    NOTE: this test first shipped with an integer left-edge scan that could NOT fail —
//    the snapped chain scored 0.920 on it against the swept stroke's 0.893, i.e. better —
//    and with an axis-aligned "control" whose left silhouette is a circular end cap, so it
//    scored 0.667 against an unreachable `< 0.35`. Both are documented at the metric.
//  * ModifiedVertexCountIsMeasured — restore `HeightData.Num()` and `modifiedVertices`
//    equals `verticesConsidered`, failing the "not the rectangle area" assertion; restore
//    the zero-delta fall-through and the no-op case stops reporting 0.
//  * RejectsUnknownToolMode — restore the string-compare chain with no else and the verb
//    answers success instead of LANDSCAPE_INVALID_TOOL_MODE.
//  * FalloffProfilesAreDistinct — collapse the profiles back to one ramp and the four
//    curves stop differing at t=0.25, and smoothstep stops lagging/leading linear either
//    side of the midpoint. This test first shipped asserting that smoothstep sits BELOW
//    linear at what it called t=0.25; the distance it passed was in fact t=0.75, where
//    smoothstep is above. The implementation was correct and the expectation was not.
//
// Fixtures are built in-code through the production landscape.create handler and wrapped
// in FScopedEditorWorldActorGuard: GetAllLevelActors filters RF_Transient, so a transient
// probe would be invisible to the verb under test and the failure would read as a verb
// defect. A landscape that fails to create is a FAILURE, never a skip.

#include "Misc/AutomationTest.h"
#include "Templates/Function.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Landscape.h"
#include "Handlers/Environment/LandscapeBrush.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Tests/TestSkipReporting.h"

namespace
{
    using namespace PinWright::LandscapeBrush;

    // Creates a 1x1 component, 63-quad edit-layer landscape (64x64 heightfield vertices)
    // through the production landscape.create handler and returns it. Big enough that a
    // default-radius brush placed at its centre never touches the clamped edge, which
    // matters for the "modifiedVertices is not the rectangle area" assertion.
    ALandscape* CreateSculptFixture(FAutomationTestBase& Test, UWorld* World, const FString& Label)
    {
        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("name"), Label);
        CreatePayload->SetNumberField(TEXT("componentsX"), 1);
        CreatePayload->SetNumberField(TEXT("componentsY"), 1);
        CreatePayload->SetNumberField(TEXT("quadsPerComponent"), 63);
        CreatePayload->SetNumberField(TEXT("sectionsPerComponent"), 1);

        TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
        if (!InvokeHandlerWithSharedCapture(TEXT("landscape.create"), CreatePayload, Capture))
        {
            Test.AddError(TEXT("landscape.create handler is not registered"));
            return nullptr;
        }
        PumpUntilCaptured(*Capture, /*TimeoutSeconds=*/30.0);
        // The landscape IS the fixture: a create that did not succeed is a failure.
        Test.TestTrue(TEXT("landscape.create fixture succeeded"), Capture->bSuccess);
        if (!Capture->bSuccess)
        {
            return nullptr;
        }

        for (TActorIterator<ALandscape> It(World); It; ++It)
        {
            if (It->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase))
            {
                return *It;
            }
        }
        Test.AddError(TEXT("landscape.create reported success but no actor with that label exists"));
        return nullptr;
    }

    // Runs landscape.sculpt and returns the capture. Async: the handler completes inside an
    // AsyncTask on the game thread, so the shared capture + pump form is mandatory.
    TSharedRef<FTestResponseCapture> RunSculpt(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Payload)
    {
        TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
        if (!InvokeHandlerWithSharedCapture(TEXT("landscape.sculpt"), Payload, Capture))
        {
            Test.AddError(TEXT("landscape.sculpt handler is not registered"));
            return Capture;
        }
        PumpUntilCaptured(*Capture, /*TimeoutSeconds=*/60.0);
        return Capture;
    }

    double GetNumber(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, double Fallback = -1.0)
    {
        double Out = Fallback;
        if (Obj.IsValid())
        {
            Obj->TryGetNumberField(Field, Out);
        }
        return Out;
    }

    // ---- Sub-cell boundary measurement ---------------------------------------------
    //
    // These replace the integer left-edge scan this file shipped with. That metric took the
    // leftmost COVERED INTEGER COLUMN on each row and scored how often it changed between
    // adjacent rows, and it could not fail in the defect direction: measured on this exact
    // fixture, the sub-cell swept diagonal scores 0.893 and the SAME path drawn as a chain
    // of lattice-snapped stamps -- the pre-`path` behaviour this verb exists to remove --
    // scores 0.920. The broken form scored HIGHER than the fixed one. That is rpc-design.md
    // section 16's failure shape exactly: a real number, correctly computed, measuring the
    // wrong quantity. Thresholding coverage at integer sample points throws away precisely
    // the sub-cell information the fix adds.
    //
    // Its axis-aligned "control" was wrong for a second, independent reason. The LEFT
    // silhouette of a horizontal capsule is its circular END CAP, not a cell edge, so it
    // advances on 12 of its 18 rows -- 0.667 -- for any implementation whatsoever, and the
    // `< 0.35` it was asserted against was unreachable by construction.
    //
    // What follows measures the boundary itself, in fractions of a cell, by bisecting the
    // production distance field. Everything is computed from ClosestPointOnSegment +
    // EvaluateFalloff, so a reverted fix changes the number rather than leaving a private
    // re-implementation passing on its own (rpc-design.md section 12).

    const double NoCrossing = TNumericLimits<double>::Max();

    // Swept-polyline coverage: consecutive stroke vertices are segments, which is what the
    // handler sweeps. A one-vertex stroke is one zero-length segment -- the `location` form.
    bool StrokeCovers(const TArray<FStrokeVertex>& Stroke, double X, double Y,
        double RadiusUu, double ScaleX, double ScaleY)
    {
        const int32 SegmentCount = FMath::Max(1, Stroke.Num() - 1);
        for (int32 s = 0; s < SegmentCount; ++s)
        {
            const FStrokeVertex& A = Stroke[s];
            const FStrokeVertex& B = Stroke[FMath::Min(s + 1, Stroke.Num() - 1)];
            double Dist = 0.0, Z = 0.0;
            ClosestPointOnSegment(X, Y, A, B, ScaleX, ScaleY, Dist, Z);
            if (EvaluateFalloff(EFalloffProfile::Linear, Dist, RadiusUu, /*Falloff=*/0.0) > 0.0f)
            {
                return true;
            }
        }
        return false;
    }

    // Independent stamps: every point is its own zero-length segment with nothing swept
    // between them. This is the shape a caller was forced into before `path` existed -- N
    // separate landscape.sculpt calls, each centre rounded to a vertex -- and it is the
    // negative control the assertions below measure against, on the SAME metric, so both
    // failure directions are read off one number (rpc-design.md section 6).
    bool StampsCover(const TArray<FStrokeVertex>& Stamps, double X, double Y,
        double RadiusUu, double ScaleX, double ScaleY)
    {
        for (const FStrokeVertex& P : Stamps)
        {
            double Dist = 0.0, Z = 0.0;
            ClosestPointOnSegment(X, Y, P, P, ScaleX, ScaleY, Dist, Z);
            if (EvaluateFalloff(EFalloffProfile::Linear, Dist, RadiusUu, /*Falloff=*/0.0) > 0.0f)
            {
                return true;
            }
        }
        return false;
    }

    // Fractional local X at which coverage begins on row Y, to ~1e-9 of a cell. Returns
    // NoCrossing when the row is uncovered within [XMin, XMax], and also when XMin is
    // itself already covered -- a clipped band would return a number that is not the
    // boundary, and a wrong number is worse than none.
    double BoundaryCrossingX(TFunctionRef<bool(double, double)> Covers, double Y,
        double XMin, double XMax)
    {
        constexpr double CoarseStep = 0.25;
        const int32 Steps = FMath::CeilToInt32((XMax - XMin) / CoarseStep);
        double Inside = NoCrossing;
        for (int32 i = 0; i <= Steps; ++i)
        {
            const double X = XMin + (double)i * CoarseStep;
            if (Covers(X, Y))
            {
                Inside = X;
                break;
            }
        }
        if (Inside == NoCrossing)
        {
            return NoCrossing;
        }

        double Lo = Inside - CoarseStep;
        double Hi = Inside;
        if (Covers(Lo, Y))
        {
            return NoCrossing;
        }
        for (int32 i = 0; i < 60; ++i)
        {
            const double Mid = 0.5 * (Lo + Hi);
            if (Covers(Mid, Y)) { Hi = Mid; } else { Lo = Mid; }
        }
        return 0.5 * (Lo + Hi);
    }

    // Collects the boundary over an inclusive row band. Returns false -- never a number --
    // if any row in the band has no boundary, so a partial or empty denominator cannot
    // print a pass (rpc-design.md section 16).
    bool CollectBoundary(TFunctionRef<bool(double, double)> Covers,
        int32 YFirst, int32 YLast, double XMin, double XMax,
        TArray<double>& OutY, TArray<double>& OutX)
    {
        OutY.Reset();
        OutX.Reset();
        for (int32 Y = YFirst; Y <= YLast; ++Y)
        {
            const double Cross = BoundaryCrossingX(Covers, (double)Y, XMin, XMax);
            if (Cross == NoCrossing)
            {
                return false;
            }
            OutY.Add((double)Y);
            OutX.Add(Cross);
        }
        return OutY.Num() > 0;
    }

    // Largest distance, in cells, from the measured boundary to its own least-squares
    // straight line. The side of a single swept segment is an exact offset line, so this is
    // zero to floating point for a straight stroke and grows with every scallop.
    double MaxDeviationFromLine(const TArray<double>& Ys, const TArray<double>& Xs)
    {
        const int32 N = Ys.Num();
        if (N < 3)
        {
            return -1.0;
        }
        double MeanY = 0.0, MeanX = 0.0;
        for (int32 i = 0; i < N; ++i) { MeanY += Ys[i]; MeanX += Xs[i]; }
        MeanY /= (double)N;
        MeanX /= (double)N;

        double Sxy = 0.0, Syy = 0.0;
        for (int32 i = 0; i < N; ++i)
        {
            Sxy += (Ys[i] - MeanY) * (Xs[i] - MeanX);
            Syy += (Ys[i] - MeanY) * (Ys[i] - MeanY);
        }
        const double Slope = (Syy > 0.0) ? (Sxy / Syy) : 0.0;
        const double Intercept = MeanX - Slope * MeanY;

        double Worst = 0.0;
        for (int32 i = 0; i < N; ++i)
        {
            Worst = FMath::Max(Worst, FMath::Abs(Xs[i] - (Slope * Ys[i] + Intercept)));
        }
        return Worst;
    }

    // Resamples a stroke into independent stamps at a fixed local spacing, snapping every
    // centre with the RoundToInt the fix removed. The pre-`path` verb, reconstructed.
    TArray<FStrokeVertex> SnappedStampChain(const TArray<FStrokeVertex>& Stroke, double SpacingCells)
    {
        TArray<FStrokeVertex> Out;
        for (int32 s = 0; s + 1 < Stroke.Num(); ++s)
        {
            const FStrokeVertex& A = Stroke[s];
            const FStrokeVertex& B = Stroke[s + 1];
            const double Len = FMath::Sqrt(FMath::Square(B.X - A.X) + FMath::Square(B.Y - A.Y));
            const int32 Count = FMath::Max(1, FMath::RoundToInt(Len / SpacingCells));
            for (int32 i = 0; i <= Count; ++i)
            {
                const double T = (double)i / (double)Count;
                Out.Add(FStrokeVertex{
                    (double)FMath::RoundToInt(A.X + T * (B.X - A.X)),
                    (double)FMath::RoundToInt(A.Y + T * (B.Y - A.Y)),
                    0.0 });
            }
        }
        return Out;
    }
}

// ---- The brush centre is not rounded to the lattice --------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeSculptSubCellCentreTest,
    "PinWright.landscape.sculpt.SubCellCentreIsNotRounded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeSculptSubCellCentreTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("landscape.sculpt handler registered"), IsHandlerRegistered(TEXT("landscape.sculpt")));

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("SKIPPED (no editor world): PinWright.landscape.sculpt.SubCellCentreIsNotRounded ran ZERO of its substantive assertions."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = FString::Printf(TEXT("PW_SculptSubCell_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    ALandscape* Landscape = CreateSculptFixture(*this, World, Label);
    if (!Landscape)
    {
        return false;
    }

    // Pick the centre in LOCAL heightfield coordinates and transform it out to the world,
    // so the test is independent of where landscape.create put the actor and at what draw
    // scale. 30.5 / 24.25 are deliberately non-integral in both axes and a quarter-cell
    // apart in Y, which no rounding rule can preserve.
    const FVector LocalCentre(30.5, 24.25, 0.0);
    const FVector WorldCentre = Landscape->GetActorTransform().TransformPosition(LocalCentre);

    auto MakePayload = [&](bool bSnap)
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("landscapeName"), Label);
        TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
        Loc->SetNumberField(TEXT("x"), WorldCentre.X);
        Loc->SetNumberField(TEXT("y"), WorldCentre.Y);
        Loc->SetNumberField(TEXT("z"), WorldCentre.Z);
        P->SetObjectField(TEXT("location"), Loc);
        P->SetStringField(TEXT("toolMode"), TEXT("Raise"));
        P->SetNumberField(TEXT("brushRadius"), 800.0);
        P->SetNumberField(TEXT("strength"), 50.0);
        if (bSnap)
        {
            P->SetBoolField(TEXT("snapToVertex"), true);
        }
        return P;
    };

    // --- Default: sub-cell. The reported centre must still carry the fraction. ---
    {
        TSharedRef<FTestResponseCapture> Capture = RunSculpt(*this, MakePayload(/*bSnap=*/false));
        TestTrue(TEXT("sub-cell sculpt responded"), Capture->bWasCalled);
        TestTrue(TEXT("sub-cell sculpt succeeded"), Capture->bSuccess);
        if (!Capture->bSuccess)
        {
            return false;
        }

        bool bSnapped = true;
        Capture->Result->TryGetBoolField(TEXT("snappedToVertex"), bSnapped);
        TestFalse(TEXT("snappedToVertex is false by default"), bSnapped);

        const TSharedPtr<FJsonObject>* CentreObj = nullptr;
        const bool bHasCentre = Capture->Result->TryGetObjectField(TEXT("firstCentreVertexFractional"), CentreObj);
        TestTrue(TEXT("response reports the fractional centre it used"), bHasCentre && CentreObj);
        if (bHasCentre && CentreObj)
        {
            const double Cx = GetNumber(*CentreObj, TEXT("x"));
            const double Cy = GetNumber(*CentreObj, TEXT("y"));
            // The core regression assertion: RoundToInt would make both of these integral.
            TestTrue(FString::Printf(TEXT("centre X kept its sub-cell fraction (expected ~30.5, got %f)"), Cx),
                FMath::Abs(Cx - 30.5) < 0.01);
            TestTrue(FString::Printf(TEXT("centre Y kept its sub-cell fraction (expected ~24.25, got %f)"), Cy),
                FMath::Abs(Cy - 24.25) < 0.01);
            TestTrue(FString::Printf(TEXT("centre X is genuinely off-lattice (got %f)"), Cx),
                FMath::Abs(Cx - FMath::RoundToDouble(Cx)) > 0.05);
        }
    }

    // --- Opt-in rounding: the old behaviour, and the response says it happened. ---
    {
        TSharedRef<FTestResponseCapture> Capture = RunSculpt(*this, MakePayload(/*bSnap=*/true));
        TestTrue(TEXT("snapped sculpt responded"), Capture->bWasCalled);
        TestTrue(TEXT("snapped sculpt succeeded"), Capture->bSuccess);
        if (Capture->bSuccess)
        {
            bool bSnapped = false;
            Capture->Result->TryGetBoolField(TEXT("snappedToVertex"), bSnapped);
            TestTrue(TEXT("snappedToVertex is reported true when requested"), bSnapped);

            const TSharedPtr<FJsonObject>* CentreObj = nullptr;
            if (Capture->Result->TryGetObjectField(TEXT("firstCentreVertexFractional"), CentreObj) && CentreObj)
            {
                const double Cx = GetNumber(*CentreObj, TEXT("x"));
                const double Cy = GetNumber(*CentreObj, TEXT("y"));
                TestTrue(FString::Printf(TEXT("snapToVertex lands the centre on the lattice (%f, %f)"), Cx, Cy),
                    FMath::Abs(Cx - FMath::RoundToDouble(Cx)) < 1e-6 &&
                    FMath::Abs(Cy - FMath::RoundToDouble(Cy)) < 1e-6);
            }
        }
    }

    return true;
}

// ---- A polyline sweep leaves a boundary that follows the curve, not the cell edges --
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeSculptPolylineBoundaryTest,
    "PinWright.landscape.sculpt.PolylineBoundaryFollowsTheCurve",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeSculptPolylineBoundaryTest::RunTest(const FString& Parameters)
{
    // No world needed: this measures the production brush geometry directly, which is the
    // layer that decides the shape. The handler-level half lives in the two tests either
    // side of this one.
    constexpr double ScaleX = 100.0;
    constexpr double ScaleY = 100.0;
    // A hard-edged brush (falloff 0) so the measured boundary IS the offset curve of the
    // stroke rather than the outer edge of a ramp.
    constexpr double RadiusUu = 900.0;
    constexpr double XMin = -20.0;
    constexpr double XMax = 95.0;
    // Rows well inside the diagonal's span, so what is measured is the capsule's SIDE and
    // not either circular end cap. (Measuring the cap is what made the old axis-aligned
    // control score 0.667 against a threshold of 0.35 it could never reach.)
    constexpr int32 YFirst = 25;
    constexpr int32 YLast = 58;
    constexpr double ShiftCells = 0.37;

    TArray<FStrokeVertex> Diagonal;
    Diagonal.Add(FStrokeVertex{ 14.3, 12.7, 0.0 });
    Diagonal.Add(FStrokeVertex{ 81.6, 70.4, 0.0 });

    // --- 1. Swept in one call, the boundary is the exact offset line of the stroke. ---
    TArray<double> SweptY, SweptX;
    double SweptDeviation = -1.0;
    {
        auto Covers = [&](double X, double Y) { return StrokeCovers(Diagonal, X, Y, RadiusUu, ScaleX, ScaleY); };
        const bool bGot = CollectBoundary(Covers, YFirst, YLast, XMin, XMax, SweptY, SweptX);
        TestTrue(TEXT("the swept diagonal has a boundary on every row of the measured band"), bGot);
        if (!bGot)
        {
            return false;
        }
        SweptDeviation = MaxDeviationFromLine(SweptY, SweptX);
        TestTrue(FString::Printf(
            TEXT("swept stroke's boundary is a straight offset line (max deviation %.5f cells over %d rows, expected < 0.02)"),
            SweptDeviation, SweptY.Num()),
            SweptDeviation >= 0.0 && SweptDeviation < 0.02);
    }

    // --- 2. The negative control: the SAME path drawn the only way the verb allowed before
    //        `path` existed -- N independent stamps, each centre rounded to the lattice. One
    //        stamp per cell is the densest a caller could possibly go, so no sampling rate
    //        rescues it; the boundary still bends by a quarter of a cell or more.
    double StampedDeviation = -1.0;
    {
        const TArray<FStrokeVertex> Stamps = SnappedStampChain(Diagonal, /*SpacingCells=*/1.0);
        TestTrue(FString::Printf(TEXT("the stamped control has stamps to measure (got %d)"), Stamps.Num()),
            Stamps.Num() > 10);
        auto Covers = [&](double X, double Y) { return StampsCover(Stamps, X, Y, RadiusUu, ScaleX, ScaleY); };
        TArray<double> Ys, Xs;
        const bool bGot = CollectBoundary(Covers, YFirst, YLast, XMin, XMax, Ys, Xs);
        TestTrue(TEXT("the stamped control has a boundary on every row of the measured band"), bGot);
        if (bGot)
        {
            StampedDeviation = MaxDeviationFromLine(Ys, Xs);
            TestTrue(FString::Printf(
                TEXT("lattice-snapped stamps scallop the boundary (max deviation %.4f cells, expected > 0.25)"),
                StampedDeviation),
                StampedDeviation > 0.25);
        }
    }

    // --- Both directions on ONE metric, so the two cases cannot score alike (rpc-design.md
    //     section 6). The integer left-edge scan this test used to carry could not do this:
    //     on it the stamped control scored 0.920 against the swept stroke's 0.893, i.e. the
    //     broken form scored BETTER and the assertion could not fail.
    TestTrue(FString::Printf(
        TEXT("swept (%.5f cells) and lattice-stamped (%.4f cells) boundaries are clearly separated"),
        SweptDeviation, StampedDeviation),
        StampedDeviation > SweptDeviation + 0.2);

    // --- 3. Sub-cell response. Sliding the whole stroke a third of a cell must slide the
    //        boundary by exactly that, on every row. This is the assertion the RoundToInt
    //        the fix removed cannot satisfy, and the snapped control below measures what it
    //        does instead rather than asserting it from memory.
    {
        TArray<FStrokeVertex> Shifted = Diagonal;
        for (FStrokeVertex& V : Shifted) { V.X += ShiftCells; }

        auto Covers = [&](double X, double Y) { return StrokeCovers(Shifted, X, Y, RadiusUu, ScaleX, ScaleY); };
        TArray<double> Ys, Xs;
        const bool bGot = CollectBoundary(Covers, YFirst, YLast, XMin, XMax, Ys, Xs);
        TestTrue(TEXT("the shifted stroke has a boundary on every row of the measured band"), bGot);
        TestEqual(TEXT("shifted and unshifted bands cover the same rows"), Xs.Num(), SweptX.Num());
        if (bGot && Xs.Num() == SweptX.Num())
        {
            double WorstErr = 0.0;
            for (int32 i = 0; i < Xs.Num(); ++i)
            {
                WorstErr = FMath::Max(WorstErr, FMath::Abs((Xs[i] - SweptX[i]) - ShiftCells));
            }
            TestTrue(FString::Printf(
                TEXT("a %.2f-cell shift moves the boundary by exactly that on every row (worst error %.2e cells)"),
                ShiftCells, WorstErr),
                WorstErr < 1e-6);
        }

        // The same shift, snapped: every stamp centre rounds, so the boundary moves by a
        // whole cell on some rows and not at all on others. Note the MEAN shift is ~0.35 --
        // rounding dithers, so an average would have read as correct. The per-row error is
        // what exposes it, which is why this asserts on the worst row and not on the mean.
        const TArray<FStrokeVertex> SnappedBase = SnappedStampChain(Diagonal, /*SpacingCells=*/1.0);
        const TArray<FStrokeVertex> SnappedShifted = SnappedStampChain(Shifted, /*SpacingCells=*/1.0);
        auto CoversBase = [&](double X, double Y) { return StampsCover(SnappedBase, X, Y, RadiusUu, ScaleX, ScaleY); };
        auto CoversShifted = [&](double X, double Y) { return StampsCover(SnappedShifted, X, Y, RadiusUu, ScaleX, ScaleY); };
        TArray<double> BaseY, BaseX, ShiftY, ShiftX;
        const bool bGotBase = CollectBoundary(CoversBase, YFirst, YLast, XMin, XMax, BaseY, BaseX);
        const bool bGotShift = CollectBoundary(CoversShifted, YFirst, YLast, XMin, XMax, ShiftY, ShiftX);
        TestTrue(TEXT("both snapped controls have a boundary on every row of the measured band"),
            bGotBase && bGotShift && BaseX.Num() == ShiftX.Num());
        if (bGotBase && bGotShift && BaseX.Num() == ShiftX.Num())
        {
            double SnappedWorstErr = 0.0;
            for (int32 i = 0; i < BaseX.Num(); ++i)
            {
                SnappedWorstErr = FMath::Max(SnappedWorstErr, FMath::Abs((ShiftX[i] - BaseX[i]) - ShiftCells));
            }
            TestTrue(FString::Printf(
                TEXT("under lattice snapping the same shift is wrong by %.4f cells on the worst row (expected > 0.2)"),
                SnappedWorstErr),
                SnappedWorstErr > 0.2);
        }
    }

    // --- 4. A multi-segment curve keeps both properties across its joints: the sweep is one
    //        continuous distance field, so nothing beads or flattens where segments meet.
    {
        TArray<FStrokeVertex> Curve;
        for (int32 i = 0; i <= 8; ++i)
        {
            const double T = (double)i / 8.0;
            Curve.Add(FStrokeVertex{ 12.0 + T * 66.0, 30.0 + 22.0 * FMath::Sin(T * PI), 0.0 });
        }
        TArray<FStrokeVertex> CurveShifted = Curve;
        for (FStrokeVertex& V : CurveShifted) { V.X += ShiftCells; }

        constexpr int32 CurveYFirst = 22;
        constexpr int32 CurveYLast = 60;

        auto CoversCurve = [&](double X, double Y) { return StrokeCovers(Curve, X, Y, RadiusUu, ScaleX, ScaleY); };
        auto CoversCurveShifted = [&](double X, double Y) { return StrokeCovers(CurveShifted, X, Y, RadiusUu, ScaleX, ScaleY); };
        TArray<double> CurveY, CurveX, CurveShiftY, CurveShiftX;
        const bool bGot = CollectBoundary(CoversCurve, CurveYFirst, CurveYLast, XMin, XMax, CurveY, CurveX);
        const bool bGotShift = CollectBoundary(CoversCurveShifted, CurveYFirst, CurveYLast, XMin, XMax, CurveShiftY, CurveShiftX);
        // Every row of the span carries a boundary: a chain of separate stamps would leave
        // gaps between the beads, and CollectBoundary refuses rather than averaging over
        // the rows it did find.
        TestTrue(FString::Printf(TEXT("the multi-segment curve is unbroken across all %d rows of its span"),
            CurveYLast - CurveYFirst + 1), bGot && bGotShift);
        if (bGot && bGotShift && CurveX.Num() == CurveShiftX.Num())
        {
            double WorstErr = 0.0;
            for (int32 i = 0; i < CurveX.Num(); ++i)
            {
                WorstErr = FMath::Max(WorstErr, FMath::Abs((CurveShiftX[i] - CurveX[i]) - ShiftCells));
            }
            TestTrue(FString::Printf(
                TEXT("the curve's boundary is sub-cell across its joints too (worst error %.2e cells)"), WorstErr),
                WorstErr < 1e-6);
        }
    }

    return true;
}

// ---- modifiedVertices is a measured count, and a no-op reports zero -----------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeSculptModifiedCountTest,
    "PinWright.landscape.sculpt.ModifiedVertexCountIsMeasured",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeSculptModifiedCountTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("landscape.sculpt handler registered"), IsHandlerRegistered(TEXT("landscape.sculpt")));

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("SKIPPED (no editor world): PinWright.landscape.sculpt.ModifiedVertexCountIsMeasured ran ZERO of its substantive assertions."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = FString::Printf(TEXT("PW_SculptCount_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    ALandscape* Landscape = CreateSculptFixture(*this, World, Label);
    if (!Landscape)
    {
        return false;
    }

    const FVector WorldCentre = Landscape->GetActorTransform().TransformPosition(FVector(31.5, 31.5, 0.0));

    auto BasePayload = [&]()
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("landscapeName"), Label);
        TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
        Loc->SetNumberField(TEXT("x"), WorldCentre.X);
        Loc->SetNumberField(TEXT("y"), WorldCentre.Y);
        Loc->SetNumberField(TEXT("z"), WorldCentre.Z);
        P->SetObjectField(TEXT("location"), Loc);
        P->SetNumberField(TEXT("brushRadius"), 1200.0);
        P->SetNumberField(TEXT("brushFalloff"), 0.5);
        return P;
    };

    // --- A real raise: the count is measured, and it is NOT the rectangle's area. ---
    {
        TSharedPtr<FJsonObject> P = BasePayload();
        P->SetStringField(TEXT("toolMode"), TEXT("Raise"));
        P->SetNumberField(TEXT("strength"), 60.0);

        TSharedRef<FTestResponseCapture> Capture = RunSculpt(*this, P);
        TestTrue(TEXT("raise responded"), Capture->bWasCalled);
        TestTrue(TEXT("raise succeeded"), Capture->bSuccess);
        if (!Capture->bSuccess)
        {
            return false;
        }

        bool bVerified = false;
        Capture->Result->TryGetBoolField(TEXT("verified"), bVerified);
        // The count only means anything if it came from a post-write readback.
        TestTrue(TEXT("raise reports verified=true (count came from a heightmap readback)"), bVerified);

        const double Modified = GetNumber(Capture->Result, TEXT("modifiedVertices"));
        const double InBrush = GetNumber(Capture->Result, TEXT("verticesInBrush"));
        const double Considered = GetNumber(Capture->Result, TEXT("verticesConsidered"));

        TestTrue(FString::Printf(TEXT("raise moved vertices (modifiedVertices %.0f)"), Modified), Modified > 0.0);
        // THE regression assertion. The old handler returned HeightData.Num(), i.e. exactly
        // `verticesConsidered`, for any stamp that changed anything at all.
        TestTrue(FString::Printf(TEXT("modifiedVertices (%.0f) is not the clamped rectangle area (%.0f)"), Modified, Considered),
            Considered > 0.0 && Modified < Considered);
        // A circular brush cannot move more vertices than it covers.
        TestTrue(FString::Printf(TEXT("modifiedVertices (%.0f) <= verticesInBrush (%.0f)"), Modified, InBrush),
            Modified <= InBrush);
        TestTrue(FString::Printf(TEXT("verticesInBrush (%.0f) < verticesConsidered (%.0f) — the brush is a circle in a rectangle"), InBrush, Considered),
            InBrush > 0.0 && InBrush < Considered);

        bool bChanged = false;
        Capture->Result->TryGetBoolField(TEXT("changed"), bChanged);
        TestTrue(TEXT("raise reports changed=true"), bChanged);
        TestTrue(FString::Printf(TEXT("raise reports a non-zero height magnitude (%f cm)"),
            GetNumber(Capture->Result, TEXT("maxHeightDeltaCm"))),
            GetNumber(Capture->Result, TEXT("maxHeightDeltaCm")) > 0.0);
    }

    // --- A genuine no-op accepted explicitly: the count must be ZERO, not the area. ---
    {
        TSharedPtr<FJsonObject> P = BasePayload();
        P->SetStringField(TEXT("toolMode"), TEXT("Raise"));
        P->SetNumberField(TEXT("strength"), 0.0);
        P->SetBoolField(TEXT("allowNoChange"), true);

        TSharedRef<FTestResponseCapture> Capture = RunSculpt(*this, P);
        TestTrue(TEXT("zero-strength raise responded"), Capture->bWasCalled);
        TestTrue(TEXT("zero-strength raise succeeded under allowNoChange"), Capture->bSuccess);
        if (Capture->bSuccess)
        {
            const double Modified = GetNumber(Capture->Result, TEXT("modifiedVertices"));
            TestEqual(TEXT("a no-op reports modifiedVertices 0"), Modified, 0.0);

            bool bChanged = true;
            Capture->Result->TryGetBoolField(TEXT("changed"), bChanged);
            TestFalse(TEXT("a no-op reports changed=false"), bChanged);

            // The brush still covered vertices — so 0 here is a measurement, not an
            // empty region that could not have changed anything anyway.
            TestTrue(FString::Printf(TEXT("the no-op's brush still covered vertices (%.0f)"),
                GetNumber(Capture->Result, TEXT("verticesInBrush"))),
                GetNumber(Capture->Result, TEXT("verticesInBrush")) > 0.0);
        }
    }

    // --- The same no-op WITHOUT allowNoChange is refused, so it cannot pass unnoticed. --
    {
        TSharedPtr<FJsonObject> P = BasePayload();
        P->SetStringField(TEXT("toolMode"), TEXT("Raise"));
        P->SetNumberField(TEXT("strength"), 0.0);

        TSharedRef<FTestResponseCapture> Capture = RunSculpt(*this, P);
        TestTrue(TEXT("refused no-op responded"), Capture->bWasCalled);
        TestFalse(TEXT("a sculpt that changes nothing is not reported as a success"), Capture->bSuccess);
        TestEqual(TEXT("refused with LANDSCAPE_SCULPT_NO_CHANGE"),
            Capture->ErrorCode, FString(TEXT("LANDSCAPE_SCULPT_NO_CHANGE")));
    }

    // --- A polyline sweep through the same landscape, in one call. ---
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("landscapeName"), Label);
        TArray<TSharedPtr<FJsonValue>> PathPoints;
        for (int32 i = 0; i <= 6; ++i)
        {
            const double T = (double)i / 6.0;
            const FVector LocalPt(8.0 + T * 46.0, 20.0 + 14.0 * FMath::Sin(T * PI), 0.0);
            const FVector WorldPt = Landscape->GetActorTransform().TransformPosition(LocalPt);
            TSharedPtr<FJsonObject> Pt = MakeShared<FJsonObject>();
            Pt->SetNumberField(TEXT("x"), WorldPt.X);
            Pt->SetNumberField(TEXT("y"), WorldPt.Y);
            Pt->SetNumberField(TEXT("z"), WorldPt.Z);
            PathPoints.Add(MakeShared<FJsonValueObject>(Pt));
        }
        P->SetArrayField(TEXT("path"), PathPoints);
        P->SetStringField(TEXT("toolMode"), TEXT("Raise"));
        P->SetStringField(TEXT("falloffProfile"), TEXT("smooth"));
        P->SetNumberField(TEXT("brushRadius"), 600.0);
        P->SetNumberField(TEXT("heightDelta"), 400.0);

        TSharedRef<FTestResponseCapture> Capture = RunSculpt(*this, P);
        TestTrue(TEXT("polyline sculpt responded"), Capture->bWasCalled);
        TestTrue(TEXT("polyline sculpt succeeded"), Capture->bSuccess);
        if (Capture->bSuccess)
        {
            TestEqual(TEXT("the whole polyline was applied in one call"),
                GetNumber(Capture->Result, TEXT("pathPointCount")), 7.0);
            TestEqual(TEXT("six segments swept"), GetNumber(Capture->Result, TEXT("segmentCount")), 6.0);
            TestTrue(FString::Printf(TEXT("the sweep moved vertices (%.0f)"), GetNumber(Capture->Result, TEXT("modifiedVertices"))),
                GetNumber(Capture->Result, TEXT("modifiedVertices")) > 0.0);
            FString Profile;
            Capture->Result->TryGetStringField(TEXT("falloffProfile"), Profile);
            TestEqual(TEXT("the requested falloff profile is echoed back measured"), Profile, FString(TEXT("smooth")));
        }
    }

    return true;
}

// ---- An unrecognised toolMode is refused, not silently ignored ----------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeSculptUnknownToolModeTest,
    "PinWright.landscape.sculpt.RejectsUnknownToolMode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeSculptUnknownToolModeTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("landscape.sculpt handler registered"), IsHandlerRegistered(TEXT("landscape.sculpt")));

    // The toolMode check runs before the async token is minted, so this needs no world,
    // no landscape and no pump — it is pure argument validation.
    auto Payload = [](const TCHAR* ToolMode)
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("landscapeName"), TEXT("PW_NoSuchLandscape"));
        TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
        Loc->SetNumberField(TEXT("x"), 0.0);
        Loc->SetNumberField(TEXT("y"), 0.0);
        Loc->SetNumberField(TEXT("z"), 0.0);
        P->SetObjectField(TEXT("location"), Loc);
        P->SetStringField(TEXT("toolMode"), ToolMode);
        return P;
    };

    // --- A typo is a typed error carrying the valid spellings. ---
    {
        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("landscape.sculpt"), Payload(TEXT("Rasie")), Capture);
        TestTrue(TEXT("handler invoked"), bFound);
        TestTrue(TEXT("handler responded synchronously to the bad toolMode"), Capture.bWasCalled);
        TestFalse(TEXT("a misspelled toolMode is no longer a success"), Capture.bSuccess);
        TestEqual(TEXT("refused with LANDSCAPE_INVALID_TOOL_MODE"),
            Capture.ErrorCode, FString(TEXT("LANDSCAPE_INVALID_TOOL_MODE")));
        // The recovery has to be IN the payload, not only in the prose (rpc-design.md §7).
        const TArray<TSharedPtr<FJsonValue>>* Valid = nullptr;
        const bool bHasValid = Capture.Result.IsValid() && Capture.Result->TryGetArrayField(TEXT("validToolModes"), Valid) && Valid;
        TestTrue(TEXT("the error payload lists the valid tool modes"), bHasValid);
        if (bHasValid)
        {
            TestEqual(TEXT("four tool modes are offered"), Valid->Num(), 4);
        }
    }

    // --- Over-rejection guard: every documented spelling must get past this check. ---
    for (const TCHAR* Good : { TEXT("Raise"), TEXT("Lower"), TEXT("Flatten"), TEXT("Smooth"), TEXT("raise"), TEXT("FLATTEN") })
    {
        TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
        InvokeHandlerWithSharedCapture(TEXT("landscape.sculpt"), Payload(Good), Capture);
        PumpUntilCaptured(*Capture, /*TimeoutSeconds=*/30.0);
        TestNotEqual(FString::Printf(TEXT("'%s' is accepted as a tool mode"), Good),
            Capture->ErrorCode, FString(TEXT("LANDSCAPE_INVALID_TOOL_MODE")));
    }

    // --- An unrecognised falloffProfile is refused on the same principle. ---
    {
        TSharedPtr<FJsonObject> P = Payload(TEXT("Raise"));
        P->SetStringField(TEXT("falloffProfile"), TEXT("gaussian"));
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("landscape.sculpt"), P, Capture);
        TestTrue(TEXT("handler responded synchronously to the bad falloffProfile"), Capture.bWasCalled);
        TestFalse(TEXT("an unknown falloff profile is not a success"), Capture.bSuccess);
        TestEqual(TEXT("refused with INVALID_ARGUMENT"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    }

    // --- Passing both a location and a path is refused rather than half-honoured. ---
    {
        TSharedPtr<FJsonObject> P = Payload(TEXT("Raise"));
        TArray<TSharedPtr<FJsonValue>> Pts;
        TSharedPtr<FJsonObject> Pt = MakeShared<FJsonObject>();
        Pt->SetNumberField(TEXT("x"), 100.0);
        Pt->SetNumberField(TEXT("y"), 100.0);
        Pt->SetNumberField(TEXT("z"), 0.0);
        Pts.Add(MakeShared<FJsonValueObject>(Pt));
        P->SetArrayField(TEXT("path"), Pts);
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("landscape.sculpt"), P, Capture);
        TestTrue(TEXT("handler responded synchronously to location+path"), Capture.bWasCalled);
        TestFalse(TEXT("location and path together is not a success"), Capture.bSuccess);
        TestEqual(TEXT("refused with INVALID_ARGUMENT"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    }

    return true;
}

// ---- The four falloff profiles are genuinely different curves ----------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeSculptFalloffProfileTest,
    "PinWright.landscape.sculpt.FalloffProfilesAreDistinct",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeSculptFalloffProfileTest::RunTest(const FString& Parameters)
{
    constexpr double R = 1000.0;
    constexpr double F = 0.5;                 // ramp occupies the outer half of the radius
    const double MidRamp = R * (1.0 - F * 0.5);  // t = 0.5 for every profile

    const double Linear    = EvaluateFalloff(EFalloffProfile::Linear,    MidRamp, R, F);
    const double Smooth    = EvaluateFalloff(EFalloffProfile::Smooth,    MidRamp, R, F);
    const double Spherical = EvaluateFalloff(EFalloffProfile::Spherical, MidRamp, R, F);
    const double Tip       = EvaluateFalloff(EFalloffProfile::Tip,       MidRamp, R, F);

    // Linear must still be exactly the ramp the pre-2026-08 handler evaluated, so no
    // existing call changes shape: alpha = (R - Dist) / (R * falloff).
    TestTrue(FString::Printf(TEXT("linear reproduces the historical ramp at t=0.5 (got %f)"), Linear),
        FMath::Abs(Linear - 0.5) < 1e-5);
    TestTrue(FString::Printf(TEXT("smoothstep at t=0.5 is 0.5 (got %f)"), Smooth),
        FMath::Abs(Smooth - 0.5) < 1e-5);
    // The engine's curves: spherical = sqrt(1-(1-t)^2) = 0.8660, tip = 1-sqrt(1-t^2) = 0.1340.
    TestTrue(FString::Printf(TEXT("spherical bulges above linear at t=0.5 (got %f, expected ~0.866)"), Spherical),
        FMath::Abs(Spherical - 0.8660254) < 1e-4);
    TestTrue(FString::Printf(TEXT("tip sits below linear at t=0.5 (got %f, expected ~0.134)"), Tip),
        FMath::Abs(Tip - 0.1339746) < 1e-4);
    TestTrue(TEXT("spherical, linear and tip are ordered and distinct"),
        Spherical > Linear + 0.1 && Linear > Tip + 0.1);

    // Smoothstep and linear agree at the midpoint by construction, so pin them apart where
    // they actually differ. `t` runs 0 at the outer rim to 1 at the inner plateau edge and
    // EvaluateFalloff derives it as (R - Dist) / (R * falloff), so the distance sampling a
    // given t is R - t*R*F. This test previously wrote that distance as `R * (1.0 - F*0.75)`
    // -- which IS t = 0.75 -- while its comment and its assertion both called it t = 0.25,
    // and it then asserted the wrong half of the curve. Sampling through DistanceAtT
    // removes the chance to mislabel it again.
    auto DistanceAtT = [&](double T) { return R - T * R * F; };
    auto Alpha = [&](EFalloffProfile P, double T) { return (double)EvaluateFalloff(P, DistanceAtT(T), R, F); };

    // The S-curve property, in the direction it actually has. Smoothstep has zero slope at
    // both ends, so it LAGS the straight ramp over the first half of the ramp and LEADS it
    // over the second, meeting it exactly at the midpoint: smoothstep(0.25) = 0.15625 sits
    // below linear's 0.25, and smoothstep(0.75) = 0.84375 sits above linear's 0.75. A
    // profile collapsed back into one ramp fails every row of this; a sign-flipped one
    // fails half of them.
    TestTrue(FString::Printf(TEXT("smoothstep and linear meet at t=0.5 (%f vs %f)"),
        Alpha(EFalloffProfile::Smooth, 0.5), Alpha(EFalloffProfile::Linear, 0.5)),
        FMath::Abs(Alpha(EFalloffProfile::Smooth, 0.5) - Alpha(EFalloffProfile::Linear, 0.5)) < 1e-6);

    for (const double T : { 0.1, 0.25, 0.4 })
    {
        const double L = Alpha(EFalloffProfile::Linear, T);
        const double S = Alpha(EFalloffProfile::Smooth, T);
        TestTrue(FString::Printf(TEXT("smoothstep (%f) lags linear (%f) below the midpoint (t=%.2f)"), S, L, T),
            S < L - 1e-6);
    }
    for (const double T : { 0.6, 0.75, 0.9 })
    {
        const double L = Alpha(EFalloffProfile::Linear, T);
        const double S = Alpha(EFalloffProfile::Smooth, T);
        TestTrue(FString::Printf(TEXT("smoothstep (%f) leads linear (%f) above the midpoint (t=%.2f)"), S, L, T),
            S > L + 1e-6);
    }

    // Named values, so a curve that merely differs from linear cannot pass as smoothstep.
    {
        const double S25 = Alpha(EFalloffProfile::Smooth, 0.25);
        const double S75 = Alpha(EFalloffProfile::Smooth, 0.75);
        TestTrue(FString::Printf(TEXT("smoothstep at t=0.25 is 0.15625 (got %f)"), S25),
            FMath::Abs(S25 - 0.15625) < 1e-5);
        TestTrue(FString::Printf(TEXT("smoothstep at t=0.75 is 0.84375 (got %f)"), S75),
            FMath::Abs(S75 - 0.84375) < 1e-5);
        TestTrue(FString::Printf(TEXT("smoothstep is antisymmetric about the midpoint (%f + %f)"), S25, S75),
            FMath::Abs((S25 + S75) - 1.0) < 1e-5);
        TestTrue(FString::Printf(TEXT("the gap to linear at t=0.25 is a real separation (%f)"), 0.25 - S25),
            FMath::Abs((0.25 - S25) - 0.09375) < 1e-5);
    }

    // Every profile rises monotonically towards the plateau. A collapsed or inverted curve
    // shows up here even at the t values where two profiles happen to cross.
    for (EFalloffProfile P : { EFalloffProfile::Linear, EFalloffProfile::Smooth,
                               EFalloffProfile::Spherical, EFalloffProfile::Tip })
    {
        double Prev = -1.0;
        bool bMonotone = true;
        for (int32 i = 0; i <= 20; ++i)
        {
            const double A = Alpha(P, (double)i / 20.0);
            if (A < Prev - 1e-6) { bMonotone = false; break; }
            Prev = A;
        }
        TestTrue(FString::Printf(TEXT("profile %s rises monotonically towards the plateau"), FalloffProfileName(P)),
            bMonotone);
    }

    // ...and the four are pairwise distinct off the midpoint, so collapsing them into one
    // ramp cannot pass. At t=0.25: linear 0.25, smooth 0.15625, spherical 0.6614, tip 0.0318.
    {
        const EFalloffProfile Profiles[4] = { EFalloffProfile::Linear, EFalloffProfile::Smooth,
                                              EFalloffProfile::Spherical, EFalloffProfile::Tip };
        for (int32 i = 0; i < 4; ++i)
        {
            for (int32 j = i + 1; j < 4; ++j)
            {
                const double Ai = Alpha(Profiles[i], 0.25);
                const double Aj = Alpha(Profiles[j], 0.25);
                TestTrue(FString::Printf(TEXT("profiles %s (%f) and %s (%f) differ at t=0.25"),
                    FalloffProfileName(Profiles[i]), Ai, FalloffProfileName(Profiles[j]), Aj),
                    FMath::Abs(Ai - Aj) > 0.01);
            }
        }
    }

    // Shared invariants: full weight on the plateau, nothing beyond the radius, and a
    // falloff of 0 is a hard edge rather than a division by zero.
    for (EFalloffProfile P : { EFalloffProfile::Linear, EFalloffProfile::Smooth,
                               EFalloffProfile::Spherical, EFalloffProfile::Tip })
    {
        TestEqual(FString::Printf(TEXT("profile %s is 1 at the centre"), FalloffProfileName(P)),
            (double)EvaluateFalloff(P, 0.0, R, F), 1.0);
        TestEqual(FString::Printf(TEXT("profile %s is 0 outside the radius"), FalloffProfileName(P)),
            (double)EvaluateFalloff(P, R * 1.01, R, F), 0.0);
        TestEqual(FString::Printf(TEXT("profile %s with falloff 0 is a hard edge"), FalloffProfileName(P)),
            (double)EvaluateFalloff(P, R * 0.99, R, 0.0), 1.0);
    }

    // Parsing: the documented spellings resolve, an unknown one leaves Out untouched.
    {
        EToolMode Mode = EToolMode::Flatten;
        TestFalse(TEXT("an unknown tool mode does not parse"), ParseToolMode(TEXT("Erode"), Mode));
        TestTrue(TEXT("a failed parse leaves the caller's value untouched"), Mode == EToolMode::Flatten);
        TestTrue(TEXT("Smooth parses"), ParseToolMode(TEXT("smooth"), Mode) && Mode == EToolMode::Smooth);

        EFalloffProfile Profile = EFalloffProfile::Tip;
        TestFalse(TEXT("an unknown falloff profile does not parse"), ParseFalloffProfile(TEXT("gaussian"), Profile));
        TestTrue(TEXT("a failed profile parse leaves the caller's value untouched"), Profile == EFalloffProfile::Tip);
        TestTrue(TEXT("'smoothstep' is accepted as an alias for 'smooth'"),
            ParseFalloffProfile(TEXT("smoothstep"), Profile) && Profile == EFalloffProfile::Smooth);
    }

    // The nearest-point solve must be exact for the degenerate one-point stroke, because
    // that is the single-stamp form: `location` is a one-vertex path.
    {
        const FStrokeVertex P{ 10.0, 10.0, 500.0 };
        double Dist = -1.0, Z = -1.0;
        ClosestPointOnSegment(13.0, 14.0, P, P, /*ScaleX=*/100.0, /*ScaleY=*/100.0, Dist, Z);
        TestTrue(FString::Printf(TEXT("a one-point stroke measures a plain radial distance (got %f, expected 500)"), Dist),
            FMath::Abs(Dist - 500.0) < 1e-6);
        TestEqual(TEXT("a one-point stroke's Z is the point's own Z"), Z, 500.0);
    }

    // Non-uniform draw scale: the same local separation must measure differently per axis,
    // which is the whole reason the radius is no longer converted through ScaleX alone.
    {
        const FStrokeVertex Origin{ 0.0, 0.0, 0.0 };
        double DistX = 0.0, DistY = 0.0, Z = 0.0;
        ClosestPointOnSegment(4.0, 0.0, Origin, Origin, /*ScaleX=*/50.0, /*ScaleY=*/200.0, DistX, Z);
        ClosestPointOnSegment(0.0, 4.0, Origin, Origin, /*ScaleX=*/50.0, /*ScaleY=*/200.0, DistY, Z);
        TestTrue(FString::Printf(TEXT("4 cells along X at ScaleX 50 is 200 uu (got %f)"), DistX),
            FMath::Abs(DistX - 200.0) < 1e-6);
        TestTrue(FString::Printf(TEXT("4 cells along Y at ScaleY 200 is 800 uu (got %f)"), DistY),
            FMath::Abs(DistY - 800.0) < 1e-6);
    }

    return true;
}
