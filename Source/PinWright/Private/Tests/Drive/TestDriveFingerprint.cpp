// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for the pure-logic drive UI fingerprint, equality helper, and per-handle diff
// (FDriveChangeDetector). No live editor / Slate / CEF dependency - everything runs on plain
// TArray<FDriveElement> value sets.

#include "Misc/AutomationTest.h"

#include "Handlers/Drive/DriveTypes.h"
#include "Handlers/Drive/DriveFingerprint.h"

namespace
{
    // Builds a drive element with just the fields the fingerprint/diff care about.
    FDriveElement MakeElement(const FString& Handle, const FString& Type,
        double X, double Y, double W, double H, bool bVisible = true)
    {
        FDriveElement Element;
        Element.Handle = Handle;
        Element.Type = Type;
        Element.AbsolutePosition = FVector2D(X, Y);
        Element.AbsoluteSize = FVector2D(W, H);
        Element.bVisible = bVisible;
        return Element;
    }
}

// ============================================================================
// Fingerprint: identical input yields an identical fingerprint
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveFingerprintStableTest,
    "PinWright.drive.fingerprint.SameInputSameFingerprint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveFingerprintStableTest::RunTest(const FString& Parameters)
{
    TArray<FDriveElement> Elements;
    Elements.Add(MakeElement(TEXT("a"), TEXT("Button"), 10, 20, 100, 40));
    Elements.Add(MakeElement(TEXT("b"), TEXT("Text"), 0, 0, 50, 12));

    const FDriveFingerprint First = FDriveChangeDetector::Compute(Elements);
    const FDriveFingerprint Second = FDriveChangeDetector::Compute(Elements);

    TestTrue(TEXT("identical input gives identical fingerprint"), First == Second);
    TestTrue(TEXT("Equals helper agrees"), FDriveChangeDetector::Equals(First, Second));
    TestEqual(TEXT("visible count is 2"), First.VisibleCount, 2);

    return true;
}

// ============================================================================
// Fingerprint: reordering elements alone does not change the fingerprint
// (documented choice: the fingerprint is order-independent)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveFingerprintReorderTest,
    "PinWright.drive.fingerprint.ReorderDoesNotChangeFingerprint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveFingerprintReorderTest::RunTest(const FString& Parameters)
{
    TArray<FDriveElement> Ordered;
    Ordered.Add(MakeElement(TEXT("a"), TEXT("Button"), 10, 20, 100, 40));
    Ordered.Add(MakeElement(TEXT("b"), TEXT("Text"), 0, 0, 50, 12));
    Ordered.Add(MakeElement(TEXT("c"), TEXT("Image"), 5, 5, 16, 16));

    TArray<FDriveElement> Reordered;
    Reordered.Add(Ordered[2]);
    Reordered.Add(Ordered[0]);
    Reordered.Add(Ordered[1]);

    const FDriveFingerprint A = FDriveChangeDetector::Compute(Ordered);
    const FDriveFingerprint B = FDriveChangeDetector::Compute(Reordered);

    TestTrue(TEXT("reordering the same set yields the same fingerprint"), A == B);

    return true;
}

// ============================================================================
// Fingerprint: a changed rect yields a different fingerprint
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveFingerprintRectChangeTest,
    "PinWright.drive.fingerprint.ChangedRectChangesFingerprint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveFingerprintRectChangeTest::RunTest(const FString& Parameters)
{
    TArray<FDriveElement> Before;
    Before.Add(MakeElement(TEXT("a"), TEXT("Button"), 10, 20, 100, 40));

    TArray<FDriveElement> After;
    After.Add(MakeElement(TEXT("a"), TEXT("Button"), 10, 25, 100, 40)); // moved 5px in Y

    const FDriveFingerprint A = FDriveChangeDetector::Compute(Before);
    const FDriveFingerprint B = FDriveChangeDetector::Compute(After);

    TestTrue(TEXT("a moved rect changes the fingerprint"), A != B);
    TestEqual(TEXT("visible count unchanged"), A.VisibleCount, B.VisibleCount);

    return true;
}

// ============================================================================
// Fingerprint: sub-pixel jitter (rounds to the same whole pixel) is ignored
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveFingerprintSubPixelTest,
    "PinWright.drive.fingerprint.SubPixelJitterIgnored",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveFingerprintSubPixelTest::RunTest(const FString& Parameters)
{
    TArray<FDriveElement> Before;
    Before.Add(MakeElement(TEXT("a"), TEXT("Button"), 10.0, 20.0, 100.0, 40.0));

    TArray<FDriveElement> After;
    After.Add(MakeElement(TEXT("a"), TEXT("Button"), 10.2, 19.8, 100.1, 40.3)); // all round the same

    const FDriveFingerprint A = FDriveChangeDetector::Compute(Before);
    const FDriveFingerprint B = FDriveChangeDetector::Compute(After);

    TestTrue(TEXT("sub-pixel jitter rounds to the same fingerprint"), A == B);

    return true;
}

