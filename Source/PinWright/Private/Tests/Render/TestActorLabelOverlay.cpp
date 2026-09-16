// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for PinWrightActorLabels (ActorLabelOverlay.h) — the pure selection / ordering /
// capping / de-overlap / behind-camera math behind render.capture_annotated's `actorLabels`
// discovery overlay.
//
// No world, no editor, no viewport, no RHI: every test builds synthetic candidates, so these run
// deterministically on any host (same shape as Tests/Drive/TestDriveSetOfMarkLayout.cpp, which
// covers the equivalent Set-of-Mark layout math for the UI surface).

#include "Misc/AutomationTest.h"

#include "Handlers/Render/ActorLabelOverlay.h"

namespace
{
    using namespace PinWrightActorLabels;

    // Builds an on-screen candidate with the given name and clamped screen rect.
    FActorLabelCandidate MakeLabelCandidate(const FString& Name, double X, double Y,
        double Width, double Height, double Distance = 100.0)
    {
        FActorLabelCandidate Candidate;
        Candidate.Name = Name;
        Candidate.Label = Name;
        Candidate.ClassName = TEXT("StaticMeshActor");
        Candidate.CentroidPixelX = X + Width * 0.5;
        Candidate.CentroidPixelY = Y + Height * 0.5;
        Candidate.ClampedRect = FBox2D(FVector2D(X, Y), FVector2D(X + Width, Y + Height));
        Candidate.ScreenArea = Width * Height;
        Candidate.Distance = Distance;
        Candidate.bCentroidOnScreen = true;
        return Candidate;
    }

    // Options with de-overlap disabled, so ordering/cap tests are not perturbed by it.
    FActorLabelOptions MakeSpreadOptions(int32 MaxLabels)
    {
        FActorLabelOptions Options;
        Options.MaxLabels = MaxLabels;
        Options.MinScreenAreaPx = 0.0;
        Options.MinLabelSpacingPx = 0;
        return Options;
    }
}

// ============================================================================
// Ordering + cap: the cap keeps the LARGEST projected footprints, not the first ones the world
// iterator yielded. If this regresses, a thousands-actor level labels 50 arbitrary specks and the
// discovery mode is useless — which is the whole reason the sort exists.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorLabelCapKeepsLargestTest,
    "PinWright.render.actor_labels.CapKeepsLargestByScreenArea",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorLabelCapKeepsLargestTest::RunTest(const FString& Parameters)
{
    // Deliberately fed in ASCENDING area order so a cap that just takes the first N fails.
    TArray<FActorLabelCandidate> Candidates;
    Candidates.Add(MakeLabelCandidate(TEXT("Tiny"), 0.0, 0.0, 10.0, 10.0));      // 100
    Candidates.Add(MakeLabelCandidate(TEXT("Small"), 100.0, 0.0, 20.0, 20.0));   // 400
    Candidates.Add(MakeLabelCandidate(TEXT("Medium"), 200.0, 0.0, 40.0, 40.0));  // 1600
    Candidates.Add(MakeLabelCandidate(TEXT("Large"), 300.0, 0.0, 80.0, 80.0));   // 6400

    const FActorLabelLayout Layout = BuildLayout(Candidates, MakeSpreadOptions(2));

    TestEqual(TEXT("cap keeps exactly two rows"), Layout.Placements.Num(), 2);
    TestEqual(TEXT("all four passed the visibility filters"), Layout.Stats.TotalMatches, 4);
    TestEqual(TEXT("two were dropped by the cap"), Layout.Stats.CapDropped, 2);
    TestEqual(TEXT("resolved cap is echoed"), Layout.Stats.ResolvedCap, 2);

    if (Layout.Placements.Num() == 2)
    {
        TestEqual(TEXT("largest projected footprint ranks first"),
            Candidates[Layout.Placements[0].CandidateIndex].Name, FString(TEXT("Large")));
        TestEqual(TEXT("second-largest ranks second"),
            Candidates[Layout.Placements[1].CandidateIndex].Name, FString(TEXT("Medium")));
    }
    return true;
}

