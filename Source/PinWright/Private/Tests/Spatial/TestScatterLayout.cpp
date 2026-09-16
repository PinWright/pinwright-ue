// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for spatial.scatter_layout (Handlers/Spatial/ScatterLayoutHandler.cpp).
//
// The verb is a pure function - no world, no actor, no asset, no trace - so every case here runs
// with no fixture, nothing spawned and nothing to tear down. That is deliberate: the whole reason
// the layout algorithm is behind a verb instead of a doc paragraph is that its DISTRIBUTION is the
// part callers get wrong, and a distribution is only checkable by a test.
//
// The defect these tests exist to catch is not a crash and not a wrong count. A hand-rolled version
// of this lattice drew the jitter as a uniform choice over (-1, 0, 1) times the jitter amount
// instead of a continuous offset. That is a second, coarser lattice wearing jitter's name: it
// renders as banding from overhead and as rows from the ground, and it passes every check a caller
// would think to run - instance count, spacing statistics and bounds are all correct.
// JitterIsContinuousAndBounded is the assertion that separates the two.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"

#include "Containers/Set.h"
#include "Math/UnrealMathUtility.h"

namespace
{
    // Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace
    // helper with a common name would collide with a sibling test TU once Unity merges them.

    // Kept in step with ScatterLayoutHandler.cpp's ScatterLayoutHexRowFactor (sqrt(3)/2). Written
    // out rather than shared, so a silent change to the handler's row pitch fails a test instead
    // of moving both sides at once.
    constexpr double ScatterLayoutTest_HexRowFactor = 0.86602540378443864676;

    TSharedPtr<FJsonObject> ScatterLayoutTest_Vec(double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), X);
        Obj->SetNumberField(TEXT("y"), Y);
        Obj->SetNumberField(TEXT("z"), Z);
        return Obj;
    }

    TSharedPtr<FJsonObject> ScatterLayoutTest_BoxRegion(double HalfSize, double Z)
    {
        TSharedPtr<FJsonObject> Region = MakeShared<FJsonObject>();
        Region->SetObjectField(TEXT("min"), ScatterLayoutTest_Vec(-HalfSize, -HalfSize, Z));
        Region->SetObjectField(TEXT("max"), ScatterLayoutTest_Vec(HalfSize, HalfSize, Z));
        return Region;
    }

    // One returned transform, decomposed.
    struct FScatterLayoutTestPoint
    {
        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
        double Pitch = 0.0;
        double Yaw = 0.0;
        double Roll = 0.0;
        double ScaleX = 0.0;
        double ScaleY = 0.0;
        double ScaleZ = 0.0;
    };

    // Invokes the verb and decomposes its transforms[]. Returns false (leaving OutPoints empty)
    // when the call failed, so a case that expected success can assert on Capture first and then
    // read the points without re-walking the JSON.
    bool ScatterLayoutTest_Run(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Payload,
        FTestResponseCapture& Capture, TArray<FScatterLayoutTestPoint>& OutPoints)
    {
        OutPoints.Reset();
        Test.TestTrue(TEXT("spatial.scatter_layout is registered"),
            InvokeHandlerWithCapture(TEXT("spatial.scatter_layout"), Payload, Capture));
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* Transforms = nullptr;
        if (!Capture.Result->TryGetArrayField(TEXT("transforms"), Transforms) || !Transforms)
        {
            Test.AddError(TEXT("Successful response carried no transforms[] array."));
            return false;
        }

        for (const TSharedPtr<FJsonValue>& Value : *Transforms)
        {
            const TSharedPtr<FJsonObject>* Entry = nullptr;
            if (!Value.IsValid() || !Value->TryGetObject(Entry) || !Entry || !(*Entry).IsValid())
            {
                Test.AddError(TEXT("transforms[] carried a non-object entry."));
                return false;
            }
            const TSharedPtr<FJsonObject>* Location = nullptr;
            const TSharedPtr<FJsonObject>* Rotation = nullptr;
            const TSharedPtr<FJsonObject>* Scale = nullptr;
            if (!(*Entry)->TryGetObjectField(TEXT("location"), Location) ||
                !(*Entry)->TryGetObjectField(TEXT("rotation"), Rotation) ||
                !(*Entry)->TryGetObjectField(TEXT("scale"), Scale))
            {
                Test.AddError(TEXT("A transform entry was missing location/rotation/scale - the "
                                   "shape foliage.add_instances and actor.spawn_batch consume."));
                return false;
            }

            FScatterLayoutTestPoint Point;
            Point.X = (*Location)->GetNumberField(TEXT("x"));
            Point.Y = (*Location)->GetNumberField(TEXT("y"));
            Point.Z = (*Location)->GetNumberField(TEXT("z"));
            Point.Pitch = (*Rotation)->GetNumberField(TEXT("pitch"));
            Point.Yaw = (*Rotation)->GetNumberField(TEXT("yaw"));
            Point.Roll = (*Rotation)->GetNumberField(TEXT("roll"));
            Point.ScaleX = (*Scale)->GetNumberField(TEXT("x"));
            Point.ScaleY = (*Scale)->GetNumberField(TEXT("y"));
            Point.ScaleZ = (*Scale)->GetNumberField(TEXT("z"));
            OutPoints.Add(Point);
        }
        return true;
    }

    // Asserts the verb refuses a payload with the given error code rather than returning a
    // plausible-looking layout. Every refusal here is a case where a silently-accepted value
    // would produce a scatter that looks fine and is wrong.
    void ScatterLayoutTest_ExpectError(FAutomationTestBase& Test, const TCHAR* What,
        const TSharedPtr<FJsonObject>& Payload, const TCHAR* ExpectedCode)
    {
        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("spatial.scatter_layout is registered"),
            InvokeHandlerWithCapture(TEXT("spatial.scatter_layout"), Payload, Capture));
        Test.TestFalse(*FString::Printf(TEXT("%s is refused, not silently accepted"), What),
            Capture.bSuccess);
        Test.TestEqual(*FString::Printf(TEXT("%s error code"), What),
            Capture.ErrorCode, FString(ExpectedCode));
    }
}

