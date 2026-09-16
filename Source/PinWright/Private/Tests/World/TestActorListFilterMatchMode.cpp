// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for board ticket B-actor-list-filter-case-mismatch.
//
// actor.list's `filter` was a bare FString::Contains, whose ESearchCase default is
// IgnoreCase, so it was a case-INSENSITIVE SUBSTRING match while the wiki documented
// it case-sensitive. Live damage: filter:"SH_" matched "Brush_0" (the lowercase sh_
// inside "Bru sh_ 0") and reported totalMatches:98 where the true SH_-prefixed count
// was 4, with nothing in the response to signal it.
//
// The fix adds opt-in `matchMode` (substring | prefix | exact | regex) and
// `caseSensitive` params. These tests pin BOTH halves of the contract:
//   - the legacy default STILL matches Brush_* from "SH_" (back-compat), and
//   - the new modes do NOT, which is what convention-prefix counting needs.
// A malformed regex must surface a typed INVALID_PATTERN instead of silently
// matching nothing (ICU swallows regex compile errors - see Utils/NameMatchFilter.cpp).
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Utils/NameMatchFilter.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

namespace ActorListFilterMatchModeTestUtils
{
    // The exact strings from the ticket: the requested naming-convention prefix, and
    // the actor that wrongly matched it because "Bru[sh_]0" carries a lowercase sh_.
    const TCHAR* const ReportedFilter = TEXT("SH_");
    const TCHAR* const TrapCandidate = TEXT("Brush_0");
    const TCHAR* const IntendedCandidate = TEXT("SH_Wall_01");

    // Builds a filter through the real wire-parse path so the tests cover key
    // resolution + validation, not just the match switch.
    bool ParseFilter(const TSharedPtr<FJsonObject>& Payload, NameMatch::FFilter& OutFilter,
                     FString& OutErrorCode, FString& OutErrorMessage)
    {
        const FHandlerContext Ctx = FHandlerContext::MakeTestContext(
            TEXT("test-id"), TEXT("actor.list"), Payload);
        return NameMatch::Parse(Ctx, TArray<FString>{TEXT("filter")},
            OutFilter, OutErrorCode, OutErrorMessage);
    }

    TSharedPtr<FJsonObject> MakeFilterPayload(const TCHAR* Filter, const TCHAR* MatchMode,
                                              const bool* CaseSensitive)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        if (Filter)
        {
            Payload->SetStringField(TEXT("filter"), Filter);
        }
        if (MatchMode)
        {
            Payload->SetStringField(TEXT("matchMode"), MatchMode);
        }
        if (CaseSensitive)
        {
            Payload->SetBoolField(TEXT("caseSensitive"), *CaseSensitive);
        }
        return Payload;
    }

    const FHandlerRegistration* FindActorListRegistration()
    {
        for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
        {
            if (Reg.MethodName == TEXT("actor.list"))
            {
                return &Reg;
            }
        }
        return nullptr;
    }

    // True when the registration declares ParamName either as a canonical name or as
    // an alias - the dispatcher's known-params set accepts both, so both count.
    bool DeclaresParamOrAlias(const FHandlerRegistration* Reg, const TCHAR* ParamName)
    {
        if (!Reg)
        {
            return false;
        }
        for (const FParamSpec& Spec : Reg->Params)
        {
            if (Spec.Name == ParamName || Spec.Aliases.Contains(FString(ParamName)))
            {
                return true;
            }
        }
        return false;
    }

    // Collects every `label` value out of an actor.list response.
    TArray<FString> CollectLabels(const FTestResponseCapture& Capture)
    {
        TArray<FString> Labels;
        const TArray<TSharedPtr<FJsonValue>>* ActorsArr = nullptr;
        if (!Capture.Result.IsValid()
            || !Capture.Result->TryGetArrayField(TEXT("actors"), ActorsArr)
            || !ActorsArr)
        {
            return Labels;
        }
        for (const TSharedPtr<FJsonValue>& Value : *ActorsArr)
        {
            const TSharedPtr<FJsonObject>* Row = nullptr;
            FString Label;
            if (Value.IsValid() && Value->TryGetObject(Row) && Row
                && (*Row)->TryGetStringField(TEXT("label"), Label))
            {
                Labels.Add(Label);
            }
        }
        return Labels;
    }

    bool AnyLabelStartsWith(const TArray<FString>& Labels, const FString& Prefix)
    {
        for (const FString& Label : Labels)
        {
            if (Label.StartsWith(Prefix, ESearchCase::CaseSensitive))
            {
                return true;
            }
        }
        return false;
    }
}