// Equal areas tie-break by distance, then by name — the determinism contract. TActorIterator's
// order is not guaranteed stable across editor sessions, so without this two identical captures of
// an unchanged scene could return different rows.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorLabelDeterministicTieBreakTest,
    "PinWright.render.actor_labels.TieBreakIsDeterministic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorLabelDeterministicTieBreakTest::RunTest(const FString& Parameters)
{
    TArray<FActorLabelCandidate> Candidates;
    Candidates.Add(MakeLabelCandidate(TEXT("Zulu"), 0.0, 0.0, 20.0, 20.0, /*Distance=*/500.0));
    Candidates.Add(MakeLabelCandidate(TEXT("Bravo"), 40.0, 0.0, 20.0, 20.0, /*Distance=*/500.0));
    Candidates.Add(MakeLabelCandidate(TEXT("Alpha"), 80.0, 0.0, 20.0, 20.0, /*Distance=*/100.0));

    const FActorLabelLayout Layout = BuildLayout(Candidates, MakeSpreadOptions(10));

    TestEqual(TEXT("all three placed"), Layout.Placements.Num(), 3);
    if (Layout.Placements.Num() == 3)
    {
        // Same area everywhere, so distance decides first: Alpha (100) before the two at 500.
        TestEqual(TEXT("nearest wins the area tie"),
            Candidates[Layout.Placements[0].CandidateIndex].Name, FString(TEXT("Alpha")));
        // Then name, case-sensitively ascending: Bravo before Zulu.
        TestEqual(TEXT("name breaks the remaining tie (B before Z)"),
            Candidates[Layout.Placements[1].CandidateIndex].Name, FString(TEXT("Bravo")));
        TestEqual(TEXT("name tie-break is ascending"),
            Candidates[Layout.Placements[2].CandidateIndex].Name, FString(TEXT("Zulu")));
    }
    return true;
}

// ============================================================================
// Behind the camera: rejected outright, never turned into a row. A behind-camera point still
// projects — mirrored, through |W| — so a regression here publishes a plausible-looking pixel that
// points at the wrong thing, which is worse than publishing nothing.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorLabelBehindCameraRejectedTest,
    "PinWright.render.actor_labels.BehindCameraRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorLabelBehindCameraRejectedTest::RunTest(const FString& Parameters)
{
    // Camera at the origin looking down +X (FRotator(0,0,0)).
    const FVector CameraLocation = FVector::ZeroVector;
    const FRotator Forward = FRotator::ZeroRotator;

    TestFalse(TEXT("a point 1000cm ahead is in front"),
        IsBehindCamera(CameraLocation, Forward, FVector(1000.0, 0.0, 0.0)));
    TestTrue(TEXT("a point 1000cm behind is behind"),
        IsBehindCamera(CameraLocation, Forward, FVector(-1000.0, 0.0, 0.0)));
    // Exactly on the camera plane: rejected, because that is where the projection divides by zero.
    TestTrue(TEXT("a point on the camera plane counts as behind"),
        IsBehindCamera(CameraLocation, Forward, FVector(0.0, 500.0, 0.0)));

    // The pose the PIXELS show is what decides. A top-down orthographic capture renders as
    // pitch -90 / yaw 180 (the engine fixes each ortho axis view's in-plane orientation), and the
    // forward vector must still point DOWN — feeding the requested rotation instead of
    // FViewportCaptureOutput::EffectiveRotation is the bug this asserts against.
    const FVector OrthoCamera(0.0, 0.0, 5000.0);
    const FRotator TopDownEffective(-90.0f, 180.0f, 0.0f);
    TestFalse(TEXT("ground below a top-down camera is in front"),
        IsBehindCamera(OrthoCamera, TopDownEffective, FVector(0.0, 0.0, 0.0)));
    TestTrue(TEXT("a point above a top-down camera is behind it"),
        IsBehindCamera(OrthoCamera, TopDownEffective, FVector(0.0, 0.0, 9000.0)));

    // ...and BuildLayout must turn the flag into a rejection, not a row.
    TArray<FActorLabelCandidate> Candidates;
    Candidates.Add(MakeLabelCandidate(TEXT("Visible"), 0.0, 0.0, 40.0, 40.0));
    FActorLabelCandidate Behind = MakeLabelCandidate(TEXT("Behind"), 100.0, 0.0, 80.0, 80.0);
    Behind.bBehindCamera = true; // bigger on screen, so only the rejection can keep it out
    Candidates.Add(Behind);

    const FActorLabelLayout Layout = BuildLayout(Candidates, MakeSpreadOptions(10));

    TestEqual(TEXT("only the in-front actor is placed"), Layout.Placements.Num(), 1);
    TestEqual(TEXT("the behind-camera actor is counted, not silently gone"),
        Layout.Stats.BehindCamera, 1);
    if (Layout.Placements.Num() == 1)
    {
        TestEqual(TEXT("the placed row is the in-front actor"),
            Candidates[Layout.Placements[0].CandidateIndex].Name, FString(TEXT("Visible")));
    }
    return true;
}