// ============================================================================
// Reproducibility: the same seed returns the same layout, a different seed does not.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScatterLayoutSameSeedTest,
    "PinWright.spatial.scatter_layout.SameSeedIsByteIdentical",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FScatterLayoutSameSeedTest::RunTest(const FString& Parameters)
{
    auto MakePayload = [](int32 Seed)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("region"), ScatterLayoutTest_BoxRegion(600.0, 250.0));
        Payload->SetNumberField(TEXT("spacing"), 120.0);
        Payload->SetNumberField(TEXT("seed"), Seed);
        return Payload;
    };

    FTestResponseCapture CaptureA;
    TArray<FScatterLayoutTestPoint> First;
    TestTrue(TEXT("first run succeeds"), ScatterLayoutTest_Run(*this, MakePayload(4242), CaptureA, First));
    TestTrue(TEXT("the layout is not empty"), First.Num() > 0);
    if (First.Num() == 0)
    {
        return false;
    }

    FTestResponseCapture CaptureB;
    TArray<FScatterLayoutTestPoint> Second;
    TestTrue(TEXT("second run succeeds"), ScatterLayoutTest_Run(*this, MakePayload(4242), CaptureB, Second));

    TestEqual(TEXT("same seed returns the same point count"), Second.Num(), First.Num());
    if (Second.Num() == First.Num())
    {
        bool bIdentical = true;
        for (int32 Index = 0; Index < First.Num(); ++Index)
        {
            const FScatterLayoutTestPoint& A = First[Index];
            const FScatterLayoutTestPoint& B = Second[Index];
            // Exact equality, not near-equality: the contract is byte-identical output, and a
            // tolerance here would hide exactly the drift the contract exists to forbid.
            if (A.X != B.X || A.Y != B.Y || A.Z != B.Z ||
                A.Pitch != B.Pitch || A.Yaw != B.Yaw || A.Roll != B.Roll ||
                A.ScaleX != B.ScaleX || A.ScaleY != B.ScaleY || A.ScaleZ != B.ScaleZ)
            {
                bIdentical = false;
                break;
            }
        }
        TestTrue(TEXT("same seed returns byte-identical transforms"), bIdentical);
    }

    // The seed has to actually reach the generator: a layout that ignored it would pass every
    // reproducibility assertion above while being reproducibly wrong.
    FTestResponseCapture CaptureC;
    TArray<FScatterLayoutTestPoint> Other;
    TestTrue(TEXT("third run succeeds"), ScatterLayoutTest_Run(*this, MakePayload(4243), CaptureC, Other));
    bool bDiffers = Other.Num() != First.Num();
    for (int32 Index = 0; !bDiffers && Index < First.Num(); ++Index)
    {
        bDiffers = (Other[Index].X != First[Index].X) || (Other[Index].Y != First[Index].Y);
    }
    TestTrue(TEXT("a different seed returns a different layout"), bDiffers);

    // The Rotator trap the same doc page warns about: pitch and roll must be literal zero, or
    // every instance is laid on its side and still reads correct from directly overhead.
    bool bUprightAndScaled = true;
    for (const FScatterLayoutTestPoint& Point : First)
    {
        if (Point.Pitch != 0.0 || Point.Roll != 0.0)
        {
            bUprightAndScaled = false;
            break;
        }
        // Default scaleRange is the doc's +/-15%, applied uniformly on all three axes.
        if (Point.ScaleX < 0.85 - UE_KINDA_SMALL_NUMBER || Point.ScaleX > 1.15 + UE_KINDA_SMALL_NUMBER ||
            Point.ScaleY != Point.ScaleX || Point.ScaleZ != Point.ScaleX)
        {
            bUprightAndScaled = false;
            break;
        }
        if (Point.Z != 250.0)
        {
            bUprightAndScaled = false;
            break;
        }
    }
    TestTrue(TEXT("pitch/roll are zero, scale is uniform within the default +/-15%, Z is the "
                  "region plane"), bUprightAndScaled);

    return true;
}