// ============================================================================
// Matching semantics — the reported failure, both directions
// ============================================================================

// The load-bearing assertion pair: the legacy default must STILL match Brush_0 from
// "SH_" (any other result breaks callers relying on today's behaviour), while each
// new opt-in must reject it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorListFilterCaseAndAnchorSemanticsTest,
    "PinWright.actor.list.FilterCaseAndAnchorSemantics",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorListFilterCaseAndAnchorSemanticsTest::RunTest(const FString& Parameters)
{
    using namespace ActorListFilterMatchModeTestUtils;

    const bool bTrue = true;
    const bool bFalse = false;

    auto Build = [this](const TCHAR* Filter, const TCHAR* MatchMode, const bool* CaseSensitive,
                        NameMatch::FFilter& Out) -> bool
    {
        FString ErrorCode;
        FString ErrorMessage;
        const bool bOk = ParseFilter(MakeFilterPayload(Filter, MatchMode, CaseSensitive),
            Out, ErrorCode, ErrorMessage);
        TestTrue(FString::Printf(TEXT("filter parsed (mode=%s): %s"),
            MatchMode ? MatchMode : TEXT("<default>"), *ErrorMessage), bOk);
        return bOk;
    };

    // BACK-COMPAT: no matchMode, no caseSensitive == today's behaviour exactly.
    // "SH_" must still match "Brush_0". If this flips, existing callers silently lose
    // rows they used to get, which is a worse break than the bug being fixed.
    {
        NameMatch::FFilter Filter;
        if (Build(ReportedFilter, nullptr, nullptr, Filter))
        {
            TestTrue(TEXT("legacy default is case-insensitive substring: SH_ still matches Brush_0"),
                Filter.Matches(TrapCandidate));
            TestTrue(TEXT("legacy default still matches the intended SH_Wall_01"),
                Filter.Matches(IntendedCandidate));
            TestEqual(TEXT("default matchMode is contains"),
                FString(NameMatch::ModeToString(Filter.Mode)), FString(TEXT("contains")));
            TestFalse(TEXT("default caseSensitive is false"), Filter.bCaseSensitive);
        }
    }

    // caseSensitive:true alone kills the trap: "Brush_0" carries lowercase sh_, not SH_.
    {
        NameMatch::FFilter Filter;
        if (Build(ReportedFilter, nullptr, &bTrue, Filter))
        {
            TestFalse(TEXT("caseSensitive substring: SH_ does NOT match Brush_0"),
                Filter.Matches(TrapCandidate));
            TestTrue(TEXT("caseSensitive substring still matches SH_Wall_01"),
                Filter.Matches(IntendedCandidate));
        }
    }

    // matchMode:"prefix" alone kills it too — Brush_0 contains sh_ but does not start
    // with it. This is the anchored form convention-prefix counting actually wants.
    {
        NameMatch::FFilter Filter;
        if (Build(ReportedFilter, TEXT("prefix"), nullptr, Filter))
        {
            TestFalse(TEXT("prefix mode: SH_ does NOT match Brush_0"),
                Filter.Matches(TrapCandidate));
            TestTrue(TEXT("prefix mode matches SH_Wall_01"),
                Filter.Matches(IntendedCandidate));
            // Case-insensitive by default even in prefix mode, so a lowercase-prefixed
            // label still matches unless caseSensitive is also set.
            TestTrue(TEXT("prefix mode is still case-insensitive by default"),
                Filter.Matches(TEXT("sh_lowercase_wall")));
        }
    }

    // Both knobs together — the exact call a prefix-counting caller should make.
    {
        NameMatch::FFilter Filter;
        if (Build(ReportedFilter, TEXT("prefix"), &bTrue, Filter))
        {
            TestFalse(TEXT("prefix+caseSensitive: SH_ does NOT match Brush_0"),
                Filter.Matches(TrapCandidate));
            TestFalse(TEXT("prefix+caseSensitive: SH_ does NOT match sh_lowercase_wall"),
                Filter.Matches(TEXT("sh_lowercase_wall")));
            TestTrue(TEXT("prefix+caseSensitive matches SH_Wall_01"),
                Filter.Matches(IntendedCandidate));
        }
    }

    // exact
    {
        NameMatch::FFilter Filter;
        if (Build(IntendedCandidate, TEXT("exact"), &bTrue, Filter))
        {
            TestTrue(TEXT("exact matches the identical string"),
                Filter.Matches(IntendedCandidate));
            TestFalse(TEXT("exact does not match a superstring"),
                Filter.Matches(TEXT("SH_Wall_01_Extra")));
            TestFalse(TEXT("exact+caseSensitive does not match a case variant"),
                Filter.Matches(TEXT("sh_wall_01")));
        }
    }

    // regex — unanchored search, so the caller anchors with ^ to express a prefix.
    {
        NameMatch::FFilter Filter;
        if (Build(TEXT("^SH_"), TEXT("regex"), &bTrue, Filter))
        {
            TestFalse(TEXT("anchored regex ^SH_ does NOT match Brush_0"),
                Filter.Matches(TrapCandidate));
            TestTrue(TEXT("anchored regex ^SH_ matches SH_Wall_01"),
                Filter.Matches(IntendedCandidate));
        }
    }

    // An explicit caseSensitive:false must be accepted and behave like the default,
    // not be treated as "absent". Also pins `substring` as an accepted alias for the
    // canonical `contains` (the spelling actor.list's own docs have always used).
    {
        NameMatch::FFilter Filter;
        if (Build(ReportedFilter, TEXT("substring"), &bFalse, Filter))
        {
            TestEqual(TEXT("'substring' resolves to the canonical 'contains'"),
                FString(NameMatch::ModeToString(Filter.Mode)), FString(TEXT("contains")));
            TestTrue(TEXT("explicit caseSensitive:false reproduces the legacy match"),
                Filter.Matches(TrapCandidate));
        }
    }

    // `starts_with` is blueprint.graph.find_nodes' spelling of prefix; a caller who
    // learned the vocabulary there must not hit INVALID_MODE here.
    {
        NameMatch::FFilter Filter;
        if (Build(ReportedFilter, TEXT("starts_with"), &bTrue, Filter))
        {
            TestEqual(TEXT("'starts_with' resolves to the canonical 'prefix'"),
                FString(NameMatch::ModeToString(Filter.Mode)), FString(TEXT("prefix")));
            TestFalse(TEXT("starts_with alias also rejects Brush_0"),
                Filter.Matches(TrapCandidate));
        }
    }

    // Snake_case spellings resolve to the same filter as their camelCase twins.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("filter"), ReportedFilter);
        Payload->SetStringField(TEXT("match_mode"), TEXT("prefix"));
        Payload->SetBoolField(TEXT("case_sensitive"), true);

        NameMatch::FFilter Filter;
        FString ErrorCode;
        FString ErrorMessage;
        if (TestTrue(TEXT("snake_case match_mode / case_sensitive parse"),
                ParseFilter(Payload, Filter, ErrorCode, ErrorMessage)))
        {
            TestEqual(TEXT("match_mode resolved to prefix"),
                FString(NameMatch::ModeToString(Filter.Mode)), FString(TEXT("prefix")));
            TestTrue(TEXT("case_sensitive resolved to true"), Filter.bCaseSensitive);
            TestFalse(TEXT("snake_case form also rejects Brush_0"),
                Filter.Matches(TrapCandidate));
        }
    }

    // An inactive (no-filter) filter keeps everything — the unfiltered actor.list path.
    {
        NameMatch::FFilter Filter;
        FString ErrorCode;
        FString ErrorMessage;
        if (TestTrue(TEXT("empty payload parses"),
                ParseFilter(MakeShared<FJsonObject>(), Filter, ErrorCode, ErrorMessage)))
        {
            TestFalse(TEXT("absent filter is inactive"), Filter.IsActive());
            TestTrue(TEXT("inactive filter matches everything"),
                Filter.Matches(TrapCandidate));
        }
    }

    return true;
}