// ============================================================================
// Fingerprint: a visibility change yields a different fingerprint
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveFingerprintVisibilityTest,
    "PinWright.drive.fingerprint.VisibilityChangeChangesFingerprint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveFingerprintVisibilityTest::RunTest(const FString& Parameters)
{
    TArray<FDriveElement> Before;
    Before.Add(MakeElement(TEXT("a"), TEXT("Button"), 10, 20, 100, 40, /*bVisible*/ true));
    Before.Add(MakeElement(TEXT("b"), TEXT("Text"), 0, 0, 50, 12, /*bVisible*/ true));

    TArray<FDriveElement> After = Before;
    After[1].bVisible = false; // b becomes hidden

    const FDriveFingerprint A = FDriveChangeDetector::Compute(Before);
    const FDriveFingerprint B = FDriveChangeDetector::Compute(After);

    TestTrue(TEXT("hiding an element changes the fingerprint"), A != B);
    TestEqual(TEXT("before visible count is 2"), A.VisibleCount, 2);
    TestEqual(TEXT("after visible count is 1"), B.VisibleCount, 1);

    return true;
}

// ============================================================================
// Fingerprint: invisible elements never contribute (changing a hidden element
// does not change the fingerprint)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveFingerprintHiddenIgnoredTest,
    "PinWright.drive.fingerprint.HiddenElementsIgnored",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveFingerprintHiddenIgnoredTest::RunTest(const FString& Parameters)
{
    TArray<FDriveElement> Before;
    Before.Add(MakeElement(TEXT("a"), TEXT("Button"), 10, 20, 100, 40, /*bVisible*/ true));
    Before.Add(MakeElement(TEXT("h"), TEXT("Panel"), 0, 0, 50, 12, /*bVisible*/ false));

    TArray<FDriveElement> After = Before;
    After[1].AbsolutePosition = FVector2D(999, 999); // move the hidden element

    const FDriveFingerprint A = FDriveChangeDetector::Compute(Before);
    const FDriveFingerprint B = FDriveChangeDetector::Compute(After);

    TestTrue(TEXT("moving a hidden element does not change the fingerprint"), A == B);
    TestEqual(TEXT("only the visible element is counted"), A.VisibleCount, 1);

    return true;
}

// ============================================================================
// Fingerprint: a changed count yields a different fingerprint
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveFingerprintCountTest,
    "PinWright.drive.fingerprint.CountChangeChangesFingerprint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveFingerprintCountTest::RunTest(const FString& Parameters)
{
    TArray<FDriveElement> Before;
    Before.Add(MakeElement(TEXT("a"), TEXT("Button"), 10, 20, 100, 40));

    TArray<FDriveElement> After = Before;
    After.Add(MakeElement(TEXT("b"), TEXT("Text"), 0, 0, 50, 12));

    const FDriveFingerprint A = FDriveChangeDetector::Compute(Before);
    const FDriveFingerprint B = FDriveChangeDetector::Compute(After);

    TestTrue(TEXT("adding an element changes the fingerprint"), A != B);
    TestEqual(TEXT("count goes 1 -> 2"), B.VisibleCount, A.VisibleCount + 1);

    return true;
}

// ============================================================================
// Fingerprint: empty set is well-defined and stable
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveFingerprintEmptyTest,
    "PinWright.drive.fingerprint.EmptySetStable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveFingerprintEmptyTest::RunTest(const FString& Parameters)
{
    const TArray<FDriveElement> Empty;
    const FDriveFingerprint A = FDriveChangeDetector::Compute(Empty);
    const FDriveFingerprint B = FDriveChangeDetector::Compute(Empty);

    TestTrue(TEXT("two empty fingerprints are equal"), A == B);
    TestEqual(TEXT("empty visible count is 0"), A.VisibleCount, 0);

    // An empty set differs from a non-empty one.
    TArray<FDriveElement> One;
    One.Add(MakeElement(TEXT("a"), TEXT("Button"), 0, 0, 1, 1));
    TestTrue(TEXT("empty differs from non-empty"), A != FDriveChangeDetector::Compute(One));

    return true;
}