// ============================================================================
// The distribution itself - the assertion a hand-rolled lattice fails silently.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScatterLayoutJitterDistributionTest,
    "PinWright.spatial.scatter_layout.JitterIsContinuousAndBounded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FScatterLayoutJitterDistributionTest::RunTest(const FString& Parameters)
{
    constexpr double Spacing = 100.0;
    constexpr double JitterFraction = 0.18;
    const double JitterCm = Spacing * JitterFraction;
    const double RowStep = Spacing * ScatterLayoutTest_HexRowFactor;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("region"), ScatterLayoutTest_BoxRegion(1000.0, 0.0));
    Payload->SetNumberField(TEXT("spacing"), Spacing);
    Payload->SetNumberField(TEXT("jitter"), JitterFraction);
    Payload->SetNumberField(TEXT("seed"), 20260828);

    FTestResponseCapture Capture;
    TArray<FScatterLayoutTestPoint> Points;
    TestTrue(TEXT("layout succeeds"), ScatterLayoutTest_Run(*this, Payload, Capture, Points));
    // A small sample cannot distinguish a continuous distribution from a three-valued one, so
    // assert the sample size before drawing any conclusion from it.
    TestTrue(TEXT("the sample is large enough to be about a distribution"), Points.Num() >= 200);
    if (Points.Num() < 200)
    {
        return false;
    }

    // Recover each point's lattice cell and its offset within it. Jitter is capped at 0.5 of a
    // step, and 0.18 is well inside that, so the nearest lattice site is unambiguous: 0.18*100
    // against a 86.6 row pitch is 0.21 of a row, and against a 100 column pitch is 0.18.
    TSet<int64> DistinctOffsetsX;
    TSet<int64> DistinctOffsetsY;
    double SumAbsOffsetX = 0.0;
    int32 EvenRowPoints = 0;
    int32 OddRowPoints = 0;
    bool bWithinJitterBound = true;

    for (const FScatterLayoutTestPoint& Point : Points)
    {
        const int32 Row = FMath::RoundToInt32(Point.Y / RowStep);
        const double RowOffsetX = ((Row & 1) != 0) ? (Spacing * 0.5) : 0.0;
        const int32 Col = FMath::RoundToInt32((Point.X - RowOffsetX) / Spacing);

        const double OffsetX = Point.X - (Col * Spacing + RowOffsetX);
        const double OffsetY = Point.Y - (Row * RowStep);

        if (FMath::Abs(OffsetX) > JitterCm + UE_KINDA_SMALL_NUMBER ||
            FMath::Abs(OffsetY) > JitterCm + UE_KINDA_SMALL_NUMBER)
        {
            bWithinJitterBound = false;
        }

        SumAbsOffsetX += FMath::Abs(OffsetX);
        ((Row & 1) != 0 ? OddRowPoints : EvenRowPoints) += 1;

        // Quantise to 1e-4 cm before counting distinct values. A continuous draw produces a
        // distinct offset per point; the (-1, 0, 1) defect produces exactly three.
        DistinctOffsetsX.Add(static_cast<int64>(FMath::RoundToDouble(OffsetX * 10000.0)));
        DistinctOffsetsY.Add(static_cast<int64>(FMath::RoundToDouble(OffsetY * 10000.0)));
    }

    TestTrue(TEXT("every offset stays inside +/- jitter*spacing"), bWithinJitterBound);

    // THE assertion. A discrete jitter (the recorded defect) collapses this to a handful of
    // values while leaving count, spacing statistics and bounds all correct.
    const int32 MinDistinct = FMath::Max(2, (Points.Num() * 9) / 10);
    TestTrue(*FString::Printf(TEXT("X offsets are continuous: %d distinct values over %d points "
                                   "(need >= %d)"), DistinctOffsetsX.Num(), Points.Num(), MinDistinct),
        DistinctOffsetsX.Num() >= MinDistinct);
    TestTrue(*FString::Printf(TEXT("Y offsets are continuous: %d distinct values over %d points "
                                   "(need >= %d)"), DistinctOffsetsY.Num(), Points.Num(), MinDistinct),
        DistinctOffsetsY.Num() >= MinDistinct);

    // Jitter is applied at roughly the requested magnitude. A uniform draw over [-J, +J] has
    // mean |offset| = J/2; a jitter silently dropped to zero would pass the bound check above.
    const double MeanAbsOffsetX = SumAbsOffsetX / static_cast<double>(Points.Num());
    TestTrue(*FString::Printf(TEXT("mean |X offset| %.2f is a real fraction of the %.2f cm jitter"),
                              MeanAbsOffsetX, JitterCm),
        MeanAbsOffsetX > JitterCm * 0.25 && MeanAbsOffsetX < JitterCm);

    // The two hex sublattices (offset and un-offset rows) carry comparable populations. A broken
    // half-step offset - the lattice degenerating towards a square grid, or one parity dropped -
    // shows up here.
    const int32 LargerParity = FMath::Max(EvenRowPoints, OddRowPoints);
    TestTrue(*FString::Printf(TEXT("both hex sublattices are populated (%d even-row, %d odd-row)"),
                              EvenRowPoints, OddRowPoints),
        EvenRowPoints > 0 && OddRowPoints > 0 &&
        LargerParity < static_cast<int32>(Points.Num() * 0.6));

    // Mean nearest-neighbour distance sits AT the spacing, not well above it. Rejection sampling
    // with a minimum-distance test - the tool the doc page argues against - averages far above
    // its own minimum, which is why the stand it produces reads sparse and clumped.
    double SumNearest = 0.0;
    for (int32 I = 0; I < Points.Num(); ++I)
    {
        double Nearest = TNumericLimits<double>::Max();
        for (int32 J = 0; J < Points.Num(); ++J)
        {
            if (I == J)
            {
                continue;
            }
            const double DX = Points[I].X - Points[J].X;
            const double DY = Points[I].Y - Points[J].Y;
            Nearest = FMath::Min(Nearest, DX * DX + DY * DY);
        }
        SumNearest += FMath::Sqrt(Nearest);
    }
    const double MeanNearest = SumNearest / static_cast<double>(Points.Num());
    TestTrue(*FString::Printf(TEXT("mean nearest-neighbour distance %.2f cm sits near the %.2f cm "
                                   "spacing, not far above it"), MeanNearest, Spacing),
        MeanNearest > Spacing * 0.6 && MeanNearest < Spacing * 1.05);

    return true;
}

