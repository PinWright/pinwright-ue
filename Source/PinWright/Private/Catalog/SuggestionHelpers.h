// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// Fuzzy near-miss ranking shared by the call sites that turn a miss into a
// self-correcting hint: WikiHandler's not-found page, RpcDispatcher's
// UNKNOWN_ACTION error, and PwModelOpTable::SuggestClosest in PinWrightGeometry
// (PWSRC_UNKNOWN_OP / PWSRC_UNKNOWN_PARAM). Extracted from WikiHandler's
// anonymous namespace into a named namespace + inline function so the plugin's
// Unity build can merge the translation units without an anonymous-namespace ODR /
// redefinition collision.
//
// Conventions match Tests/Infra/DispatcherTestHelpers.h: named namespace +
// inline function, no module API macro.

#include "CoreMinimal.h"
#include "Algo/LevenshteinDistance.h"
#include "Algo/Sort.h"

namespace SuggestionHelpers
{
    // Single-pool fuzzy ranking: tier 1 = case-insensitive substring-contains hits ranked
    // by candidate length (shorter matches rank higher); tier 2 = Levenshtein similarity
    // over whole names with a cutoff STRICTLY above 0.5. Returns up to MaxResults
    // candidates.
    inline TArray<FString> RankSuggestions(const FString& QueryLower,
                                           const TArray<FString>& Pool,
                                           int32 MaxResults)
    {
        struct FScored { FString Name; double Score; bool bSubstring; };
        TArray<FScored> Scored;
        Scored.Reserve(Pool.Num());

        for (const FString& Name : Pool)
        {
            const FString NameLower = Name.ToLower();
            if (NameLower.Contains(QueryLower))
            {
                // Tier 1: shorter candidates win (closer-to-exact match).
                Scored.Add({ Name, -static_cast<double>(NameLower.Len()), true });
                continue;
            }
            const int32 Dist = Algo::LevenshteinDistance(QueryLower, NameLower);
            const int32 Sum = QueryLower.Len() + NameLower.Len();
            const double Sim = (Sum == 0) ? 0.0 : 1.0 - (static_cast<double>(Dist) / static_cast<double>(Sum));
            // STRICTLY above 0.5 - the same rule, and the same reason, as the tier-2
            // cutoff in RankSuggestionsWithSummaries below; the long derivation lives
            // there. Sim normalises the edit distance by the SUMMED length, so two
            // strings sharing no character at all score exactly 0.5 whenever their
            // lengths are equal - Dist is then max(LenA, LenB), the cost of retyping the
            // whole name. `>=` therefore admitted a candidate with literally nothing in
            // common: a three-character unknown pwmodel op was answered with a
            // three-character op name sharing not one letter with it.
            //
            // INVARIANT, and it holds at every length rather than at a tuned constant: a
            // query sharing NOTHING with a candidate is never suggested for it. `>` is
            // the same statement as 2*Dist < LenA + LenB - the edit script has to beat a
            // full retype - and a fully disjoint pair can never clear it, since that
            // needs max(LenA, LenB) < min(LenA, LenB). Anything sharing one aligned
            // character does clear it, so nothing real is lost.
            if (Sim > 0.5)
            {
                Scored.Add({ Name, Sim, false });
            }
        }

        Algo::Sort(Scored, [](const FScored& A, const FScored& B)
        {
            if (A.bSubstring != B.bSubstring) return A.bSubstring;     // substring tier first
            return A.Score > B.Score;                                  // higher score wins within tier
        });

        TArray<FString> Out;
        Out.Reserve(FMath::Min(MaxResults, Scored.Num()));
        for (int32 i = 0; i < Scored.Num() && Out.Num() < MaxResults; ++i)
        {
            Out.Add(Scored[i].Name);
        }
        return Out;
    }