// ============================================================================
// Typed rejections
// ============================================================================

// A malformed regex must be a typed error, never a silent zero-match: ICU drops the
// failed pattern and every subsequent FindNext() returns false, which a caller reads
// as a real "no actors matched".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorListFilterTypedRejectionsTest,
    "PinWright.actor.list.FilterTypedRejections",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorListFilterTypedRejectionsTest::RunTest(const FString& Parameters)
{
    using namespace ActorListFilterMatchModeTestUtils;

    auto ExpectRejection = [this](const TSharedPtr<FJsonObject>& Payload,
                                  const TCHAR* ExpectedCode, const TCHAR* What)
    {
        NameMatch::FFilter Filter;
        FString ErrorCode;
        FString ErrorMessage;
        const bool bOk = ParseFilter(Payload, Filter, ErrorCode, ErrorMessage);
        TestFalse(FString::Printf(TEXT("%s is rejected"), What), bOk);
        TestEqual(*FString::Printf(TEXT("%s error code"), What), ErrorCode, FString(ExpectedCode));
        TestFalse(FString::Printf(TEXT("%s carries a message"), What), ErrorMessage.IsEmpty());
    };

    const bool bTrue = true;

    // Unbalanced bracket — the canonical malformed pattern.
    ExpectRejection(MakeFilterPayload(TEXT("["), TEXT("regex"), nullptr),
        TEXT("INVALID_PATTERN"), TEXT("regex '['"));
    // Unbalanced group.
    ExpectRejection(MakeFilterPayload(TEXT("(SH_"), TEXT("regex"), nullptr),
        TEXT("INVALID_PATTERN"), TEXT("regex '(SH_'"));
    // Dangling quantifier.
    ExpectRejection(MakeFilterPayload(TEXT("a{2,"), TEXT("regex"), nullptr),
        TEXT("INVALID_PATTERN"), TEXT("regex 'a{2,'"));

    // Unknown matchMode must name the valid set rather than silently falling back to
    // substring (which is how actor.find_by_tag's matchType still behaves).
    ExpectRejection(MakeFilterPayload(TEXT("SH_"), TEXT("fuzzy"), nullptr),
        TEXT("INVALID_MODE"), TEXT("matchMode 'fuzzy'"));

    // Modifiers with no filter would return the entire unfiltered world while looking
    // like a filtered result — the same fabricated-count shape as the original bug.
    ExpectRejection(MakeFilterPayload(nullptr, TEXT("prefix"), nullptr),
        TEXT("INVALID_ARGUMENT"), TEXT("matchMode without filter"));
    ExpectRejection(MakeFilterPayload(nullptr, nullptr, &bTrue),
        TEXT("INVALID_ARGUMENT"), TEXT("caseSensitive without filter"));

    // A literal '[' under the default (non-regex) mode is just a character, never a
    // pattern error — the typed rejection must not leak into the legacy modes.
    {
        NameMatch::FFilter Filter;
        FString ErrorCode;
        FString ErrorMessage;
        TestTrue(TEXT("'[' is a plain substring outside regex mode"),
            ParseFilter(MakeFilterPayload(TEXT("["), nullptr, nullptr),
                Filter, ErrorCode, ErrorMessage));
        TestTrue(TEXT("'[' matches a label containing it"), Filter.Matches(TEXT("Foo[0]")));
    }

    return true;
}