// ============================================================================
// Constraints: the region bounds and the carve-outs are honoured, and nothing is
// dropped unattributed.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScatterLayoutConstraintsTest,
    "PinWright.spatial.scatter_layout.HonoursExclusionsAndBounds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FScatterLayoutConstraintsTest::RunTest(const FString& Parameters)
{
    constexpr double HalfSize = 1000.0;
    constexpr double CarveRadius = 400.0;

    TSharedPtr<FJsonObject> Carve = MakeShared<FJsonObject>();
    Carve->SetObjectField(TEXT("center"), ScatterLayoutTest_Vec(0.0, 0.0, 0.0));
    Carve->SetNumberField(TEXT("radius"), CarveRadius);

    TArray<TSharedPtr<FJsonValue>> Excludes;
    Excludes.Add(MakeShared<FJsonValueObject>(Carve));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("region"), ScatterLayoutTest_BoxRegion(HalfSize, 0.0));
    Payload->SetNumberField(TEXT("spacing"), 150.0);
    Payload->SetNumberField(TEXT("seed"), 7);
    Payload->SetArrayField(TEXT("exclude"), Excludes);

    FTestResponseCapture Capture;
    TArray<FScatterLayoutTestPoint> Points;
    TestTrue(TEXT("layout succeeds"), ScatterLayoutTest_Run(*this, Payload, Capture, Points));
    TestTrue(TEXT("the layout is not empty"), Points.Num() > 0);
    if (Points.Num() == 0)
    {
        return false;
    }

    bool bInsideRegion = true;
    bool bOutsideCarve = true;
    for (const FScatterLayoutTestPoint& Point : Points)
    {
        if (Point.X < -HalfSize || Point.X > HalfSize || Point.Y < -HalfSize || Point.Y > HalfSize)
        {
            bInsideRegion = false;
        }
        if (FMath::Sqrt(Point.X * Point.X + Point.Y * Point.Y) <= CarveRadius)
        {
            bOutsideCarve = false;
        }
    }
    // Containment is tested after jitter, so "inside" means inside - not inside plus a jitter's
    // worth of slop.
    TestTrue(TEXT("every returned point is inside the region"), bInsideRegion);
    TestTrue(TEXT("no returned point is inside the carve-out"), bOutsideCarve);

    // Nothing vanishes: every lattice point is either returned, outside the region, or carved.
    double LatticePoints = 0.0;
    double OutsideRegion = 0.0;
    double Excluded = 0.0;
    double Count = 0.0;
    TestTrue(TEXT("response carries latticePoints"),
        Capture.Result->TryGetNumberField(TEXT("latticePoints"), LatticePoints));
    TestTrue(TEXT("response carries outsideRegion"),
        Capture.Result->TryGetNumberField(TEXT("outsideRegion"), OutsideRegion));
    TestTrue(TEXT("response carries excluded"),
        Capture.Result->TryGetNumberField(TEXT("excluded"), Excluded));
    TestTrue(TEXT("response carries count"),
        Capture.Result->TryGetNumberField(TEXT("count"), Count));
    TestTrue(TEXT("the carve-out actually removed points"), Excluded > 0.0);
    TestEqual(TEXT("count matches the returned transforms"),
        static_cast<int32>(Count), Points.Num());
    TestEqual(TEXT("count + outsideRegion + excluded accounts for every lattice point"),
        static_cast<int32>(Count + OutsideRegion + Excluded), static_cast<int32>(LatticePoints));

    return true;
}