    // ------------------------------------------------------------------------
    // Summary-aware ranking, used by RpcDispatcher's UNKNOWN_ACTION path.
    //
    // Why a second entry point rather than a change to RankSuggestions: that one
    // scores whole dotted strings, so on a 1191-method registry the shared
    // namespace prefix dominates the Levenshtein term and the verb token - the
    // only part carrying meaning - is drowned out. Measured against the live
    // registry, `asset.find_by_name` answered "Did you mean: asset.find_by_tag,
    // actor.find_by_name, actor.find_by_tag, asset.bulk_rename,
    // actor.find_by_class": five bare names, none of them `asset.search` (the verb
    // that actually answers a find-by-name question), and nothing in the list
    // letting the caller tell a UMetaData tag search from a name search before
    // spending a call on one.
    //
    // Three generic changes: candidates are also matched on the WORDS of the query
    // against the registered Summary, so the semantically right verb enters the
    // list at all; same-namespace candidates outrank cross-namespace ones, which
    // is what frees the slots the actor.* near-misses were occupying; and each
    // entry carries its clipped summary, so picking one is a decision rather than
    // a coin flip. WikiHandler's not-found page keeps the name-only ranker above.
    // ------------------------------------------------------------------------

    struct FSuggestionCandidate
    {
        FString Method;   // "asset.search"
        FString Summary;  // FHandlerRegistration::Summary, may be empty
    };

    // Everything before the LAST dot: "asset" for asset.search, "system.inspect"
    // for system.inspect.find_by_tag. Empty when the input carries no dot.
    inline FString SuggestionNamespaceOf(const FString& DottedLower)
    {
        int32 Idx = INDEX_NONE;
        return DottedLower.FindLastChar(TEXT('.'), Idx) ? DottedLower.Left(Idx) : FString();
    }

    // Words of a dotted method name that carry intent. Splitting on . _ - and
    // dropping sub-3-character fragments removes the glue ("by", "of") without a
    // hand-maintained stopword list; the few generic words that survive are the
    // ones the >= 2 match floor below is there to absorb.
    inline TArray<FString> SuggestionQueryTokens(const FString& QueryLower)
    {
        static const TCHAR* Delims[] = { TEXT("."), TEXT("_"), TEXT("-") };
        TArray<FString> Raw;
        QueryLower.ParseIntoArray(Raw, Delims, UE_ARRAY_COUNT(Delims), /*InCullEmpty=*/true);

        TArray<FString> Tokens;
        for (const FString& Token : Raw)
        {
            if (Token.Len() >= 3)
            {
                Tokens.AddUnique(Token);
            }
        }
        return Tokens;
    }

    // One short phrase, so five suggestions stay a line each rather than a wall.
    // Cuts at the first sentence end when that lands inside the budget, otherwise
    // hard-clips; newlines are flattened because the message is a single string.
    inline FString ClipSuggestionSummary(const FString& Summary, int32 MaxChars = 96)
    {
        FString Flat = Summary.Replace(TEXT("\r"), TEXT(" ")).Replace(TEXT("\n"), TEXT(" "));
        Flat.TrimStartAndEndInline();

        int32 SentenceEnd = INDEX_NONE;
        if (Flat.FindChar(TEXT('.'), SentenceEnd) && SentenceEnd > 0 && SentenceEnd < MaxChars)
        {
            return Flat.Left(SentenceEnd + 1);
        }
        if (Flat.Len() <= MaxChars)
        {
            return Flat;
        }
        return Flat.Left(MaxChars).TrimEnd() + TEXT("...");
    }