// ============================================================================
// Registration contract
// ============================================================================

// The two knobs must be DECLARED on actor.list, aliases included: the dispatcher
// rejects any argument outside the declared known-params set, so an undeclared
// matchMode would hard-fail UNKNOWN_PARAMS at the wire before reaching the body.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorListFilterParamsDeclaredTest,
    "PinWright.actor.list.FilterParamsDeclared",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorListFilterParamsDeclaredTest::RunTest(const FString& Parameters)
{
    using namespace ActorListFilterMatchModeTestUtils;

    const FHandlerRegistration* Reg = FindActorListRegistration();
    if (!TestNotNull(TEXT("actor.list is registered"), Reg))
    {
        return false;
    }

    TestTrue(TEXT("actor.list declares filter"), DeclaresParamOrAlias(Reg, TEXT("filter")));
    TestTrue(TEXT("actor.list declares matchMode"), DeclaresParamOrAlias(Reg, TEXT("matchMode")));
    TestTrue(TEXT("actor.list declares match_mode alias"),
        DeclaresParamOrAlias(Reg, TEXT("match_mode")));
    TestTrue(TEXT("actor.list declares caseSensitive"),
        DeclaresParamOrAlias(Reg, TEXT("caseSensitive")));
    TestTrue(TEXT("actor.list declares case_sensitive alias"),
        DeclaresParamOrAlias(Reg, TEXT("case_sensitive")));
    // Both were already documented as accepted in the param help but were NOT declared,
    // so the dispatcher rejected them with UNKNOWN_PARAMS. Same docs-vs-behaviour class
    // as the case bug, fixed in the same pass.
    TestTrue(TEXT("actor.list declares names_only alias"),
        DeclaresParamOrAlias(Reg, TEXT("names_only")));
    TestTrue(TEXT("actor.list declares field alias"), DeclaresParamOrAlias(Reg, TEXT("field")));

    // The summary and the filter param help must not claim case-sensitivity: the wiki
    // page inherits this text, and the wrong claim is what made the bug credible.
    for (const FParamSpec& Spec : Reg->Params)
    {
        if (Spec.Name == TEXT("filter"))
        {
            TestFalse(TEXT("filter help no longer claims a bare 'case-sensitive' default"),
                Spec.Description.Contains(TEXT("(case-sensitive)"), ESearchCase::IgnoreCase));
        }
    }

    return true;
}

