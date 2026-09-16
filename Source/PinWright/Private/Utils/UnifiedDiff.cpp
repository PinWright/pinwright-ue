// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/UnifiedDiff.h"
#include "Compat/EngineVersionCompat.h"
#include "Algo/Reverse.h"

namespace UnifiedDiff
{

namespace
{

enum class EEdit : uint8 { Keep, Delete, Insert };

struct FEditOp
{
    EEdit Op;
    FString Line;
    int32 OldLine; // 1-based; 0 for Insert
    int32 NewLine; // 1-based; 0 for Delete
};

// LCS dynamic programming. Fills a flat (M+1)*(N+1) table, then backtracks.
TArray<FEditOp> ComputeEditScript(const TArray<FString>& A, const TArray<FString>& B)
{
    const int32 M = A.Num();
    const int32 N = B.Num();

    TArray<int32> Dp;
    Dp.SetNumZeroed((M + 1) * (N + 1));

    for (int32 i = 1; i <= M; ++i)
        for (int32 j = 1; j <= N; ++j)
        {
            if (A[i - 1] == B[j - 1])
                Dp[i * (N + 1) + j] = Dp[(i - 1) * (N + 1) + (j - 1)] + 1;
            else
                Dp[i * (N + 1) + j] = FMath::Max(Dp[(i - 1) * (N + 1) + j],
                                                   Dp[i * (N + 1) + (j - 1)]);
        }

    TArray<FEditOp> Script;
    int32 i = M, j = N;
    while (i > 0 || j > 0)
    {
        if (i > 0 && j > 0 && A[i - 1] == B[j - 1])
        {
            Script.Add({EEdit::Keep, A[i - 1], i, j});
            --i; --j;
        }
        else if (j > 0 && (i == 0 || Dp[i * (N + 1) + (j - 1)] >= Dp[(i - 1) * (N + 1) + j]))
        {
            Script.Add({EEdit::Insert, B[j - 1], 0, j});
            --j;
        }
        else
        {
            Script.Add({EEdit::Delete, A[i - 1], i, 0});
            --i;
        }
    }

    Algo::Reverse(Script);
    return Script;
}

} // anonymous namespace

FString MakeUnifiedDiff(
    const FString& Old,
    const FString& New,
    const FString& OldLabel,
    const FString& NewLabel,
    int32 ContextLines)
{
    if (Old == New)
        return FString();

    // Strip a single trailing '\n' from each side before splitting so that a
    // terminated "a\n" yields ["a"] rather than ["a", ""]. Track whether the
    // trailing newline was present — if the two sides disagree, the last line
    // on the terminated side differs semantically from the last line on the
    // non-terminated side (git shows this as a "\ No newline at end of file"
    // marker with both -old and +new entries).
    FString OldBody = Old;
    FString NewBody = New;
    const bool bOldHadTrailingNewline = OldBody.EndsWith(TEXT("\n"));
    const bool bNewHadTrailingNewline = NewBody.EndsWith(TEXT("\n"));
    if (bOldHadTrailingNewline) OldBody.LeftChopInline(1, EAllowShrinking::No);
    if (bNewHadTrailingNewline) NewBody.LeftChopInline(1, EAllowShrinking::No);

    TArray<FString> OldLines, NewLines;
    if (!OldBody.IsEmpty())
        OldBody.ParseIntoArray(OldLines, TEXT("\n"), /*bCullEmpty=*/false);
    if (!NewBody.IsEmpty())
        NewBody.ParseIntoArray(NewLines, TEXT("\n"), /*bCullEmpty=*/false);

    // If the two sides' trailing-newline state differs, the last line on the
    // terminated side is semantically a different line from the last line on
    // the non-terminated side. Distinguish them to the LCS algorithm by
    // appending a sentinel codepoint (U+E000, a private-use code point that
    // will never appear in normal source text) to the terminated side's last
    // line. We strip the sentinel back out when emitting each hunk line, so
    // users never see it.
    const TCHAR SentinelChars[2] = { (TCHAR)0xE000, (TCHAR)0 };
    const FString SentinelStr(SentinelChars);
    if (bOldHadTrailingNewline != bNewHadTrailingNewline)
    {
        if (bOldHadTrailingNewline && OldLines.Num() > 0)
            OldLines.Last() += SentinelStr;
        else if (bNewHadTrailingNewline && NewLines.Num() > 0)
            NewLines.Last() += SentinelStr;
        else
        {
            // One side is empty, other is just "\n" (or the reverse). Inject a
            // distinguishing empty-ish line so LCS produces an Insert or Delete.
            if (bOldHadTrailingNewline) OldLines.Add(SentinelStr);
            else                         NewLines.Add(SentinelStr);
        }
    }

    TArray<FEditOp> Script = ComputeEditScript(OldLines, NewLines);
    const int32 Total = Script.Num();

    // Identify indices of all changed ops.
    TArray<int32> ChangeIndices;
    for (int32 k = 0; k < Total; ++k)
        if (Script[k].Op != EEdit::Keep)
            ChangeIndices.Add(k);

    if (ChangeIndices.IsEmpty())
        return FString();

    // Group change indices into hunk ranges [Begin, Finish] in script coordinates.
    // Two changes merge into the same hunk if their context windows overlap, i.e.
    // the gap (number of Keep lines between them) <= 2 * ContextLines.
    struct FHunkRange { int32 Begin; int32 Finish; };
    TArray<FHunkRange> Ranges;

    {
        int32 CurBegin  = FMath::Max(0, ChangeIndices[0] - ContextLines);
        int32 CurFinish = FMath::Min(Total - 1, ChangeIndices[0] + ContextLines);
        int32 LastChange = ChangeIndices[0];

        for (int32 ci = 1; ci < ChangeIndices.Num(); ++ci)
        {
            int32 Next = ChangeIndices[ci];
            int32 Gap = Next - LastChange - 1; // Keep lines between LastChange and Next
            if (Gap <= 2 * ContextLines)
            {
                // Merge: extend current hunk.
                CurFinish = FMath::Min(Total - 1, Next + ContextLines);
            }
            else
            {
                Ranges.Add({CurBegin, CurFinish});
                CurBegin  = FMath::Max(0, Next - ContextLines);
                CurFinish = FMath::Min(Total - 1, Next + ContextLines);
            }
            LastChange = Next;
        }
        Ranges.Add({CurBegin, CurFinish});
    }

    // Build output.
    FString Out;
    Out += TEXT("--- ") + OldLabel + TEXT("\n");
    Out += TEXT("+++ ") + NewLabel + TEXT("\n");

    for (const FHunkRange& R : Ranges)
    {
        // Determine OldStart and NewStart: the 1-based line numbers of the first
        // op in this hunk in each file.
        int32 OldStart = 0, NewStart = 0;
        for (int32 k = R.Begin; k <= R.Finish; ++k)
        {
            const FEditOp& Op = Script[k];
            if (Op.Op == EEdit::Keep || Op.Op == EEdit::Delete)
            {
                if (OldStart == 0) OldStart = Op.OldLine;
            }
            if (Op.Op == EEdit::Keep || Op.Op == EEdit::Insert)
            {
                if (NewStart == 0) NewStart = Op.NewLine;
            }
            if (OldStart != 0 && NewStart != 0) break;
        }

        // For a purely added hunk (OldStart still 0): set OldStart to the old-file
        // line BEFORE the insertion. Count old-file lines contributed by ops before
        // this hunk.
        if (OldStart == 0)
        {
            int32 OldOffset = 0;
            for (int32 k = 0; k < R.Begin; ++k)
                if (Script[k].Op != EEdit::Insert) ++OldOffset;
            OldStart = OldOffset; // 0 means "before line 1" -> @@ -0,0 ... @@
        }
        if (NewStart == 0)
        {
            int32 NewOffset = 0;
            for (int32 k = 0; k < R.Begin; ++k)
                if (Script[k].Op != EEdit::Delete) ++NewOffset;
            NewStart = NewOffset;
        }

        int32 OldLen = 0, NewLen = 0;
        TArray<FString> HunkLines;
        for (int32 k = R.Begin; k <= R.Finish; ++k)
        {
            const FEditOp& Op = Script[k];
            // Strip the trailing-newline sentinel (see above) before emitting.
            FString DisplayLine = Op.Line;
            DisplayLine.RemoveFromEnd(SentinelStr);
            if (Op.Op == EEdit::Keep)
            {
                HunkLines.Add(TEXT(" ") + DisplayLine);
                ++OldLen; ++NewLen;
            }
            else if (Op.Op == EEdit::Delete)
            {
                HunkLines.Add(TEXT("-") + DisplayLine);
                ++OldLen;
            }
            else
            {
                HunkLines.Add(TEXT("+") + DisplayLine);
                ++NewLen;
            }
        }

        Out += FString::Printf(TEXT("@@ -%d,%d +%d,%d @@\n"),
            OldStart, OldLen, NewStart, NewLen);
        for (const FString& L : HunkLines)
            Out += L + TEXT("\n");
    }

    return Out;
}

} // namespace UnifiedDiff