    // Returns up to MaxResults entries formatted "method (clipped summary)", or the
    // bare method when the registration carries no summary.
    inline TArray<FString> RankSuggestionsWithSummaries(const FString& QueryLower,
                                                        const TArray<FSuggestionCandidate>& Pool,
                                                        int32 MaxResults)
    {
        const FString QueryNamespace = SuggestionNamespaceOf(QueryLower);
        const TArray<FString> QueryTokens = SuggestionQueryTokens(QueryLower);

        // Tier 0 = the candidate name contains the whole query (unchanged from the
        // name-only ranker); tier 1 = keyword overlap with name+summary; tier 2 =
        // whole-name Levenshtein. Lower tier wins, and a same-namespace candidate
        // wins over any cross-namespace one.
        struct FScored { FString Entry; int32 Tier; bool bSameNamespace; double Score; };
        TArray<FScored> Scored;
        Scored.Reserve(Pool.Num());

        for (const FSuggestionCandidate& Candidate : Pool)
        {
            const FString NameLower = Candidate.Method.ToLower();
            const bool bSameNamespace =
                !QueryNamespace.IsEmpty() && SuggestionNamespaceOf(NameLower) == QueryNamespace;

            const int32 Dist = Algo::LevenshteinDistance(QueryLower, NameLower);
            const int32 Sum = QueryLower.Len() + NameLower.Len();
            const double Sim = (Sum == 0) ? 0.0 : 1.0 - (static_cast<double>(Dist) / static_cast<double>(Sum));

            int32 Tier = INDEX_NONE;
            double Score = 0.0;

            if (NameLower.Contains(QueryLower))
            {
                Tier = 0;
                Score = -static_cast<double>(NameLower.Len());  // shorter == closer to exact
            }
            else
            {
                // Keyword tier searches the summary as well as the name, which is
                // the whole point: `asset.search`'s summary is where the word
                // "name" lives, and matching only the dotted name can never see it.
                const FString Haystack = NameLower + TEXT(" ") + Candidate.Summary.ToLower();
                int32 Matched = 0;
                for (const FString& Token : QueryTokens)
                {
                    if (Haystack.Contains(Token))
                    {
                        ++Matched;
                    }
                }
                // Two is the floor because one shared word ("asset", "get") is
                // satisfied by most of the namespace and would rank noise.
                if (Matched >= 2)
                {
                    Tier = 1;
                    Score = static_cast<double>(Matched) + Sim;  // Sim only breaks ties
                }
                // STRICTLY above 0.5, and the strictness is the rule rather than a
                // tuned constant. Sim normalises the edit distance by the SUMMED
                // length, so two strings sharing no character at all score exactly
                // 0.5 whenever their lengths are equal - Dist is then max(LenA, LenB),
                // the cost of retyping the whole name. `>=` therefore admitted a
                // candidate with literally nothing in common. `>` is the same
                // statement as 2*Dist < LenA + LenB: the edit script has to beat a
                // full retype, which is the weakest honest claim a did-you-mean can
                // make, and anything sharing one aligned character clears it.
                //
                // Measured on the live registry: a 64-character all-`z` method name
                // scored exactly 0.5 against
                // `material.authoring.set_material_instance_base_property_overrides`
                // - 64 characters, not one of them a `z` - and was offered as the
                // did-you-mean. UnknownActionNoCloseMatch caught it only because that
                // verb was the first registered name long enough to reach the
                // boundary; every shorter garbage string had been landing on the same
                // rule unnoticed.
                else if (Sim > 0.5)
                {
                    Tier = 2;
                    Score = Sim;
                }
            }

            if (Tier == INDEX_NONE)
            {
                continue;
            }

            const FString Clipped = ClipSuggestionSummary(Candidate.Summary);
            const FString Entry = Clipped.IsEmpty()
                ? Candidate.Method
                : FString::Printf(TEXT("%s (%s)"), *Candidate.Method, *Clipped);
            Scored.Add({ Entry, Tier, bSameNamespace, Score });
        }

        Algo::Sort(Scored, [](const FScored& A, const FScored& B)
        {
            if (A.bSameNamespace != B.bSameNamespace) return A.bSameNamespace;
            if (A.Tier != B.Tier) return A.Tier < B.Tier;
            return A.Score > B.Score;
        });

        TArray<FString> Out;
        Out.Reserve(FMath::Min(MaxResults, Scored.Num()));
        for (int32 i = 0; i < Scored.Num() && Out.Num() < MaxResults; ++i)
        {
            Out.Add(Scored[i].Entry);
        }
        return Out;
    }
}