// ============================================================================
// Screen-area threshold and off-frame rejection are distinguished in `dropped`.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorLabelScreenAreaThresholdTest,
    "PinWright.render.actor_labels.ScreenAreaThresholdAndOffScreen",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorLabelScreenAreaThresholdTest::RunTest(const FString& Parameters)
{
    TArray<FActorLabelCandidate> Candidates;
    Candidates.Add(MakeLabelCandidate(TEXT("BigEnough"), 0.0, 0.0, 40.0, 40.0)); // 1600 px^2
    Candidates.Add(MakeLabelCandidate(TEXT("TooSmall"), 100.0, 0.0, 5.0, 5.0));  // 25 px^2

    // No overlap with the frame at all: an invalid clamped rect.
    FActorLabelCandidate OffFrame;
    OffFrame.Name = TEXT("OffFrame");
    OffFrame.Label = OffFrame.Name;
    Candidates.Add(OffFrame);

    FActorLabelOptions Options = MakeSpreadOptions(10);
    Options.MinScreenAreaPx = 256.0;

    const FActorLabelLayout Layout = BuildLayout(Candidates, Options);

    TestEqual(TEXT("only the big actor is placed"), Layout.Placements.Num(), 1);
    TestEqual(TEXT("the small actor is counted under belowMinScreenArea"),
        Layout.Stats.BelowMinScreenArea, 1);
    TestEqual(TEXT("the off-frame actor is counted under offScreen"), Layout.Stats.OffScreen, 1);
    TestEqual(TEXT("nothing was miscounted as behind the camera"), Layout.Stats.BehindCamera, 0);
    return true;
}

// ============================================================================
// De-overlap: a second label anchored within minSpacing is not PAINTED, but its row survives.
// The row surviving is the contract that keeps the machine-readable map complete on a crowded
// frame; losing it to a drawing decision would be the real bug.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorLabelOverlapSkipKeepsRowTest,
    "PinWright.render.actor_labels.OverlapSkipStillEmitsRow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorLabelOverlapSkipKeepsRowTest::RunTest(const FString& Parameters)
{
    TArray<FActorLabelCandidate> Candidates;
    Candidates.Add(MakeLabelCandidate(TEXT("First"), 100.0, 100.0, 60.0, 60.0));  // 3600, ranks 1st
    Candidates.Add(MakeLabelCandidate(TEXT("Crowded"), 104.0, 103.0, 40.0, 40.0)); // anchor 5px away
    Candidates.Add(MakeLabelCandidate(TEXT("Apart"), 400.0, 400.0, 30.0, 30.0));   // far anchor

    FActorLabelOptions Options;
    Options.MaxLabels = 10;
    Options.MinScreenAreaPx = 0.0;
    Options.MinLabelSpacingPx = 24;

    const FActorLabelLayout Layout = BuildLayout(Candidates, Options);

    TestEqual(TEXT("every eligible actor still gets a row"), Layout.Placements.Num(), 3);
    TestEqual(TEXT("two labels are painted"), Layout.Stats.Drawn, 2);
    TestEqual(TEXT("the crowded label is skipped for painting only"), Layout.Stats.OverlapSkipped, 1);
    TestEqual(TEXT("no row was lost to the cap"), Layout.Stats.CapDropped, 0);

    for (const FActorLabelPlacement& Placement : Layout.Placements)
    {
        if (Candidates[Placement.CandidateIndex].Name == TEXT("Crowded"))
        {
            TestFalse(TEXT("the crowded row reports drawn:false"), Placement.bDrawn);
        }
    }

    // draw:false paints nothing at all, but still returns every row.
    Options.bDraw = false;
    const FActorLabelLayout NoDraw = BuildLayout(Candidates, Options);
    TestEqual(TEXT("draw:false still emits every row"), NoDraw.Placements.Num(), 3);
    TestEqual(TEXT("draw:false paints nothing"), NoDraw.Stats.Drawn, 0);
    TestEqual(TEXT("draw:false does not report overlap skips"), NoDraw.Stats.OverlapSkipped, 0);
    return true;
}

