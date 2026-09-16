// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for the pure helpers of FDriveInput: the drag-interpolation step
// generator (ComputeDragStepPoints) and the character-to-key/shift mapping
// (MapCharToKey). The live Slate injection is covered by later integration
// tests; these touch no FSlateApplication and spawn no windows.

#include "Misc/AutomationTest.h"

#include "InputCoreTypes.h"
#include "Math/Vector2D.h"
#include "Handlers/Drive/DriveInput.h"

// ============================================================================
// ComputeDragStepPoints: step counts. Floor of 5 for sub-frame durations,
// scales up with longer durations, and DurationMs <= 0 falls back to the
// default duration (same count as an explicit 200 ms).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveInputDragStepCountTest,
    "PinWright.drive.input.DragStepCount",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveInputDragStepCountTest::RunTest(const FString& Parameters)
{
    const FVector2D From(100.0, 100.0);
    const FVector2D To(300.0, 220.0);

    // Sub-frame durations clamp to the 5-step floor (1 ms and 16 ms are both
    // under one 60 Hz frame).
    TestEqual(TEXT("1ms -> 5 steps"), FDriveInput::ComputeDragStepPoints(From, To, 1).Num(), 5);
    TestEqual(TEXT("16ms -> 5 steps"), FDriveInput::ComputeDragStepPoints(From, To, 16).Num(), 5);

    // Longer drags emit more steps than short ones.
    const int32 ShortCount = FDriveInput::ComputeDragStepPoints(From, To, 16).Num();
    const int32 LongCount = FDriveInput::ComputeDragStepPoints(From, To, 600).Num();
    TestTrue(TEXT("600ms emits more steps than 16ms"), LongCount > ShortCount);

    // DurationMs <= 0 uses the default duration: same count as an explicit 200 ms.
    const int32 DefaultCount = FDriveInput::ComputeDragStepPoints(From, To, 200).Num();
    TestEqual(TEXT("0ms uses default duration"), FDriveInput::ComputeDragStepPoints(From, To, 0).Num(), DefaultCount);
    TestEqual(TEXT("negative uses default duration"), FDriveInput::ComputeDragStepPoints(From, To, -50).Num(), DefaultCount);
    TestTrue(TEXT("default count is at least the floor"), DefaultCount >= 5);

    return true;
}

// ============================================================================
// ComputeDragStepPoints: the path excludes From, ends exactly at To, and
// advances monotonically (distance from From strictly increases).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveInputDragStepPathTest,
    "PinWright.drive.input.DragStepPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveInputDragStepPathTest::RunTest(const FString& Parameters)
{
    const FVector2D From(100.0, 100.0);
    const FVector2D To(300.0, 200.0);

    const TArray<FVector2D> Points = FDriveInput::ComputeDragStepPoints(From, To, 200);

    TestTrue(TEXT("non-empty"), Points.Num() > 0);

    // Last point lands exactly on the target.
    TestTrue(TEXT("last point equals To"), Points.Last().Equals(To, KINDA_SMALL_NUMBER));

    // First point is past From (From itself is the press point and excluded).
    TestTrue(TEXT("first point past From"), FVector2D::Distance(Points[0], From) > 0.0);

    // Strictly monotonic progress away from From toward To.
    bool bMonotonic = true;
    double PrevDist = -1.0;
    for (const FVector2D& P : Points)
    {
        const double Dist = FVector2D::Distance(P, From);
        if (Dist <= PrevDist)
        {
            bMonotonic = false;
            break;
        }
        PrevDist = Dist;
    }
    TestTrue(TEXT("distance from From strictly increases"), bMonotonic);

    return true;
}

// ============================================================================
// ComputeDragStepPoints: a zero-length drag (From == To) still yields the
// step floor, with every point sitting on the shared point.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveInputDragStepDegenerateTest,
    "PinWright.drive.input.DragStepDegenerate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveInputDragStepDegenerateTest::RunTest(const FString& Parameters)
{
    const FVector2D P(50.0, 75.0);
    const TArray<FVector2D> Points = FDriveInput::ComputeDragStepPoints(P, P, 33);

    TestTrue(TEXT("at least the floor of steps"), Points.Num() >= 5);

    bool bAllAtP = true;
    for (const FVector2D& Pt : Points)
    {
        if (!Pt.Equals(P, KINDA_SMALL_NUMBER))
        {
            bAllAtP = false;
            break;
        }
    }
    TestTrue(TEXT("all points sit on the shared point"), bAllAtP);

    return true;
}