// ============================================================================
// Diff: appeared, disappeared, and changed are reported by handle
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveDiffBasicTest,
    "PinWright.drive.fingerprint.DiffReportsAppearedDisappearedChanged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveDiffBasicTest::RunTest(const FString& Parameters)
{
    TArray<FDriveElement> Previous;
    Previous.Add(MakeElement(TEXT("a"), TEXT("Button"), 10, 20, 100, 40)); // unchanged
    Previous.Add(MakeElement(TEXT("b"), TEXT("Text"), 0, 0, 50, 12));      // will move
    Previous.Add(MakeElement(TEXT("c"), TEXT("Image"), 5, 5, 16, 16));     // will disappear

    TArray<FDriveElement> Current;
    Current.Add(MakeElement(TEXT("a"), TEXT("Button"), 10, 20, 100, 40)); // identical
    Current.Add(MakeElement(TEXT("b"), TEXT("Text"), 0, 8, 50, 12));      // moved in Y
    Current.Add(MakeElement(TEXT("d"), TEXT("Slider"), 1, 2, 30, 10));    // new

    const FDriveDiff Diff = FDriveChangeDetector::Diff(Previous, Current);

    TestEqual(TEXT("one appeared"), Diff.Appeared.Num(), 1);
    TestEqual(TEXT("appeared is d"), Diff.Appeared.Num() == 1 ? Diff.Appeared[0] : FString(), TEXT("d"));

    TestEqual(TEXT("one disappeared"), Diff.Disappeared.Num(), 1);
    TestEqual(TEXT("disappeared is c"), Diff.Disappeared.Num() == 1 ? Diff.Disappeared[0] : FString(), TEXT("c"));

    TestEqual(TEXT("one changed"), Diff.Changed.Num(), 1);
    TestEqual(TEXT("changed is b"), Diff.Changed.Num() == 1 ? Diff.Changed[0] : FString(), TEXT("b"));

    TestFalse(TEXT("diff is not empty"), Diff.IsEmpty());

    return true;
}

// ============================================================================
// Diff: identical sets produce an empty diff (no false positives)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveDiffNoChangeTest,
    "PinWright.drive.fingerprint.DiffEmptyWhenUnchanged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveDiffNoChangeTest::RunTest(const FString& Parameters)
{
    TArray<FDriveElement> Previous;
    Previous.Add(MakeElement(TEXT("a"), TEXT("Button"), 10, 20, 100, 40));
    Previous.Add(MakeElement(TEXT("b"), TEXT("Text"), 0, 0, 50, 12));

    // Same elements, different array order, sub-pixel jitter that rounds the same.
    TArray<FDriveElement> Current;
    Current.Add(MakeElement(TEXT("b"), TEXT("Text"), 0.1, -0.2, 50.0, 12.4));
    Current.Add(MakeElement(TEXT("a"), TEXT("Button"), 10.3, 20.1, 99.8, 40.2));

    const FDriveDiff Diff = FDriveChangeDetector::Diff(Previous, Current);

    TestTrue(TEXT("reorder + sub-pixel jitter is not a change"), Diff.IsEmpty());

    return true;
}

// ============================================================================
// Diff: a type change and a visibility change both count as "changed"
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveDiffTypeAndVisibilityTest,
    "PinWright.drive.fingerprint.DiffDetectsTypeAndVisibilityChange",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveDiffTypeAndVisibilityTest::RunTest(const FString& Parameters)
{
    TArray<FDriveElement> Previous;
    Previous.Add(MakeElement(TEXT("t"), TEXT("Button"), 0, 0, 10, 10));         // type will change
    Previous.Add(MakeElement(TEXT("v"), TEXT("Text"), 0, 0, 10, 10, true));     // visibility will change

    TArray<FDriveElement> Current;
    Current.Add(MakeElement(TEXT("t"), TEXT("ToggleButton"), 0, 0, 10, 10));    // same rect, new type
    Current.Add(MakeElement(TEXT("v"), TEXT("Text"), 0, 0, 10, 10, false));     // same rect, now hidden

    const FDriveDiff Diff = FDriveChangeDetector::Diff(Previous, Current);

    TestEqual(TEXT("nothing appeared"), Diff.Appeared.Num(), 0);
    TestEqual(TEXT("nothing disappeared"), Diff.Disappeared.Num(), 0);
    TestEqual(TEXT("both changed"), Diff.Changed.Num(), 2);
    // Sorted lexicographically: t before v.
    if (Diff.Changed.Num() == 2)
    {
        TestEqual(TEXT("changed[0] is t"), Diff.Changed[0], TEXT("t"));
        TestEqual(TEXT("changed[1] is v"), Diff.Changed[1], TEXT("v"));
    }

    return true;
}

