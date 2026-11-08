// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/NameMatchFilter.h"

#include "Dom/JsonObject.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamAliasUtils.h"
#include "Utils/StringUtils.h"

// File-scope helpers live in a named namespace, not an anonymous one: Unity merges
// translation units and a plain `static ParseMode` would collide with a same-named
// static elsewhere in the cluster.
namespace NameMatchFilterInternal
{
    // ICU swallows regex compile errors. FRegexPattern's constructor reports nothing,
    // FICURegexManager::CreateRegexPattern drops the failed pattern on the floor, and
    // every FRegexMatcher::FindNext() against it then returns false - i.e. a malformed
    // pattern silently matches NOTHING, which is the same class of quiet-wrong-answer
    // this filter exists to eliminate.
    //
    // There is no public "did it compile" API, so probe instead: wrap the caller's
    // pattern in an optional non-capturing group. `(?:PATTERN)?` compiles if and only
    // if PATTERN compiles, and because the whole group is optional it can always match
    // the empty string. So a probe run against an empty input matching means the
    // pattern is well-formed; not matching means the wrapper (and therefore the
    // caller's pattern) failed to compile.
    bool CompileMatchPattern(const FString& Pattern, bool bCaseSensitive,
                             TSharedPtr<FRegexPattern>& OutCompiled)
    {
        const ERegexPatternFlags Flags = bCaseSensitive
            ? ERegexPatternFlags::None
            : ERegexPatternFlags::CaseInsensitive;

        const FString ProbeSource = FString::Printf(TEXT("(?:%s)?"), *Pattern);
        const FRegexPattern ProbePattern(ProbeSource, Flags);
        const FString EmptyInput;
        FRegexMatcher ProbeMatcher(ProbePattern, EmptyInput);
        if (!ProbeMatcher.FindNext())
        {
            OutCompiled.Reset();
            return false;
        }

        OutCompiled = MakeShared<FRegexPattern>(Pattern, Flags);
        return true;
    }

    // Presence (not truthiness) test across an alias set, so "supplied but false" is
    // distinguishable from "absent" - the difference between an explicit
    // caseSensitive:false and a caller who never mentioned case at all.
    bool PayloadHasAnyKey(const TSharedPtr<FJsonObject>& Payload, const TArray<FString>& Keys)
    {
        if (!Payload.IsValid())
        {
            return false;
        }
        for (const FString& Key : Keys)
        {
            if (Payload->HasField(Key))
            {
                return true;
            }
        }
        return false;
    }
}