// ============================================================================
// One global grid: two overlapping regions agree exactly on their intersection.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScatterLayoutGlobalGridTest,
    "PinWright.spatial.scatter_layout.OverlappingRegionsAgreeOnTheirIntersection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FScatterLayoutGlobalGridTest::RunTest(const FString& Parameters)
{
    auto MakePayload = [](double MinX, double MinY, double MaxX, double MaxY)
    {
        TSharedPtr<FJsonObject> Region = MakeShared<FJsonObject>();
        Region->SetObjectField(TEXT("min"), ScatterLayoutTest_Vec(MinX, MinY, 0.0));
        Region->SetObjectField(TEXT("max"), ScatterLayoutTest_Vec(MaxX, MaxY, 0.0));

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("region"), Region);
        Payload->SetNumberField(TEXT("spacing"), 130.0);
        Payload->SetNumberField(TEXT("seed"), 99);
        return Payload;
    };

    FTestResponseCapture CaptureA;
    TArray<FScatterLayoutTestPoint> ZoneA;
    TestTrue(TEXT("zone A succeeds"),
        ScatterLayoutTest_Run(*this, MakePayload(-900.0, -900.0, 100.0, 100.0), CaptureA, ZoneA));

    FTestResponseCapture CaptureB;
    TArray<FScatterLayoutTestPoint> ZoneB;
    TestTrue(TEXT("zone B succeeds"),
        ScatterLayoutTest_Run(*this, MakePayload(-400.0, -400.0, 600.0, 600.0), CaptureB, ZoneB));

    // Points of A that also fall inside B's box must appear in B, at the identical position and
    // with the identical yaw and scale. A region-anchored lattice, or one running RNG stream,
    // fails this: the two zones double-seed their intersection with two different scatters.
    int32 Shared = 0;
    int32 Missing = 0;
    for (const FScatterLayoutTestPoint& A : ZoneA)
    {
        if (A.X < -400.0 || A.X > 600.0 || A.Y < -400.0 || A.Y > 600.0)
        {
            continue;
        }
        ++Shared;
        bool bFound = false;
        for (const FScatterLayoutTestPoint& B : ZoneB)
        {
            if (A.X == B.X && A.Y == B.Y && A.Yaw == B.Yaw && A.ScaleX == B.ScaleX)
            {
                bFound = true;
                break;
            }
        }
        if (!bFound)
        {
            ++Missing;
        }
    }

    TestTrue(TEXT("the two zones genuinely overlap"), Shared > 0);
    TestEqual(TEXT("every shared point is identical in both zones"), Missing, 0);

    return true;
}