// ============================================================================
// End-to-end through the production handler
// ============================================================================

// Drives the real actor.list handler against two spawned actors that reproduce the
// ticket exactly: one legitimately SH_-prefixed, one whose label merely contains the
// lowercase sh_ ("Bru[sh_]..."). Asserts the legacy call returns both and each new
// mode returns only the first.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorListFilterEndToEndTest,
    "PinWright.actor.list.FilterEndToEnd",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorListFilterEndToEndTest::RunTest(const FString& Parameters)
{
    using namespace ActorListFilterMatchModeTestUtils;

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping actor.list filter end-to-end test."));
        return true;
    }

    // "Brush_..." is the ticket's trap: it contains a lowercase sh_ and so matches the
    // SH_ filter under the legacy case-insensitive substring rule.
    const FString IntendedLabel = TEXT("SH_ActorListFilterProbe");
    const FString TrapLabel = TEXT("Brush_ActorListFilterProbe");

    FScopedEditorWorldActorGuard WorldGuard;
    for (const FString& Label : {IntendedLabel, TrapLabel})
    {
        FTestResponseCapture SpawnCapture;
        TSharedPtr<FJsonObject> SpawnPayload = MakeShared<FJsonObject>();
        SpawnPayload->SetStringField(TEXT("classPath"), TEXT("/Script/Engine.PointLight"));
        SpawnPayload->SetStringField(TEXT("actorName"), Label);
        TestTrue(TEXT("actor.spawn handler is registered"),
            InvokeHandlerWithCapture(TEXT("actor.spawn"), SpawnPayload, SpawnCapture));
        TestTrue(FString::Printf(TEXT("spawned %s"), *Label), SpawnCapture.bSuccess);
    }

    // Every probe runs the same "SH_" filter against the editor world, projected to
    // labels only, and differs solely in the knob under test.
    auto ListLabels = [&](TFunctionRef<void(TSharedPtr<FJsonObject>)> Configure) -> TArray<FString>
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("filter"), ReportedFilter);
        Payload->SetStringField(TEXT("world"), TEXT("editor"));
        TArray<TSharedPtr<FJsonValue>> FieldsArr;
        FieldsArr.Add(MakeShared<FJsonValueString>(TEXT("label")));
        Payload->SetArrayField(TEXT("fields"), FieldsArr);
        Configure(Payload);
        TestTrue(TEXT("actor.list handler is registered"),
            InvokeHandlerWithCapture(TEXT("actor.list"), Payload, Capture));
        TestTrue(TEXT("actor.list succeeded"), Capture.bSuccess);
        return CollectLabels(Capture);
    };

    // BACK-COMPAT: the untouched call still returns the trap actor.
    {
        const TArray<FString> Labels = ListLabels([](TSharedPtr<FJsonObject>) {});
        TestTrue(TEXT("legacy filter returns the SH_-prefixed probe"),
            AnyLabelStartsWith(Labels, IntendedLabel));
        TestTrue(TEXT("legacy filter STILL returns the Brush_ trap probe (back-compat)"),
            AnyLabelStartsWith(Labels, TrapLabel));
    }

    // caseSensitive:true
    {
        const TArray<FString> Labels = ListLabels([](TSharedPtr<FJsonObject> Payload)
        {
            Payload->SetBoolField(TEXT("caseSensitive"), true);
        });
        TestTrue(TEXT("caseSensitive filter keeps the SH_-prefixed probe"),
            AnyLabelStartsWith(Labels, IntendedLabel));
        TestFalse(TEXT("caseSensitive filter drops the Brush_ trap probe"),
            AnyLabelStartsWith(Labels, TrapLabel));
    }

    // matchMode:"prefix"
    {
        const TArray<FString> Labels = ListLabels([](TSharedPtr<FJsonObject> Payload)
        {
            Payload->SetStringField(TEXT("matchMode"), TEXT("prefix"));
        });
        TestTrue(TEXT("prefix filter keeps the SH_-prefixed probe"),
            AnyLabelStartsWith(Labels, IntendedLabel));
        TestFalse(TEXT("prefix filter drops the Brush_ trap probe"),
            AnyLabelStartsWith(Labels, TrapLabel));
    }

    // The response must echo which semantics ran, so a counting caller can prove it.
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("filter"), ReportedFilter);
        Payload->SetStringField(TEXT("world"), TEXT("editor"));
        Payload->SetStringField(TEXT("matchMode"), TEXT("prefix"));
        Payload->SetBoolField(TEXT("caseSensitive"), true);
        TestTrue(TEXT("actor.list handler is registered"),
            InvokeHandlerWithCapture(TEXT("actor.list"), Payload, Capture));
        if (TestTrue(TEXT("actor.list succeeded"), Capture.bSuccess) && Capture.Result.IsValid())
        {
            FString EchoedMode;
            Capture.Result->TryGetStringField(TEXT("matchMode"), EchoedMode);
            bool bEchoedCase = false;
            Capture.Result->TryGetBoolField(TEXT("caseSensitive"), bEchoedCase);
            TestEqual(TEXT("response echoes the resolved matchMode"),
                EchoedMode, FString(TEXT("prefix")));
            TestTrue(TEXT("response echoes the resolved caseSensitive"), bEchoedCase);
        }
    }

    // An unfiltered call must keep its exact prior response shape: no filter echo,
    // and therefore no matchMode / caseSensitive keys.
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("world"), TEXT("editor"));
        Payload->SetNumberField(TEXT("limit"), 1);
        TestTrue(TEXT("actor.list handler is registered"),
            InvokeHandlerWithCapture(TEXT("actor.list"), Payload, Capture));
        if (TestTrue(TEXT("actor.list succeeded"), Capture.bSuccess) && Capture.Result.IsValid())
        {
            TestFalse(TEXT("unfiltered response carries no filter key"),
                Capture.Result->HasField(TEXT("filter")));
            TestFalse(TEXT("unfiltered response carries no matchMode key"),
                Capture.Result->HasField(TEXT("matchMode")));
            TestFalse(TEXT("unfiltered response carries no caseSensitive key"),
                Capture.Result->HasField(TEXT("caseSensitive")));
        }
    }

    // A malformed regex must surface through the handler as INVALID_PATTERN, not as a
    // successful empty list.
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("filter"), TEXT("["));
        Payload->SetStringField(TEXT("matchMode"), TEXT("regex"));
        Payload->SetStringField(TEXT("world"), TEXT("editor"));
        TestTrue(TEXT("actor.list handler is registered"),
            InvokeHandlerWithCapture(TEXT("actor.list"), Payload, Capture));
        TestFalse(TEXT("malformed regex does not succeed with an empty list"),
            Capture.bSuccess);
        TestEqual(TEXT("malformed regex returns INVALID_PATTERN"),
            Capture.ErrorCode, FString(TEXT("INVALID_PATTERN")));
    }

    return true;
}