// The label anchor rides the CLAMPED rect's top-left, so it is inside the frame even for an actor
// whose centroid projected outside it — otherwise a half-visible edge actor's label lands off-image.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorLabelAnchorInsideFrameTest,
    "PinWright.render.actor_labels.AnchorRidesClampedRect",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorLabelAnchorInsideFrameTest::RunTest(const FString& Parameters)
{
    // Bounds cross the left edge: the handler clamps to x>=0, and the centroid is off-image.
    FActorLabelCandidate Edge = MakeLabelCandidate(TEXT("EdgeActor"), 0.0, 30.0, 50.0, 50.0);
    Edge.CentroidPixelX = -120.0;
    Edge.bCentroidOnScreen = false;

    TArray<FActorLabelCandidate> Candidates;
    Candidates.Add(Edge);

    const FActorLabelLayout Layout = BuildLayout(Candidates, MakeSpreadOptions(10));

    TestEqual(TEXT("an edge actor whose bounds cross the frame is included"),
        Layout.Placements.Num(), 1);
    if (Layout.Placements.Num() == 1)
    {
        TestEqual(TEXT("anchor X is the clamped rect min, not the off-image centroid"),
            Layout.Placements[0].AnchorX, 0);
        TestEqual(TEXT("anchor Y is the clamped rect min"), Layout.Placements[0].AnchorY, 30);
        TestTrue(TEXT("the edge actor is still painted"), Layout.Placements[0].bDrawn);
    }
    return true;
}

// ============================================================================
// Accounting invariants — the contract documented on FActorLabelStats and in render.md.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorLabelStatsInvariantTest,
    "PinWright.render.actor_labels.StatsInvariant",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorLabelStatsInvariantTest::RunTest(const FString& Parameters)
{
    TArray<FActorLabelCandidate> Candidates;
    for (int32 Index = 0; Index < 6; ++Index)
    {
        Candidates.Add(MakeLabelCandidate(FString::Printf(TEXT("Big_%d"), Index),
            Index * 100.0, 0.0, 50.0, 50.0));
    }
    for (int32 Index = 0; Index < 3; ++Index)
    {
        Candidates.Add(MakeLabelCandidate(FString::Printf(TEXT("Small_%d"), Index),
            Index * 100.0, 200.0, 4.0, 4.0));
    }
    for (int32 Index = 0; Index < 2; ++Index)
    {
        FActorLabelCandidate Behind = MakeLabelCandidate(
            FString::Printf(TEXT("Behind_%d"), Index), Index * 100.0, 400.0, 50.0, 50.0);
        Behind.bBehindCamera = true;
        Candidates.Add(Behind);
    }
    FActorLabelCandidate OffFrame;
    OffFrame.Name = TEXT("OffFrame");
    Candidates.Add(OffFrame);

    FActorLabelOptions Options = MakeSpreadOptions(4);
    Options.MinScreenAreaPx = 256.0;

    const FActorLabelLayout Layout = BuildLayout(Candidates, Options);
    const FActorLabelStats& Stats = Layout.Stats;

    TestEqual(TEXT("scanned counts every candidate handed in"), Stats.Scanned, Candidates.Num());
    TestEqual(TEXT("scanned == behindCamera + offScreen + belowMinScreenArea + totalMatches"),
        Stats.Scanned,
        Stats.BehindCamera + Stats.OffScreen + Stats.BelowMinScreenArea + Stats.TotalMatches);
    TestEqual(TEXT("totalMatches == rows + cap drops"),
        Stats.TotalMatches, Layout.Placements.Num() + Stats.CapDropped);
    TestEqual(TEXT("rows == drawn + overlapSkipped"),
        Layout.Placements.Num(), Stats.Drawn + Stats.OverlapSkipped);
    return true;
}