// ============================================================================
// Typed refusals: a value that would produce a plausible-but-wrong layout is
// rejected rather than clamped.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScatterLayoutRefusalsTest,
    "PinWright.spatial.scatter_layout.TypedRefusals",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FScatterLayoutRefusalsTest::RunTest(const FString& Parameters)
{
    // No region at all.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetNumberField(TEXT("spacing"), 100.0);
        ScatterLayoutTest_ExpectError(*this, TEXT("a missing region"), Payload, TEXT("INVALID_PARAMS"));
    }

    // A region with neither shape.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("region"), MakeShared<FJsonObject>());
        Payload->SetNumberField(TEXT("spacing"), 100.0);
        ScatterLayoutTest_ExpectError(*this, TEXT("a shapeless region"), Payload, TEXT("INVALID_PARAMS"));
    }

    // Zero spacing: a lattice with no pitch is not a coarse lattice, it is a division by nothing.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("region"), ScatterLayoutTest_BoxRegion(500.0, 0.0));
        Payload->SetNumberField(TEXT("spacing"), 0.0);
        ScatterLayoutTest_ExpectError(*this, TEXT("zero spacing"), Payload, TEXT("INVALID_PARAMS"));
    }

    // An unknown pattern, rather than silently falling back to hex.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("region"), ScatterLayoutTest_BoxRegion(500.0, 0.0));
        Payload->SetNumberField(TEXT("spacing"), 100.0);
        Payload->SetStringField(TEXT("pattern"), TEXT("poisson"));
        ScatterLayoutTest_ExpectError(*this, TEXT("an unknown pattern"), Payload, TEXT("INVALID_PARAMS"));
    }

    // Jitter past half a step, where the spacing guarantee stops meaning anything.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("region"), ScatterLayoutTest_BoxRegion(500.0, 0.0));
        Payload->SetNumberField(TEXT("spacing"), 100.0);
        Payload->SetNumberField(TEXT("jitter"), 0.9);
        ScatterLayoutTest_ExpectError(*this, TEXT("over-large jitter"), Payload, TEXT("INVALID_PARAMS"));
    }

    // Over budget: refused with the computed count, never truncated to a layout that stops
    // partway across the region.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("region"), ScatterLayoutTest_BoxRegion(10000.0, 0.0));
        Payload->SetNumberField(TEXT("spacing"), 50.0);
        Payload->SetNumberField(TEXT("maxPoints"), 100.0);

        FTestResponseCapture Capture;
        TestTrue(TEXT("spatial.scatter_layout is registered"),
            InvokeHandlerWithCapture(TEXT("spatial.scatter_layout"), Payload, Capture));
        TestFalse(TEXT("an over-budget lattice is refused"), Capture.bSuccess);
        TestEqual(TEXT("over-budget error code"), Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
        TestTrue(TEXT("the refusal names the budget so the caller can raise it"),
            Capture.Message.Contains(TEXT("maxPoints")));
    }

    // A region far enough from the origin, over a small enough spacing, produces a lattice index
    // that does not fit an int32. Refused before the narrowing, which would otherwise be
    // undefined behaviour rather than a large number.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("region"), ScatterLayoutTest_BoxRegion(1.0e12, 0.0));
        Payload->SetNumberField(TEXT("spacing"), 0.01);

        FTestResponseCapture Capture;
        TestTrue(TEXT("spatial.scatter_layout is registered"),
            InvokeHandlerWithCapture(TEXT("spatial.scatter_layout"), Payload, Capture));
        TestFalse(TEXT("an unrepresentable lattice is refused"), Capture.bSuccess);
        TestEqual(TEXT("unrepresentable-lattice error code"), Capture.ErrorCode,
            FString(TEXT("INVALID_PARAMS")));
        // Specifically the index guard, not the point budget - the two are different refusals
        // and the caller's fix differs.
        TestTrue(TEXT("the refusal names the lattice index range"),
            Capture.Message.Contains(TEXT("lattice index")));
    }

    // An unparseable carve-out is refused, not skipped: a corridor silently left uncarved leaves
    // instances standing in it and still looks like a valid layout.
    {
        TArray<TSharedPtr<FJsonValue>> Excludes;
        Excludes.Add(MakeShared<FJsonValueNumber>(42.0));

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("region"), ScatterLayoutTest_BoxRegion(500.0, 0.0));
        Payload->SetNumberField(TEXT("spacing"), 100.0);
        Payload->SetArrayField(TEXT("exclude"), Excludes);
        ScatterLayoutTest_ExpectError(*this, TEXT("a non-object exclude entry"), Payload,
            TEXT("INVALID_PARAMS"));
    }

    return true;
}
