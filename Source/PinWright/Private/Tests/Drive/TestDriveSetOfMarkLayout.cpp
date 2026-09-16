// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for FDriveSetOfMarkLayout: pure Set-of-Mark layout math --
// numbering, frame clamping, offscreen / too-small / cap omission, empty input.

#include "Misc/AutomationTest.h"

#include "Handlers/Drive/DriveSetOfMarkLayout.h"
#include "Handlers/Drive/DriveTypes.h"

namespace
{
    // Builds an interactable element with the given handle and absolute rect.
    FDriveElement MakeMarkElement(const FString& Handle, double X, double Y, double W, double H)
    {
        FDriveElement Element;
        Element.Handle = Handle;
        Element.AbsolutePosition = FVector2D(X, Y);
        Element.AbsoluteSize = FVector2D(W, H);
        return Element;
    }
}

// ============================================================================
// Test: in-bounds elements get sequential numbers, exact boxes, anchors, handles
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveSetOfMarkInBoundsTest,
    "PinWright.drive.setofmark.NumbersAndBoxesForInBounds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveSetOfMarkInBoundsTest::RunTest(const FString& Parameters)
{
    TArray<FDriveElement> Elements;
    Elements.Add(MakeMarkElement(TEXT("a"), 10.0, 20.0, 100.0, 40.0));
    Elements.Add(MakeMarkElement(TEXT("b"), 200.0, 50.0, 80.0, 30.0));
    Elements.Add(MakeMarkElement(TEXT("c"), 400.0, 300.0, 120.0, 60.0));

    const FDriveMarkLayout Layout = FDriveSetOfMarkLayout::BuildLayout(Elements, 800, 600, 10);

    TestEqual(TEXT("three marks"), Layout.Marks.Num(), 3);
    TestEqual(TEXT("nothing omitted"), Layout.Omitted.Num(), 0);
    TestEqual(TEXT("omitted count zero"), Layout.OmittedCount, 0);

    if (Layout.Marks.Num() == 3)
    {
        TestEqual(TEXT("mark 1 number"), Layout.Marks[0].Number, 1);
        TestEqual(TEXT("mark 1 handle"), Layout.Marks[0].ElementHandle, TEXT("a"));
        TestEqual(TEXT("mark 1 box min x"), Layout.Marks[0].Box.Min.X, 10.0);
        TestEqual(TEXT("mark 1 box min y"), Layout.Marks[0].Box.Min.Y, 20.0);
        TestEqual(TEXT("mark 1 box max x"), Layout.Marks[0].Box.Max.X, 110.0);
        TestEqual(TEXT("mark 1 box max y"), Layout.Marks[0].Box.Max.Y, 60.0);
        TestTrue(TEXT("mark 1 box valid"), Layout.Marks[0].Box.bIsValid);
        TestEqual(TEXT("mark 1 anchor x"), Layout.Marks[0].LabelAnchor.X, 10.0);
        TestEqual(TEXT("mark 1 anchor y"), Layout.Marks[0].LabelAnchor.Y, 20.0);

        TestEqual(TEXT("mark 2 number"), Layout.Marks[1].Number, 2);
        TestEqual(TEXT("mark 2 handle"), Layout.Marks[1].ElementHandle, TEXT("b"));
        TestEqual(TEXT("mark 2 box max x"), Layout.Marks[1].Box.Max.X, 280.0);

        TestEqual(TEXT("mark 3 number"), Layout.Marks[2].Number, 3);
        TestEqual(TEXT("mark 3 handle"), Layout.Marks[2].ElementHandle, TEXT("c"));
        TestEqual(TEXT("mark 3 anchor x"), Layout.Marks[2].LabelAnchor.X, 400.0);
    }

    return true;
}

// ============================================================================
// Test: a partially-offscreen element is clamped to the frame bounds
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveSetOfMarkClampTest,
    "PinWright.drive.setofmark.ClampsPartiallyOffscreen",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveSetOfMarkClampTest::RunTest(const FString& Parameters)
{
    // Rect straddles the left and top edges: x from -20..80, y from -10..40.
    TArray<FDriveElement> Elements;
    Elements.Add(MakeMarkElement(TEXT("edge"), -20.0, -10.0, 100.0, 50.0));

    const FDriveMarkLayout Layout = FDriveSetOfMarkLayout::BuildLayout(Elements, 800, 600, 10);

    TestEqual(TEXT("one mark"), Layout.Marks.Num(), 1);
    TestEqual(TEXT("nothing omitted"), Layout.OmittedCount, 0);

    if (Layout.Marks.Num() == 1)
    {
        const FDriveMark& Mark = Layout.Marks[0];
        TestEqual(TEXT("number is 1"), Mark.Number, 1);
        // Min clamped up to the frame origin.
        TestEqual(TEXT("clamped min x"), Mark.Box.Min.X, 0.0);
        TestEqual(TEXT("clamped min y"), Mark.Box.Min.Y, 0.0);
        // Max unchanged (still inside the frame).
        TestEqual(TEXT("max x preserved"), Mark.Box.Max.X, 80.0);
        TestEqual(TEXT("max y preserved"), Mark.Box.Max.Y, 40.0);
        // Anchor rides the clamped top-left, inside the frame.
        TestEqual(TEXT("anchor x"), Mark.LabelAnchor.X, 0.0);
        TestEqual(TEXT("anchor y"), Mark.LabelAnchor.Y, 0.0);
    }

    return true;
}

