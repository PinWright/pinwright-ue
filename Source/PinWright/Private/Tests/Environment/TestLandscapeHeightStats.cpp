// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for the landscape height-readback (ticket F-landscape-height-readback).
//
// Before this fix the landscape.* namespace could WRITE the heightmap (sculpt / edit) but had
// no way to sample it, so a sculpt/edit round-trip could not be verified per region — a caller
// was reduced to reading a whole-actor aggregate that cannot distinguish "central region rose
// while the pad stayed flat". LandscapeHeightStats::BuildHeightStatsJson turns a region of raw
// uint16 height samples (the samples the write path already reads via GetHeightData) into
// verifiable aggregates: raw min/max/mean height and their world-space Z.
//
// Counterfactual: reverting the fix removes BuildHeightStatsJson / HeightToWorldZ, so this test
// fails to compile/link. With the fix present, breaking the aggregate reduction or the uint16 ->
// world-Z conversion (wrong 32768 midpoint, wrong 1/128 scale, dropped actor Z scale/offset)
// fails the assertions below.
#include "Misc/AutomationTest.h"

#include "Handlers/Environment/LandscapeHeightStats.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeHeightStatsReadbackTest,
    "PinWright.landscape.get_heights.HeightStats",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeHeightStatsReadbackTest::RunTest(const FString& Parameters)
{
    // --- 2x2 region of known raw heights, with ScaleZ=128 and ActorLocationZ=1000. ---
    // At ScaleZ=128 one uint16 unit maps to exactly one world cm relative to the 32768 plane,
    // so world Z = (Height - 32768) + 1000. Row-major samples:
    //   a=32768 -> Z=1000, b=33280 -> Z=1512, c=32512 -> Z=744, d=33024 -> Z=1256.
    // min=32512 (Z=744), max=33280 (Z=1512), sum=131584, mean=32896 (Z=1128).
    const double ScaleZ = 128.0;
    const double ActorLocationZ = 1000.0;
    TArray<uint16> Heights = { 32768, 33280, 32512, 33024 };

    FString Error;
    TSharedPtr<FJsonObject> Stats = LandscapeHeightStats::BuildHeightStatsJson(
        Heights, /*SizeX=*/2, /*SizeY=*/2, ScaleZ, ActorLocationZ,
        /*bIncludeSamples=*/true, /*MaxSamples=*/4096, Error);
    if (!TestTrue(FString::Printf(TEXT("Height stats built (err='%s')"), *Error), Stats.IsValid()))
    {
        return true;
    }

    TestEqual(TEXT("sizeX"), static_cast<int32>(Stats->GetNumberField(TEXT("sizeX"))), 2);
    TestEqual(TEXT("sizeY"), static_cast<int32>(Stats->GetNumberField(TEXT("sizeY"))), 2);
    TestEqual(TEXT("sampleCount"), static_cast<int32>(Stats->GetNumberField(TEXT("sampleCount"))), 4);

    // Raw uint16 aggregates.
    TestEqual(TEXT("minHeight"), static_cast<int32>(Stats->GetNumberField(TEXT("minHeight"))), 32512);
    TestEqual(TEXT("maxHeight"), static_cast<int32>(Stats->GetNumberField(TEXT("maxHeight"))), 33280);
    TestEqual(TEXT("meanHeight"), Stats->GetNumberField(TEXT("meanHeight")), 32896.0);

    // World-space Z conversion — the signal that turns a raw sample into a verifiable relief.
    TestEqual(TEXT("minZ"), Stats->GetNumberField(TEXT("minZ")), 744.0);
    TestEqual(TEXT("maxZ"), Stats->GetNumberField(TEXT("maxZ")), 1512.0);
    TestEqual(TEXT("meanZ"), Stats->GetNumberField(TEXT("meanZ")), 1128.0);

    // Raw samples returned in row-major order when requested, not truncated (cap > count).
    {
        const TArray<TSharedPtr<FJsonValue>>& Arr = Stats->GetArrayField(TEXT("heights"));
        if (TestEqual(TEXT("heights sample count"), Arr.Num(), 4))
        {
            TestEqual(TEXT("heights[0]"), static_cast<int32>(Arr[0]->AsNumber()), 32768);
            TestEqual(TEXT("heights[1]"), static_cast<int32>(Arr[1]->AsNumber()), 33280);
            TestEqual(TEXT("heights[2]"), static_cast<int32>(Arr[2]->AsNumber()), 32512);
            TestEqual(TEXT("heights[3]"), static_cast<int32>(Arr[3]->AsNumber()), 33024);
        }
        bool bTruncated = true;
        TestTrue(TEXT("samplesTruncated present"), Stats->TryGetBoolField(TEXT("samplesTruncated"), bTruncated));
        TestFalse(TEXT("full sample set is not truncated"), bTruncated);
    }

    // --- maxSamples cap truncates the returned array and flags it. ---
    TSharedPtr<FJsonObject> Capped = LandscapeHeightStats::BuildHeightStatsJson(
        Heights, 2, 2, ScaleZ, ActorLocationZ, /*bIncludeSamples=*/true, /*MaxSamples=*/2, Error);
    if (TestTrue(TEXT("Capped stats built"), Capped.IsValid()))
    {
        const TArray<TSharedPtr<FJsonValue>>& CappedArr = Capped->GetArrayField(TEXT("heights"));
        TestEqual(TEXT("capped heights count"), CappedArr.Num(), 2);
        bool bTruncated = false;
        TestTrue(TEXT("capped samplesTruncated present"), Capped->TryGetBoolField(TEXT("samplesTruncated"), bTruncated));
        TestTrue(TEXT("capped set flagged truncated"), bTruncated);
    }

    // --- Buffer / region size mismatch fails loud instead of reading garbage. ---
    {
        TArray<uint16> Wrong = { 1, 2, 3 };
        FString MismatchError;
        TSharedPtr<FJsonObject> Bad = LandscapeHeightStats::BuildHeightStatsJson(
            Wrong, 2, 2, ScaleZ, ActorLocationZ, false, 4096, MismatchError);
        TestFalse(TEXT("size-mismatch yields no stats"), Bad.IsValid());
        TestTrue(TEXT("size-mismatch error populated"), !MismatchError.IsEmpty());
    }

    // --- Direct world-Z conversion: isolate the scale multiplier and the actor offset. ---
    // 32768 is the zero plane, so it maps to exactly the actor Z regardless of scale.
    TestEqual(TEXT("midpoint maps to actor Z"),
        LandscapeHeightStats::HeightToWorldZ(32768, /*ScaleZ=*/256.0, /*ActorLocationZ=*/50.0), 50.0);
    // One 128-unit step above the midpoint is one local unit, scaled by ScaleZ (256 here).
    TestEqual(TEXT("scale multiplier applied"),
        LandscapeHeightStats::HeightToWorldZ(32768 + 128, /*ScaleZ=*/256.0, /*ActorLocationZ=*/0.0), 256.0);

    // --- Region resolution honors a legitimately-NEGATIVE coordinate (F-landscape-height-readback). ---
    // On a multi-component landscape whose GetLandscapeExtent min is below zero (not anchored at
    // section 0), an explicitly-provided negative minX/minY must be HONORED, not silently swapped
    // for the full-extent default. The old `(coord >= 0) ? coord : full` sentinel could not tell a
    // negative coordinate from an unspecified field; ResolveHeightRegion distinguishes them via
    // TOptional (absent -> full extent; set -> honored, even when negative). This is the readback's
    // shared resolver, used identically by the write verb landscape.edit so the two never diverge.
    {
        const int32 FullMinX = -100, FullMinY = -100, FullMaxX = 100, FullMaxY = 100;

        // Provided negative coord is honored (the core of the fix) — NOT replaced by the full min.
        const LandscapeHeightStats::FResolvedHeightRegion R = LandscapeHeightStats::ResolveHeightRegion(
            TOptional<int32>(-50), TOptional<int32>(-40), TOptional<int32>(60), TOptional<int32>(70),
            FullMinX, FullMinY, FullMaxX, FullMaxY);
        TestTrue(TEXT("negative region resolves valid"), R.bValid);
        TestEqual(TEXT("negative minX honored, not defaulted"), R.MinX, -50);
        TestEqual(TEXT("negative minY honored, not defaulted"), R.MinY, -40);
        TestEqual(TEXT("maxX honored"), R.MaxX, 60);
        TestEqual(TEXT("maxY honored"), R.MaxY, 70);

        // An ABSENT coordinate (unset optional) defaults to the corresponding full extent.
        const LandscapeHeightStats::FResolvedHeightRegion RDefault = LandscapeHeightStats::ResolveHeightRegion(
            TOptional<int32>(), TOptional<int32>(), TOptional<int32>(), TOptional<int32>(),
            FullMinX, FullMinY, FullMaxX, FullMaxY);
        TestEqual(TEXT("absent minX -> full min"), RDefault.MinX, FullMinX);
        TestEqual(TEXT("absent minY -> full min"), RDefault.MinY, FullMinY);
        TestEqual(TEXT("absent maxX -> full max"), RDefault.MaxX, FullMaxX);
        TestEqual(TEXT("absent maxY -> full max"), RDefault.MaxY, FullMaxY);

        // Out-of-range coordinates clamp into the full extent (a below-min request clamps up).
        const LandscapeHeightStats::FResolvedHeightRegion RClamp = LandscapeHeightStats::ResolveHeightRegion(
            TOptional<int32>(-500), TOptional<int32>(-500), TOptional<int32>(500), TOptional<int32>(500),
            FullMinX, FullMinY, FullMaxX, FullMaxY);
        TestEqual(TEXT("below-min clamps to full min X"), RClamp.MinX, FullMinX);
        TestEqual(TEXT("above-max clamps to full max X"), RClamp.MaxX, FullMaxX);

        // An inverted region (min > max after clamping) is flagged invalid, not sampled as garbage.
        const LandscapeHeightStats::FResolvedHeightRegion RBad = LandscapeHeightStats::ResolveHeightRegion(
            TOptional<int32>(50), TOptional<int32>(0), TOptional<int32>(10), TOptional<int32>(0),
            FullMinX, FullMinY, FullMaxX, FullMaxY);
        TestFalse(TEXT("inverted region flagged invalid"), RBad.bValid);

        // A region wholly PAST the extent is invalid too, and that is not something the clamp can
        // decide: it pulls all four coordinates onto the far edge, producing a perfectly valid
        // one-pixel region that then reads as a real measurement of terrain nobody asked about
        // (B-landscape-get-heights-fabricates-out-of-extent-region). The overlap test runs on the
        // REQUESTED rectangle, before the clamp.
        const LandscapeHeightStats::FResolvedHeightRegion ROutside = LandscapeHeightStats::ResolveHeightRegion(
            TOptional<int32>(500), TOptional<int32>(500), TOptional<int32>(600), TOptional<int32>(600),
            FullMinX, FullMinY, FullMaxX, FullMaxY);
        TestFalse(TEXT("region entirely past the extent does not overlap it"), ROutside.bOverlapsExtent);
        TestFalse(TEXT("region entirely past the extent is invalid, not a one-pixel read"), ROutside.bValid);

        // A region that straddles the edge stays valid — it has real terrain in it — but records
        // that the caller's coordinates were moved, which is what the response has to report.
        const LandscapeHeightStats::FResolvedHeightRegion RStraddle = LandscapeHeightStats::ResolveHeightRegion(
            TOptional<int32>(50), TOptional<int32>(50), TOptional<int32>(500), TOptional<int32>(500),
            FullMinX, FullMinY, FullMaxX, FullMaxY);
        TestTrue(TEXT("straddling region overlaps the extent"), RStraddle.bOverlapsExtent);
        TestTrue(TEXT("straddling region is valid"), RStraddle.bValid);
        TestTrue(TEXT("straddling region records that it was clamped"), RStraddle.bClamped);
        TestEqual(TEXT("straddling region is clamped to the extent max"), RStraddle.MaxX, FullMaxX);

        // Defaulted (absent) coordinates are never a clamp: nothing the caller asked for was moved.
        TestFalse(TEXT("a fully-defaulted region is not reported as clamped"), RDefault.bClamped);
    }

    // --- The engine's "found nothing" is a pair of sentinels, not an error
    //     (B-landscape-get-heights-fabricates-out-of-extent-region). ---
    // FLandscapeEditDataInterface::GetHeightData seeds its four int32& out-params with
    // INT_MAX / INT_MIN and narrows them only for components it found; with none found they
    // survive the final clamp (LandscapeEditInterface.cpp:1291-1294) and reach the wire verbatim,
    // which is how `region` was once published as {2147483647, ..., -2147483648}. Intersecting
    // them with the asked-for rectangle turns that into the answer it actually is.
    {
        // Nothing found: the whole buffer is interpolated fill, so nothing may be reported.
        const LandscapeHeightStats::FMeasuredHeightRegion None = LandscapeHeightStats::ResolveMeasuredRegion(
            /*Asked=*/0, 0, 9, 9, /*Returned=*/MAX_int32, MAX_int32, MIN_int32, MIN_int32);
        TestFalse(TEXT("unnarrowed sentinels mean nothing was measured"), None.bAnyMeasured);
        TestEqual(TEXT("every asked-for sample is fabricated when nothing was measured"),
            None.FabricatedSampleCount, (int64)100);

        // Everything found: the engine returns the request verbatim.
        const LandscapeHeightStats::FMeasuredHeightRegion All = LandscapeHeightStats::ResolveMeasuredRegion(
            0, 0, 9, 9, 0, 0, 9, 9);
        TestTrue(TEXT("a verbatim return is fully measured"), All.bFullyMeasured);
        TestEqual(TEXT("no fabricated samples in a full measurement"), All.FabricatedSampleCount, (int64)0);
        TestEqual(TEXT("all samples measured"), All.MeasuredSampleCount, (int64)100);

        // Part found: the measured sub-rectangle is what may be reported, and the remainder is
        // counted rather than published.
        const LandscapeHeightStats::FMeasuredHeightRegion Part = LandscapeHeightStats::ResolveMeasuredRegion(
            0, 0, 9, 9, 0, 0, 4, 9);
        TestTrue(TEXT("a narrowed return still measured something"), Part.bAnyMeasured);
        TestFalse(TEXT("a narrowed return is not fully measured"), Part.bFullyMeasured);
        TestEqual(TEXT("measured sub-rectangle max X"), Part.MaxX, 4);
        TestEqual(TEXT("measured samples"), Part.MeasuredSampleCount, (int64)50);
        TestEqual(TEXT("fabricated samples"), Part.FabricatedSampleCount, (int64)50);

        // Trimming the buffer to the measured sub-rectangle drops the fill on the floor.
        TArray<uint16> Buffer;
        Buffer.SetNumUninitialized(9);
        for (int32 i = 0; i < 9; ++i) { Buffer[i] = (uint16)(100 + i); }   // 3x3 over [0,0]..[2,2]
        TArray<uint16> Trimmed;
        TestTrue(TEXT("sub-region extraction succeeds"),
            LandscapeHeightStats::ExtractSubRegion(Buffer, 0, 0, 3, 3, 1, 1, 2, 2, Trimmed));
        TestEqual(TEXT("trimmed sample count"), Trimmed.Num(), 4);
        TestEqual(TEXT("trimmed row 0 col 0"), (int32)Trimmed[0], 104);
        TestEqual(TEXT("trimmed row 0 col 1"), (int32)Trimmed[1], 105);
        TestEqual(TEXT("trimmed row 1 col 0"), (int32)Trimmed[2], 107);
        TestEqual(TEXT("trimmed row 1 col 1"), (int32)Trimmed[3], 108);

        TArray<uint16> Rejected;
        TestFalse(TEXT("a window outside the buffer is refused, not read out of bounds"),
            LandscapeHeightStats::ExtractSubRegion(Buffer, 0, 0, 3, 3, 1, 1, 3, 3, Rejected));
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