// ============================================================================
// Diff: output is deterministic and sorted regardless of input order
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveDiffDeterministicTest,
    "PinWright.drive.fingerprint.DiffDeterministicSortedOutput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveDiffDeterministicTest::RunTest(const FString& Parameters)
{
    const TArray<FDriveElement> Previous; // empty -> everything appears

    TArray<FDriveElement> Current;
    Current.Add(MakeElement(TEXT("z"), TEXT("Button"), 0, 0, 1, 1));
    Current.Add(MakeElement(TEXT("m"), TEXT("Text"), 0, 0, 1, 1));
    Current.Add(MakeElement(TEXT("a"), TEXT("Image"), 0, 0, 1, 1));

    const FDriveDiff First = FDriveChangeDetector::Diff(Previous, Current);
    const FDriveDiff Second = FDriveChangeDetector::Diff(Previous, Current);

    TestEqual(TEXT("three appeared"), First.Appeared.Num(), 3);
    if (First.Appeared.Num() == 3)
    {
        TestEqual(TEXT("sorted appeared[0]"), First.Appeared[0], TEXT("a"));
        TestEqual(TEXT("sorted appeared[1]"), First.Appeared[1], TEXT("m"));
        TestEqual(TEXT("sorted appeared[2]"), First.Appeared[2], TEXT("z"));
    }

    TestTrue(TEXT("diff is deterministic across runs"), First.Appeared == Second.Appeared);

    return true;
}

// ============================================================================
// A value-only edit (a drive.type into a fixed-geometry field: same Type, rect,
// and visibility, only the live editable Value differs) is reported by the
// one-shot Diff, but stays deliberately INVISIBLE to the per-tick settle
// fingerprint (Compute) so a live-updating field cannot churn the settle loop
// into a timeout. Pins the split the fix relies on: Diff is value-aware, the
// fingerprint is not. Reverting the SignatureEquals value check fails the Diff
// assertion; folding Value into Compute fails the fingerprint assertion.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveDiffValueOnlyEditTest,
    "PinWright.drive.fingerprint.ValueOnlyEditDiffedNotFingerprinted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveDiffValueOnlyEditTest::RunTest(const FString& Parameters)
{
    // Before: an empty editable field (hint only, no typed value). After: the same
    // field - identical Type, geometry, and visibility - now carrying typed text.
    FDriveElement Before = MakeElement(TEXT("NameField"), TEXT("SEditableTextBox"), 100, 50, 200, 24);
    Before.Value = FString();

    FDriveElement After = MakeElement(TEXT("NameField"), TEXT("SEditableTextBox"), 100, 50, 200, 24);
    After.Value = TEXT("Aria Vance");

    TArray<FDriveElement> Prev; Prev.Add(Before);
    TArray<FDriveElement> Cur;  Cur.Add(After);

    // The one-shot response diff MUST flag the value-only edit as a changed handle.
    const FDriveDiff Diff = FDriveChangeDetector::Diff(Prev, Cur);
    TestEqual(TEXT("a value-only edit is one changed handle"), Diff.Changed.Num(), 1);
    TestEqual(TEXT("the changed handle is NameField"),
        Diff.Changed.Num() == 1 ? Diff.Changed[0] : FString(), TEXT("NameField"));
    TestEqual(TEXT("nothing appeared"), Diff.Appeared.Num(), 0);
    TestEqual(TEXT("nothing disappeared"), Diff.Disappeared.Num(), 0);
    TestFalse(TEXT("the diff is not empty for a value edit"), Diff.IsEmpty());

    // The per-tick settle fingerprint MUST stay identical across a value-only edit:
    // value is deliberately not hashed, so a live field cannot destabilize a settle.
    const FDriveFingerprint FpBefore = FDriveChangeDetector::Compute(Prev);
    const FDriveFingerprint FpAfter  = FDriveChangeDetector::Compute(Cur);
    TestTrue(TEXT("settle fingerprint ignores a value-only edit (stays churn-tolerant)"),
        FpBefore == FpAfter);

    // A case-only value edit (fixing capitalization in an editable field) is a REAL change.
    // FString::operator== is case-insensitive, so the compare must use ESearchCase::CaseSensitive
    // or "aria vance" -> "Aria Vance" is silently dropped from the diff.
    FDriveElement LowerCase = MakeElement(TEXT("NameField"), TEXT("SEditableTextBox"), 100, 50, 200, 24);
    LowerCase.Value = TEXT("aria vance");
    TArray<FDriveElement> Lower; Lower.Add(LowerCase);
    TestEqual(TEXT("a case-only value edit is one changed handle"),
        FDriveChangeDetector::Diff(Lower, Cur).Changed.Num(), 1);

    // No false positive: equal values (and equal everything else) yield an empty diff.
    TArray<FDriveElement> Same; Same.Add(Before);
    TestTrue(TEXT("equal values yield an empty diff"),
        FDriveChangeDetector::Diff(Prev, Same).IsEmpty());

    return true;
}
