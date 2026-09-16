// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for FDriveSetOfMarkRenderer::DrawMarks -- the pure Set-of-Mark paint
// over a synthetic FColor buffer. Covers box-outline pixels, untouched interior,
// digit pixels near the LabelAnchor, and the empty-layout no-op. The live viewport
// capture path (CaptureAnnotated) is not exercised here -- it needs a real game
// viewport and belongs in an integration test.

#include "Misc/AutomationTest.h"

#include "Handlers/Drive/DriveSetOfMarkRenderer.h"
#include "Handlers/Drive/DriveSetOfMarkLayout.h"
#include "Handlers/Drive/DriveTypes.h"

namespace
{
    constexpr int32 TW = 320;
    constexpr int32 TH = 240;

    // Background distinct from both the mark color and the label ink.
    const FColor TBackground(10, 10, 10, 255);

    TArray<FColor> MakeBlank(const FColor& Fill)
    {
        TArray<FColor> Px;
        Px.Init(Fill, TW * TH);
        return Px;
    }

    FColor At(const TArray<FColor>& Px, int32 X, int32 Y)
    {
        return Px[Y * TW + X];
    }

    FDriveMark MakeMark(int32 Number, double MinX, double MinY, double MaxX, double MaxY,
        double AnchorX, double AnchorY)
    {
        FDriveMark Mark;
        Mark.Number = Number;
        Mark.Box = FBox2D(FVector2D(MinX, MinY), FVector2D(MaxX, MaxY));
        Mark.LabelAnchor = FVector2D(AnchorX, AnchorY);
        return Mark;
    }
}

// ============================================================================
// Test: the box outline is stroked at the rect edges and the interior is untouched
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveSomRenderBoxBorderTest,
    "PinWright.drive.somrender.DrawsBoxBorder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveSomRenderBoxBorderTest::RunTest(const FString& Parameters)
{
    TArray<FColor> Px = MakeBlank(TBackground);

    // Box [50,50]-[250,200]; label badge anchored at the top-left corner so it sits
    // clear of the edges and interior point checked below.
    FDriveMarkLayout Layout;
    Layout.Marks.Add(MakeMark(7, 50.0, 50.0, 250.0, 200.0, 50.0, 50.0));

    FDriveSetOfMarkRenderer::DrawMarks(Px, TW, TH, Layout);

    const FColor Mark = FDriveSetOfMarkRenderer::MarkColor();

    // Border pixels at the four rect edges (mid-edge, away from the corner badge).
    TestTrue(TEXT("top edge stroked"), At(Px, 150, 50) == Mark);
    TestTrue(TEXT("bottom edge stroked"), At(Px, 150, 200) == Mark);
    TestTrue(TEXT("left edge stroked"), At(Px, 50, 125) == Mark);
    TestTrue(TEXT("right edge stroked"), At(Px, 250, 125) == Mark);

    // Interior well inside the 2px border (and below the badge) stays background.
    TestTrue(TEXT("interior untouched"), At(Px, 150, 125) == TBackground);
    // A pixel fully outside the box is untouched too.
    TestTrue(TEXT("outside box untouched"), At(Px, 10, 10) == TBackground);

    return true;
}

// ============================================================================
// Test: the mark number renders as digit ink on a badge near the LabelAnchor
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveSomRenderLabelDigitsTest,
    "PinWright.drive.somrender.DrawsLabelDigits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveSomRenderLabelDigitsTest::RunTest(const FString& Parameters)
{
    TArray<FColor> Px = MakeBlank(TBackground);

    // Two-digit number so both glyph cells carry ink; anchor in open space.
    const int32 AnchorX = 40;
    const int32 AnchorY = 40;
    FDriveMarkLayout Layout;
    Layout.Marks.Add(MakeMark(18, 40.0, 40.0, 240.0, 200.0,
        static_cast<double>(AnchorX), static_cast<double>(AnchorY)));

    FDriveSetOfMarkRenderer::DrawMarks(Px, TW, TH, Layout);

    const FColor Mark = FDriveSetOfMarkRenderer::MarkColor();
    const FColor Ink = FDriveSetOfMarkRenderer::LabelColor();

    // Scan a generous region anchored at the LabelAnchor and count badge vs ink
    // pixels. Both must be present: badge proves the panel painted, ink proves the
    // embedded font glyphs rendered on top (not just a solid rectangle).
    int32 BadgePixels = 0;
    int32 InkPixels = 0;
    for (int32 Y = AnchorY; Y < AnchorY + 24; ++Y)
    {
        for (int32 X = AnchorX; X < AnchorX + 36; ++X)
        {
            const FColor C = At(Px, X, Y);
            if (C == Mark)
            {
                ++BadgePixels;
            }
            else if (C == Ink)
            {
                ++InkPixels;
            }
        }
    }

    TestTrue(TEXT("badge painted near anchor"), BadgePixels > 0);
    TestTrue(TEXT("digit ink painted near anchor"), InkPixels > 0);

    // Far interior of the box stays background (label is anchored top-left only).
    TestTrue(TEXT("box center untouched"), At(Px, 140, 120) == TBackground);

    return true;
}

// ============================================================================
// Test: an empty layout leaves the buffer byte-for-byte unchanged
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveSomRenderEmptyLayoutTest,
    "PinWright.drive.somrender.EmptyLayoutLeavesBufferUnchanged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveSomRenderEmptyLayoutTest::RunTest(const FString& Parameters)
{
    // Seed a recognizable pattern so any stray write would be detectable.
    TArray<FColor> Px;
    Px.Reserve(TW * TH);
    for (int32 I = 0; I < TW * TH; ++I)
    {
        Px.Add(FColor(I % 256, (I / 2) % 256, (I / 3) % 256, 255));
    }
    const TArray<FColor> Original = Px;

    const FDriveMarkLayout EmptyLayout;
    FDriveSetOfMarkRenderer::DrawMarks(Px, TW, TH, EmptyLayout);

    bool bUnchanged = (Px.Num() == Original.Num());
    for (int32 I = 0; bUnchanged && I < Px.Num(); ++I)
    {
        if (!(Px[I] == Original[I]))
        {
            bUnchanged = false;
        }
    }
    TestTrue(TEXT("empty layout leaves buffer unchanged"), bUnchanged);

    return true;
}