// ============================================================================
// MapCharToKey: letters resolve to the physical letter key, with Shift only
// for uppercase.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveInputCharMapLettersTest,
    "PinWright.drive.input.CharMapLetters",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveInputCharMapLettersTest::RunTest(const FString& Parameters)
{
    const FDriveCharKeyMapping LowerA = FDriveInput::MapCharToKey(TEXT('a'));
    TestTrue(TEXT("'a' -> EKeys::A"), LowerA.Key == EKeys::A);
    TestFalse(TEXT("'a' no shift"), LowerA.bShift);

    const FDriveCharKeyMapping UpperA = FDriveInput::MapCharToKey(TEXT('A'));
    TestTrue(TEXT("'A' -> EKeys::A"), UpperA.Key == EKeys::A);
    TestTrue(TEXT("'A' shift"), UpperA.bShift);

    const FDriveCharKeyMapping LowerZ = FDriveInput::MapCharToKey(TEXT('z'));
    TestTrue(TEXT("'z' -> EKeys::Z"), LowerZ.Key == EKeys::Z);
    TestFalse(TEXT("'z' no shift"), LowerZ.bShift);

    const FDriveCharKeyMapping UpperM = FDriveInput::MapCharToKey(TEXT('M'));
    TestTrue(TEXT("'M' -> EKeys::M"), UpperM.Key == EKeys::M);
    TestTrue(TEXT("'M' shift"), UpperM.bShift);

    return true;
}

// ============================================================================
// MapCharToKey: digits, shifted number-row symbols, punctuation, and space.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveInputCharMapSymbolsTest,
    "PinWright.drive.input.CharMapSymbols",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveInputCharMapSymbolsTest::RunTest(const FString& Parameters)
{
    // Plain digit: number-row key, no shift.
    const FDriveCharKeyMapping Five = FDriveInput::MapCharToKey(TEXT('5'));
    TestTrue(TEXT("'5' -> EKeys::Five"), Five.Key == EKeys::Five);
    TestFalse(TEXT("'5' no shift"), Five.bShift);

    // Shifted number-row symbol: shares the digit's physical key, with shift.
    const FDriveCharKeyMapping Bang = FDriveInput::MapCharToKey(TEXT('!'));
    TestTrue(TEXT("'!' -> EKeys::One"), Bang.Key == EKeys::One);
    TestTrue(TEXT("'!' shift"), Bang.bShift);

    const FDriveCharKeyMapping At = FDriveInput::MapCharToKey(TEXT('@'));
    TestTrue(TEXT("'@' -> EKeys::Two"), At.Key == EKeys::Two);
    TestTrue(TEXT("'@' shift"), At.bShift);

    // Punctuation: unshifted and its shifted variant share a physical key.
    const FDriveCharKeyMapping Period = FDriveInput::MapCharToKey(TEXT('.'));
    TestTrue(TEXT("'.' -> EKeys::Period"), Period.Key == EKeys::Period);
    TestFalse(TEXT("'.' no shift"), Period.bShift);

    const FDriveCharKeyMapping Question = FDriveInput::MapCharToKey(TEXT('?'));
    TestTrue(TEXT("'?' -> EKeys::Slash"), Question.Key == EKeys::Slash);
    TestTrue(TEXT("'?' shift"), Question.bShift);

    const FDriveCharKeyMapping Underscore = FDriveInput::MapCharToKey(TEXT('_'));
    TestTrue(TEXT("'_' -> EKeys::Hyphen"), Underscore.Key == EKeys::Hyphen);
    TestTrue(TEXT("'_' shift"), Underscore.bShift);

    // Space resolves to the space bar.
    const FDriveCharKeyMapping Space = FDriveInput::MapCharToKey(TEXT(' '));
    TestTrue(TEXT("' ' -> EKeys::SpaceBar"), Space.Key == EKeys::SpaceBar);
    TestFalse(TEXT("' ' no shift"), Space.bShift);

    return true;
}

// ============================================================================
// MapCharToKey: a character with no US-layout physical key returns an invalid
// key (the caller routes it through the character event alone).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveInputCharMapUnmappedTest,
    "PinWright.drive.input.CharMapUnmapped",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveInputCharMapUnmappedTest::RunTest(const FString& Parameters)
{
    // A non-ASCII letter has no dedicated US-layout key in the mapping.
    const FDriveCharKeyMapping Euro = FDriveInput::MapCharToKey(TCHAR(0x20AC)); // EURO SIGN
    TestFalse(TEXT("unmapped char -> invalid key"), Euro.Key.IsValid());
    TestFalse(TEXT("unmapped char -> no shift"), Euro.bShift);

    return true;
}
