// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

#include "Catalog/SuggestionHelpers.h"

namespace PwSuggest
{
    // Candidates is a view so a caller holding either a TArray or a TArrayView can pass
    // it directly: ParseUse takes TArrayView<const FString> for its kind list, and a
    // TArray converts implicitly, so both spellings compose without a cast at the site.
    inline FString Closest(const FString& Requested, TArrayView<const FString> Candidates)
    {
        const FString Normalized = Requested.ToLower();
        if (Normalized.IsEmpty())
        {
            return FString();
        }

        // Tier one handles suffix and inflection-like typos such as "bevelation".
        // The longest contained candidate wins, so a more specific operation is not
        // hidden by a short name that happens to be a substring.
        FString Contained;
        int32 ContainedLen = -1;
        for (const FString& Candidate : Candidates)
        {
            const FString Lowered = Candidate.ToLower();
            if (!Lowered.IsEmpty() && Normalized.Contains(Lowered) && Lowered.Len() > ContainedLen)
            {
                ContainedLen = Lowered.Len();
                Contained = Candidate;
            }
        }
        if (!Contained.IsEmpty())
        {
            return Contained;
        }

        // Tier two is Levenshtein-based and catches transpositions such as
        // "spehre", which containment cannot see.
        // RankSuggestions still takes a TArray; materialise one only here, on the error
        // path where a suggestion is already being produced for a failed lookup.
        const TArray<FString> Pool(Candidates);
        const TArray<FString> Ranked = SuggestionHelpers::RankSuggestions(Normalized, Pool, 1);
        return Ranked.Num() > 0 ? Ranked[0] : FString();
    }
}