// maxLabels:0 means "no cap of mine" (actor.list's limit convention) but still stops at the hard
// ceiling, so an uncapped request can never return an unbounded response.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorLabelZeroCapUsesCeilingTest,
    "PinWright.render.actor_labels.ZeroMaxLabelsUsesHardCeiling",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorLabelZeroCapUsesCeilingTest::RunTest(const FString& Parameters)
{
    TArray<FActorLabelCandidate> Candidates;
    Candidates.Add(MakeLabelCandidate(TEXT("Only"), 0.0, 0.0, 40.0, 40.0));

    FActorLabelOptions Options = MakeSpreadOptions(0);
    const FActorLabelLayout Layout = BuildLayout(Candidates, Options);
    TestEqual(TEXT("maxLabels 0 resolves to the hard ceiling"),
        Layout.Stats.ResolvedCap, MaxAllowedLabels);
    TestEqual(TEXT("the single candidate is placed"), Layout.Placements.Num(), 1);

    Options.MaxLabels = MaxAllowedLabels * 10;
    const FActorLabelLayout Clamped = BuildLayout(Candidates, Options);
    TestEqual(TEXT("an over-large maxLabels clamps to the hard ceiling"),
        Clamped.Stats.ResolvedCap, MaxAllowedLabels);
    return true;
}

// ============================================================================
// Name filter: substring by default, wildcard when it carries * or ?, case-insensitive, matching
// the internal name OR the display label.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorLabelNameFilterTest,
    "PinWright.render.actor_labels.NameFilterSubstringAndWildcard",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorLabelNameFilterTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("an empty filter matches everything"),
        MatchesNameFilter(TEXT("StaticMeshActor_7"), TEXT("Tower North"), FString()));

    TestTrue(TEXT("substring matches the internal name, case-insensitively"),
        MatchesNameFilter(TEXT("SM_Tower_North_2"), TEXT("Anything"), TEXT("tower")));
    TestTrue(TEXT("substring also matches the display label"),
        MatchesNameFilter(TEXT("StaticMeshActor_7"), TEXT("Tower North"), TEXT("north")));
    TestFalse(TEXT("a non-matching substring is rejected"),
        MatchesNameFilter(TEXT("SM_Wall_1"), TEXT("Wall"), TEXT("tower")));

    // A '*' or '?' promotes the whole filter to a wildcard pattern (asset.search's rule), so it
    // must anchor rather than behave like a substring.
    TestTrue(TEXT("wildcard matches a full-name pattern"),
        MatchesNameFilter(TEXT("SM_Tower_North_2"), TEXT("x"), TEXT("SM_*_North_?")));
    TestFalse(TEXT("a wildcard pattern is not a substring test"),
        MatchesNameFilter(TEXT("SM_Tower_North_2"), TEXT("x"), TEXT("Tower*")));
    TestTrue(TEXT("wildcard matching is case-insensitive"),
        MatchesNameFilter(TEXT("SM_Tower_North_2"), TEXT("x"), TEXT("sm_*")));
    return true;
}

// Empty input yields an empty layout with zeroed stats (mirrors the Set-of-Mark empty-input test).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorLabelEmptyInputTest,
    "PinWright.render.actor_labels.EmptyInput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorLabelEmptyInputTest::RunTest(const FString& Parameters)
{
    const TArray<FActorLabelCandidate> Candidates;
    const FActorLabelLayout Layout = BuildLayout(Candidates, FActorLabelOptions());

    TestEqual(TEXT("no placements"), Layout.Placements.Num(), 0);
    TestEqual(TEXT("nothing scanned"), Layout.Stats.Scanned, 0);
    TestEqual(TEXT("nothing matched"), Layout.Stats.TotalMatches, 0);
    TestEqual(TEXT("nothing dropped by the cap"), Layout.Stats.CapDropped, 0);
    TestEqual(TEXT("the default cap is echoed"), Layout.Stats.ResolvedCap, DefaultMaxLabels);
    return true;
}