namespace NameMatch
{

bool FFilter::Matches(const FString& Candidate) const
{
    if (!IsActive())
    {
        return true;
    }

    // Every comparison passes ESearchCase explicitly. The whole bug this file fixes was
    // an implicit default: FString::Contains and StartsWith default to IgnoreCase while
    // FString::Equals defaults to CaseSensitive, so a mode set that relied on defaults
    // would silently disagree with itself across modes.
    const ESearchCase::Type SearchCase = bCaseSensitive
        ? ESearchCase::CaseSensitive
        : ESearchCase::IgnoreCase;

    switch (Mode)
    {
    case EMode::Prefix:
        return Candidate.StartsWith(Pattern, SearchCase);

    case EMode::Exact:
        return Candidate.Equals(Pattern, SearchCase);

    case EMode::Regex:
    {
        if (!CompiledPattern.IsValid())
        {
            // Parse() refuses to hand back an active regex filter without a compiled
            // pattern, so reaching here means the filter was hand-built. Match nothing
            // rather than silently degrading to a substring search.
            return false;
        }
        FRegexMatcher Matcher(*CompiledPattern, Candidate);
        return Matcher.FindNext();
    }

    case EMode::Contains:
    default:
        return Candidate.Contains(Pattern, SearchCase);
    }
}

const TCHAR* ModeToString(EMode Mode)
{
    switch (Mode)
    {
    case EMode::Prefix:   return TEXT("prefix");
    case EMode::Exact:    return TEXT("exact");
    case EMode::Regex:    return TEXT("regex");
    case EMode::Contains:
    default:              return TEXT("contains");
    }
}

FString ValidModeList()
{
    return TEXT("contains (aliases: substring), prefix (aliases: starts_with), exact, regex");
}

const TArray<FString>& MatchModeKeys()
{
    static const TArray<FString> Keys = {TEXT("matchMode"), TEXT("match_mode")};
    return Keys;
}

const TArray<FString>& CaseSensitiveKeys()
{
    static const TArray<FString> Keys = {TEXT("caseSensitive"), TEXT("case_sensitive")};
    return Keys;
}

FParamSpec MatchModeParam(const TCHAR* PatternParamName)
{
    const FString Desc = FString::Printf(
        TEXT("How '%s' is matched: 'contains' (default, matches anywhere - the legacy behaviour; alias ")
        TEXT("'substring'), 'prefix' (candidate starts with the pattern - use this to count by ")
        TEXT("naming-convention prefix; alias 'starts_with'), 'exact' (full equality), or 'regex' ")
        TEXT("(unanchored ICU regex search, so anchor it yourself with ^; a pattern that does not compile ")
        TEXT("is rejected with INVALID_PATTERN rather than silently matching nothing). Same vocabulary as ")
        TEXT("blueprint.graph.find_nodes. Snake_case match_mode accepted. Ignored when '%s' is omitted."),
        PatternParamName, PatternParamName);

    FParamSpec Spec{MatchModeKeys()[0], TEXT("string"), Desc, /*bRequired=*/false, TEXT("contains")};
    Spec.Aliases = ParamAliasUtils::MakeAliasList(MatchModeKeys(), *MatchModeKeys()[0]);
    return Spec;
}

FParamSpec CaseSensitiveParam(const TCHAR* PatternParamName)
{
    const FString Desc = FString::Printf(
        TEXT("When true, '%s' is matched case-sensitively. Default false, i.e. case-INSENSITIVE - so the ")
        TEXT("default '%s':\"SH_\" also matches \"Brush_0\" (the lowercase sh_ inside it). Pass true ")
        TEXT("(usually with matchMode:'prefix') when counting actors by an upper-case naming prefix. ")
        TEXT("Snake_case case_sensitive accepted. Ignored when '%s' is omitted."),
        PatternParamName, PatternParamName, PatternParamName);

    FParamSpec Spec{CaseSensitiveKeys()[0], TEXT("bool"), Desc, /*bRequired=*/false, TEXT("false")};
    Spec.Aliases = ParamAliasUtils::MakeAliasList(CaseSensitiveKeys(), *CaseSensitiveKeys()[0]);
    return Spec;
}

bool Parse(const FHandlerContext& Ctx, const TArray<FString>& PatternKeys,
           FFilter& OutFilter, FString& OutErrorCode, FString& OutErrorMessage)
{
    OutFilter = FFilter();
    OutErrorCode.Empty();
    OutErrorMessage.Empty();

    OutFilter.Pattern = Ctx.GetStringFirstOf(PatternKeys);

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    const bool bModeSupplied =
        NameMatchFilterInternal::PayloadHasAnyKey(Payload, MatchModeKeys());
    const bool bCaseSupplied =
        NameMatchFilterInternal::PayloadHasAnyKey(Payload, CaseSensitiveKeys());

    // A knob with no pattern would quietly return the entire unfiltered list, which is
    // exactly the fabricated-count failure mode this ticket is about. Refuse instead.
    if (OutFilter.Pattern.IsEmpty() && (bModeSupplied || bCaseSupplied))
    {
        OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
        OutErrorMessage = FString::Printf(
            TEXT("'%s' and '%s' only apply to a non-empty '%s'. Supplied without one, every item would ")
            TEXT("match and the result would look like a filtered list. Pass '%s', or drop the modifiers."),
            *MatchModeKeys()[0], *CaseSensitiveKeys()[0], *PatternKeys[0], *PatternKeys[0]);
        return false;
    }

    if (bModeSupplied)
    {
        // NormalizeToken trims, lowercases, and folds '-' to '_', so "Starts-With"
        // resolves like "starts_with". The alias spellings mirror
        // BlueprintGraphInspectionHandler::ParseSearchMatchMode so a caller who
        // learned the vocabulary on blueprint.graph.find_nodes can reuse it verbatim.
        const FString RawMode = PinWright::NormalizeToken(Ctx.GetStringFirstOf(MatchModeKeys()));
        if (RawMode == TEXT("contains") || RawMode == TEXT("substring"))
        {
            OutFilter.Mode = EMode::Contains;
        }
        else if (RawMode == TEXT("prefix") || RawMode == TEXT("starts_with"))
        {
            OutFilter.Mode = EMode::Prefix;
        }
        else if (RawMode == TEXT("exact"))
        {
            OutFilter.Mode = EMode::Exact;
        }
        else if (RawMode == TEXT("regex"))
        {
            OutFilter.Mode = EMode::Regex;
        }
        else
        {
            OutErrorCode = ErrorCodes::ERR_INVALID_MODE;
            OutErrorMessage = FString::Printf(
                TEXT("Unknown %s '%s'. Valid values: %s."),
                *MatchModeKeys()[0], *Ctx.GetStringFirstOf(MatchModeKeys()), *ValidModeList());
            return false;
        }
    }

    if (bCaseSupplied)
    {
        OutFilter.bCaseSensitive = Ctx.GetBoolFirstOf(CaseSensitiveKeys(), false);
    }

    if (OutFilter.Mode == EMode::Regex && OutFilter.IsActive())
    {
        if (!NameMatchFilterInternal::CompileMatchPattern(
                OutFilter.Pattern, OutFilter.bCaseSensitive, OutFilter.CompiledPattern))
        {
            OutErrorCode = ErrorCodes::ERR_INVALID_PATTERN;
            OutErrorMessage = FString::Printf(
                TEXT("'%s' is not a valid regular expression under matchMode:'regex': '%s'. ")
                TEXT("Rejected rather than silently matching nothing. Use matchMode:'prefix' or ")
                TEXT("'contains' for a literal fragment, or escape the regex metacharacters."),
                *PatternKeys[0], *OutFilter.Pattern);
            OutFilter = FFilter();
            return false;
        }
    }

    return true;
}

bool Require(const FHandlerContext& Ctx, const TArray<FString>& PatternKeys, FFilter& OutFilter)
{
    FString ErrorCode;
    FString ErrorMessage;
    if (!Parse(Ctx, PatternKeys, OutFilter, ErrorCode, ErrorMessage))
    {
        Ctx.SendError(ErrorCode, ErrorMessage);
        return false;
    }
    return true;
}

void AddFilterEcho(const TSharedPtr<FJsonObject>& Out, const FFilter& Filter,
                   const TCHAR* PatternFieldName)
{
    if (!Out.IsValid() || !Filter.IsActive())
    {
        return;
    }
    Out->SetStringField(PatternFieldName, Filter.Pattern);
    Out->SetStringField(TEXT("matchMode"), ModeToString(Filter.Mode));
    Out->SetBoolField(TEXT("caseSensitive"), Filter.bCaseSensitive);
}

} // namespace NameMatch
