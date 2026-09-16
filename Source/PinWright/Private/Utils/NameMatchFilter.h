// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared name/label matching policy for the list-and-filter read verbs. Consumers today:
// `actor.list`, `system.inspect.list_objects`, `system.inspect.find_objects_by_class`
// (pattern param `filter`), and `spatial.raycast` (pattern param `actorFilter`).
// The wire params below are meant to be adopted verbatim by any sibling that grows a name
// filter, so the vocabulary never drifts per verb the way `filter` / `nameMatch` /
// `matchType` / `classFilterMode` already have. Only the PATTERN key is per-verb; the two
// modifier keys and every mode spelling are fixed here.
//
// Why this exists: actor.list's `filter` was a bare `Label.Contains(Filter)`,
// and FString::Contains defaults to
// ESearchCase::IgnoreCase. It was therefore a case-INSENSITIVE SUBSTRING match while
// the wiki documented it "case-sensitive", so `filter:"SH_"` matched `Brush_0` - the
// lowercase `sh_` inside "Bru**sh_**0" - and reported totalMatches:98 where the true
// count of SH_-prefixed labels was 4. Nothing in the response looked wrong, so an agent
// counting actors by naming-convention prefix reported fabricated numbers.
//
// The fix keeps that exact behaviour as the DEFAULT so every existing call returns
// byte-identical rows, and adds two opt-in knobs:
//
//   matchMode      "contains" (default) | "prefix" | "exact" | "regex"
//   caseSensitive  false (default) | true
//
// The two param names and the mode spellings are deliberately identical to
// blueprint.graph.find_nodes (Handlers/Blueprint/BlueprintGraphInspectionHandler.cpp),
// which already shipped `caseSensitive` + `matchMode` with contains/prefix/exact - and
// to asset.search's `classFilterMode` (exact/prefix/contains) and actor.find_by_tag's
// `matchType` (exact/contains). `substring` is accepted as an alias for `contains` and
// `starts_with` for `prefix` so both established spellings work; `regex` is new here.
//
// A malformed regex is rejected with a typed INVALID_PATTERN rather than silently
// matching nothing - see the CompilePattern comment in the .cpp for why detecting that
// needs an explicit probe.
#pragma once

#include "CoreMinimal.h"
#include "Handlers/ParamSpec.h"
#include "Internationalization/Regex.h"

class FHandlerContext;
class FJsonObject;

namespace NameMatch
{
    // Wire values of the `matchMode` param. Contains is first because it is the
    // legacy (and default) behaviour; nothing here may be reordered without changing
    // what an absent `matchMode` means.
    enum class EMode : uint8
    {
        // Pattern occurs anywhere in the candidate. The pre-existing behaviour.
        // Wire: "contains" (alias "substring").
        Contains,
        // Candidate begins with the pattern. What convention-prefix counting wants.
        // Wire: "prefix" (alias "starts_with").
        Prefix,
        // Candidate equals the pattern. Wire: "exact".
        Exact,
        // Unanchored ICU regex search (FRegexMatcher::FindNext), so `^SH_` anchors
        // and `SH_` alone still behaves like Contains. Wire: "regex".
        Regex
    };

    // The parsed filter. Default-constructed it is inactive (empty pattern) and
    // matches everything, matching `actor.list` with no `filter` argument.
    struct PINWRIGHT_API FFilter
    {
        FString Pattern;
        EMode Mode = EMode::Contains;
        bool bCaseSensitive = false;

        // Compiled once at Parse() time for EMode::Regex and reused for every
        // candidate; null for every other mode.
        TSharedPtr<FRegexPattern> CompiledPattern;

        // An empty pattern means "no filtering" - callers skip Matches() entirely and
        // must not materialize candidate strings just to feed it.
        bool IsActive() const { return !Pattern.IsEmpty(); }

        // True when Candidate satisfies the pattern under the resolved mode + case.
        // Always true for an inactive filter.
        bool Matches(const FString& Candidate) const;

        // Convenience for the two-column reads (label-OR-name, name-OR-class). Callers
        // whose extra column is expensive to build (GetPathName) should chain
        // !Matches(a) && !Matches(b) && !Matches(c) by hand instead, to keep the
        // short-circuit that avoids materializing it.
        bool MatchesEither(const FString& First, const FString& Second) const
        {
            return Matches(First) || Matches(Second);
        }
    };

    // Canonical wire name of a mode, for response echo and error messages. Always the
    // canonical spelling, never the alias the caller happened to send.
    PINWRIGHT_API const TCHAR* ModeToString(EMode Mode);

    // Comma-separated list of every accepted `matchMode` value, for error messages.
    PINWRIGHT_API FString ValidModeList();

    // Accepted wire keys, canonical first. Shared by the param specs (so the
    // dispatcher's known-params set honors the snake_case spelling) and by the
    // body-side reads, so a key can never be accepted at one layer and not the other.
    PINWRIGHT_API const TArray<FString>& MatchModeKeys();
    PINWRIGHT_API const TArray<FString>& CaseSensitiveKeys();

    // Param specs for the two knobs, alias-annotated. PatternParamName is the verb's
    // own pattern param ("filter" on actor.list) and is woven into the help text so a
    // verb that spells it differently still documents the pairing correctly.
    PINWRIGHT_API FParamSpec MatchModeParam(const TCHAR* PatternParamName);
    PINWRIGHT_API FParamSpec CaseSensitiveParam(const TCHAR* PatternParamName);

    // Reads pattern + matchMode + caseSensitive off the request and compiles the
    // filter. PatternKeys is the verb's pattern slot, canonical first (e.g. {"filter"}).
    // Returns false with a typed OutErrorCode / OutErrorMessage on:
    //   INVALID_MODE      - `matchMode` is not one of ValidModeList()
    //   INVALID_PATTERN   - `matchMode:"regex"` and the pattern does not compile
    //   INVALID_ARGUMENT  - matchMode / caseSensitive supplied with no pattern, which
    //                       would silently return the whole unfiltered list
    PINWRIGHT_API bool Parse(const FHandlerContext& Ctx, const TArray<FString>& PatternKeys,
                             FFilter& OutFilter, FString& OutErrorCode, FString& OutErrorMessage);

    // Parse + send the rejection through Ctx. Returns false when the handler must
    // bail: `if (!NameMatch::Require(Ctx, {TEXT("filter")}, Filter)) return true;`
    PINWRIGHT_API bool Require(const FHandlerContext& Ctx, const TArray<FString>& PatternKeys,
                               FFilter& OutFilter);

    // Echoes the resolved filter into a success payload under PatternFieldName,
    // `matchMode`, and `caseSensitive`. No-op for an inactive filter, so an unfiltered
    // response keeps its existing shape. The echo exists because the original bug was
    // undetectable from the response: a caller could not tell which semantics ran.
    PINWRIGHT_API void AddFilterEcho(const TSharedPtr<FJsonObject>& Out, const FFilter& Filter,
                                     const TCHAR* PatternFieldName);
}
