// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Utils/UnifiedDiff.h"


// ============================================================================
// UnifiedDiff.Equal
// Identical inputs produce empty output.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnifiedDiffEqualTest,
    "PinWright.utils.unified_diff.Equal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnifiedDiffEqualTest::RunTest(const FString& Parameters)
{
    FString Result = UnifiedDiff::MakeUnifiedDiff(TEXT("a\nb\nc\n"), TEXT("a\nb\nc\n"));
    TestTrue(TEXT("Equal inputs produce empty string"), Result.IsEmpty());
    return true;
}

// ============================================================================
// UnifiedDiff.SingleLineAdd
// One line inserted between two unchanged lines.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnifiedDiffSingleLineAddTest,
    "PinWright.utils.unified_diff.SingleLineAdd",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnifiedDiffSingleLineAddTest::RunTest(const FString& Parameters)
{
    FString Result = UnifiedDiff::MakeUnifiedDiff(
        TEXT("a\nb\nc\n"),
        TEXT("a\nb\nX\nc\n"));

    TestFalse(TEXT("Output is non-empty"), Result.IsEmpty());
    TestTrue(TEXT("Contains +X"), Result.Contains(TEXT("+X")));
    TestTrue(TEXT("Context line a present"), Result.Contains(TEXT(" a")));
    TestTrue(TEXT("Context line b present"), Result.Contains(TEXT(" b")));
    TestTrue(TEXT("Context line c present"), Result.Contains(TEXT(" c")));
    // No - content lines (the --- header is fine).
    {
        TArray<FString> AddLines;
        Result.ParseIntoArrayLines(AddLines);
        bool bHasDelete = false;
        for (const FString& Line : AddLines)
        {
            if (Line.StartsWith(TEXT("-")) && !Line.StartsWith(TEXT("---")))
            {
                bHasDelete = true;
                break;
            }
        }
        TestFalse(TEXT("No deletion content lines"), bHasDelete);
    }

    // Exactly one @@ hunk.
    int32 HunkCount = 0;
    int32 SearchPos = 0;
    while (true)
    {
        int32 Found = Result.Find(TEXT("@@"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchPos);
        if (Found == INDEX_NONE) break;
        ++HunkCount;
        SearchPos = Found + 2;
    }
    TestEqual(TEXT("Exactly one @@ marker pair (open + close = 2 @@ tokens in header)"),
        HunkCount, 2); // one header has two @@ tokens

    return true;
}

// ============================================================================
// UnifiedDiff.SingleLineRemove
// One line removed; mirror of SingleLineAdd.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnifiedDiffSingleLineRemoveTest,
    "PinWright.utils.unified_diff.SingleLineRemove",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnifiedDiffSingleLineRemoveTest::RunTest(const FString& Parameters)
{
    FString Result = UnifiedDiff::MakeUnifiedDiff(
        TEXT("a\nb\nX\nc\n"),
        TEXT("a\nb\nc\n"));

    TestFalse(TEXT("Output is non-empty"), Result.IsEmpty());
    TestTrue(TEXT("Contains -X"), Result.Contains(TEXT("-X")));
    TestTrue(TEXT("Context line a present"), Result.Contains(TEXT(" a")));
    TestTrue(TEXT("Context line c present"), Result.Contains(TEXT(" c")));
    // No + content lines (the +++ header is fine; only check non-header lines).
    TArray<FString> RemoveLines;
    Result.ParseIntoArrayLines(RemoveLines);
    bool bHasInsert = false;
    for (const FString& Line : RemoveLines)
    {
        if (Line.StartsWith(TEXT("+")) && !Line.StartsWith(TEXT("+++")))
        {
            bHasInsert = true;
            break;
        }
    }
    TestFalse(TEXT("No insertion content lines"), bHasInsert);

    return true;
}

// ============================================================================
// UnifiedDiff.ChangedLine
// One line replaced: one - and one +.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnifiedDiffChangedLineTest,
    "PinWright.utils.unified_diff.ChangedLine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnifiedDiffChangedLineTest::RunTest(const FString& Parameters)
{
    FString Result = UnifiedDiff::MakeUnifiedDiff(
        TEXT("a\nb\nc\n"),
        TEXT("a\nX\nc\n"));

    TestFalse(TEXT("Output is non-empty"), Result.IsEmpty());
    TestTrue(TEXT("Contains -b"), Result.Contains(TEXT("-b")));
    TestTrue(TEXT("Contains +X"), Result.Contains(TEXT("+X")));

    return true;
}

// ============================================================================
// UnifiedDiff.TrailingNewlineHandling
// "a\n" vs "a" must produce a non-empty diff (the trailing newline changed).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnifiedDiffTrailingNewlineHandlingTest,
    "PinWright.utils.unified_diff.TrailingNewlineHandling",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnifiedDiffTrailingNewlineHandlingTest::RunTest(const FString& Parameters)
{
    FString Result = UnifiedDiff::MakeUnifiedDiff(TEXT("a\n"), TEXT("a"));

    TestFalse(TEXT("Output is non-empty (trailing newline is a semantic change)"), Result.IsEmpty());

    // "a\n" vs "a" — the trailing newline differs. A correct diff must show
    // BOTH the old (`-a\n`) and new (`+a`) lines, not just one.
    TArray<FString> Lines;
    Result.ParseIntoArrayLines(Lines);

    int32 MinusLineCount = 0;
    int32 PlusLineCount  = 0;
    for (const FString& Line : Lines)
    {
        if (Line.StartsWith(TEXT("---"))) continue;
        if (Line.StartsWith(TEXT("+++"))) continue;
        if (Line.StartsWith(TEXT("-"))) ++MinusLineCount;
        else if (Line.StartsWith(TEXT("+"))) ++PlusLineCount;
    }

    TestTrue(TEXT("Diff contains a '-' line (old side)"), MinusLineCount >= 1);
    TestTrue(TEXT("Diff contains a '+' line (new side)"), PlusLineCount >= 1);

    // Determinism: running twice with same input yields same output.
    FString Result2 = UnifiedDiff::MakeUnifiedDiff(TEXT("a\n"), TEXT("a"));
    TestEqual(TEXT("Deterministic across runs"), Result, Result2);

    return true;
}

// ============================================================================
// UnifiedDiff.EmptyOld
// Old is empty, new has content — all + lines.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnifiedDiffEmptyOldTest,
    "PinWright.utils.unified_diff.EmptyOld",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnifiedDiffEmptyOldTest::RunTest(const FString& Parameters)
{
    FString Result = UnifiedDiff::MakeUnifiedDiff(TEXT(""), TEXT("a\nb\n"));

    TestFalse(TEXT("Output is non-empty"), Result.IsEmpty());

    // Must have at least one + line.
    TestTrue(TEXT("Contains +a"), Result.Contains(TEXT("+a")));
    TestTrue(TEXT("Contains +b"), Result.Contains(TEXT("+b")));

    // Must not have any - lines (nothing was in the old).
    TArray<FString> Lines;
    Result.ParseIntoArrayLines(Lines);
    bool bHasDeleteLine = false;
    for (const FString& Line : Lines)
    {
        if (Line.StartsWith(TEXT("-")) && !Line.StartsWith(TEXT("---")))
        {
            bHasDeleteLine = true;
            break;
        }
    }
    TestFalse(TEXT("No deletion lines for empty-old"), bHasDeleteLine);

    // OldStart in header must be 0 (nothing in old).
    TestTrue(TEXT("Header contains -0,"), Result.Contains(TEXT("-0,")));

    return true;
}

// ============================================================================
// UnifiedDiff.EmptyNew
// New is empty, old has content — all - lines.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnifiedDiffEmptyNewTest,
    "PinWright.utils.unified_diff.EmptyNew",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnifiedDiffEmptyNewTest::RunTest(const FString& Parameters)
{
    FString Result = UnifiedDiff::MakeUnifiedDiff(TEXT("a\nb\n"), TEXT(""));

    TestFalse(TEXT("Output is non-empty"), Result.IsEmpty());

    TestTrue(TEXT("Contains -a"), Result.Contains(TEXT("-a")));
    TestTrue(TEXT("Contains -b"), Result.Contains(TEXT("-b")));

    // Must not have any + lines.
    TArray<FString> Lines;
    Result.ParseIntoArrayLines(Lines);
    bool bHasInsertLine = false;
    for (const FString& Line : Lines)
    {
        if (Line.StartsWith(TEXT("+")) && !Line.StartsWith(TEXT("+++")))
        {
            bHasInsertLine = true;
            break;
        }
    }
    TestFalse(TEXT("No insertion lines for empty-new"), bHasInsertLine);

    // NewStart in header must be 0 (nothing in new).
    TestTrue(TEXT("Header contains +0,"), Result.Contains(TEXT("+0,")));

    return true;
}

// ============================================================================
// UnifiedDiff.MultipleHunks
// Two separated changes produce two @@ hunks, not one merged hunk.
// Input: 10 unchanged lines, 1 change, 10 unchanged lines, 1 change, 10 unchanged lines.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnifiedDiffMultipleHunksTest,
    "PinWright.utils.unified_diff.MultipleHunks",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnifiedDiffMultipleHunksTest::RunTest(const FString& Parameters)
{
    // Build 32-line inputs. Lines 11 and 22 differ between old and new.
    FString OldStr, NewStr;
    for (int32 i = 1; i <= 32; ++i)
    {
        FString OldLine = FString::Printf(TEXT("line%d"), i);
        FString NewLine = OldLine;
        if (i == 11) NewLine = TEXT("changed11");
        if (i == 22) NewLine = TEXT("changed22");
        OldStr += OldLine + TEXT("\n");
        NewStr += NewLine + TEXT("\n");
    }

    FString Result = UnifiedDiff::MakeUnifiedDiff(OldStr, NewStr);
    TestFalse(TEXT("Output is non-empty"), Result.IsEmpty());

    // Count @@ hunk headers (each header has exactly two @@ tokens on the same line).
    TArray<FString> Lines;
    Result.ParseIntoArrayLines(Lines);
    int32 HunkHeaders = 0;
    for (const FString& Line : Lines)
    {
        if (Line.StartsWith(TEXT("@@")))
            ++HunkHeaders;
    }
    TestEqual(TEXT("Two @@ hunk headers"), HunkHeaders, 2);

    // Both changed lines appear as insertions.
    TestTrue(TEXT("contains +changed11"), Result.Contains(TEXT("+changed11")));
    TestTrue(TEXT("contains +changed22"), Result.Contains(TEXT("+changed22")));

    return true;
}

// ============================================================================
// UnifiedDiff.LabelsAppearInHeader
// Custom OldLabel and NewLabel appear in the --- and +++ header lines.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnifiedDiffLabelsAppearInHeaderTest,
    "PinWright.utils.unified_diff.LabelsAppearInHeader",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnifiedDiffLabelsAppearInHeaderTest::RunTest(const FString& Parameters)
{
    FString Result = UnifiedDiff::MakeUnifiedDiff(
        TEXT("hello\n"),
        TEXT("world\n"),
        TEXT("my-old-label"),
        TEXT("my-new-label"));

    TestTrue(TEXT("--- contains old label"), Result.Contains(TEXT("--- my-old-label")));
    TestTrue(TEXT("+++ contains new label"), Result.Contains(TEXT("+++ my-new-label")));

    return true;
}
