// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Handlers/Drive/DriveTypes.h"

// Pure-logic evaluator for a single FDriveCondition against an observed element
// set plus a journal delta. No Slate/CEF/UObject dependency and no state; it
// only reads the value structs in DriveTypes.h and returns an
// FDriveConditionResult with human-readable Actual/Expected/Detail strings.
//
// Element matching rule (shared by every widget/text/count/geometry condition):
//   an element matches Condition.Target when its Handle is non-empty and exactly
//   equals Target (case-sensitive), OR its Label is non-empty and equals Target
//   case-insensitively, OR Target is a whole '/'-delimited run of Handle's segments
//   (case-sensitive), which is how a UMG widget name ("OkButton") or a Slate type
//   segment names an element. An empty Target matches nothing. Single-element
//   conditions prefer an exact handle/label match over a sub-path match.
//
// Journal severity ordering (case-insensitive):
//   trace < debug < info < warning < error < fatal.
class FDriveConditionEval
{
public:
    // Evaluates Condition against the observed Elements and Journal delta.
    // Always returns a populated result; Actual/Expected/Detail are filled for
    // every condition type.
    static FDriveConditionResult Evaluate(
        const FDriveCondition& Condition,
        const TArray<FDriveElement>& Elements,
        const FDriveJournalDelta& Journal);

    // Rank of a severity token in the standard ordering (trace=0 ... fatal=5),
    // case-insensitive. Returns INDEX_NONE for an unrecognized token.
    static int32 SeverityRank(const FString& Severity);

    // True when Element matches Target under the rule documented above.
    static bool ElementMatchesTarget(const FDriveElement& Element, const FString& Target);

private:
    // First element matching Target, or nullptr.
    static const FDriveElement* FindMatch(const TArray<FDriveElement>& Elements, const FString& Target);

    // Number of elements matching Target.
    static int32 CountMatches(const TArray<FDriveElement>& Elements, const FString& Target);
};