// ============================================================================
// Test: a fully-offscreen element is omitted (no overlap with the frame)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveSetOfMarkOffscreenTest,
    "PinWright.drive.setofmark.OmitsFullyOffscreen",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveSetOfMarkOffscreenTest::RunTest(const FString& Parameters)
{
    // Entirely to the right of an 800-wide frame.
    TArray<FDriveElement> Elements;
    Elements.Add(MakeMarkElement(TEXT("gone"), 900.0, 100.0, 50.0, 50.0));

    const FDriveMarkLayout Layout = FDriveSetOfMarkLayout::BuildLayout(Elements, 800, 600, 10);

    TestEqual(TEXT("no marks"), Layout.Marks.Num(), 0);
    TestEqual(TEXT("one omitted"), Layout.Omitted.Num(), 1);
    TestEqual(TEXT("omitted count one"), Layout.OmittedCount, 1);
    if (Layout.Omitted.Num() == 1)
    {
        TestEqual(TEXT("omitted number is 1"), Layout.Omitted[0], 1);
    }

    return true;
}

// ============================================================================
// Test: an element below the minimum visible size after clamping is omitted
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveSetOfMarkTooSmallTest,
    "PinWright.drive.setofmark.OmitsTooSmall",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveSetOfMarkTooSmallTest::RunTest(const FString& Parameters)
{
    // Fully in-bounds but only 2x2 px -- below MinVisibleSizePx (8.0).
    TArray<FDriveElement> Elements;
    Elements.Add(MakeMarkElement(TEXT("tiny"), 5.0, 5.0, 2.0, 2.0));

    const FDriveMarkLayout Layout = FDriveSetOfMarkLayout::BuildLayout(Elements, 800, 600, 10);

    TestEqual(TEXT("no marks"), Layout.Marks.Num(), 0);
    TestEqual(TEXT("omitted count one"), Layout.OmittedCount, 1);
    if (Layout.Omitted.Num() == 1)
    {
        TestEqual(TEXT("omitted number is 1"), Layout.Omitted[0], 1);
    }

    return true;
}

// ============================================================================
// Test: cap marks the first N elements and omits the rest (no silent truncation)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveSetOfMarkCapTest,
    "PinWright.drive.setofmark.CapMarksFirstNOmitsRest",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveSetOfMarkCapTest::RunTest(const FString& Parameters)
{
    // Five in-bounds elements, cap of 3.
    TArray<FDriveElement> Elements;
    for (int32 I = 0; I < 5; ++I)
    {
        Elements.Add(MakeMarkElement(FString::Printf(TEXT("el_%d"), I),
            10.0 + I * 100.0, 20.0, 80.0, 40.0));
    }

    const FDriveMarkLayout Layout = FDriveSetOfMarkLayout::BuildLayout(Elements, 800, 600, 3);

    TestEqual(TEXT("three marks"), Layout.Marks.Num(), 3);
    TestEqual(TEXT("two omitted"), Layout.Omitted.Num(), 2);
    TestEqual(TEXT("omitted count two"), Layout.OmittedCount, 2);

    if (Layout.Marks.Num() == 3)
    {
        TestEqual(TEXT("first mark number"), Layout.Marks[0].Number, 1);
        TestEqual(TEXT("last mark number"), Layout.Marks[2].Number, 3);
    }
    if (Layout.Omitted.Num() == 2)
    {
        TestEqual(TEXT("omitted 4"), Layout.Omitted[0], 4);
        TestEqual(TEXT("omitted 5"), Layout.Omitted[1], 5);
    }

    return true;
}

// ============================================================================
// Test: empty input yields an empty layout
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveSetOfMarkEmptyTest,
    "PinWright.drive.setofmark.EmptyInput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveSetOfMarkEmptyTest::RunTest(const FString& Parameters)
{
    const TArray<FDriveElement> Elements;
    const FDriveMarkLayout Layout = FDriveSetOfMarkLayout::BuildLayout(Elements, 800, 600, 10);

    TestEqual(TEXT("no marks"), Layout.Marks.Num(), 0);
    TestEqual(TEXT("no omitted"), Layout.Omitted.Num(), 0);
    TestEqual(TEXT("omitted count zero"), Layout.OmittedCount, 0);

    return true;
}
