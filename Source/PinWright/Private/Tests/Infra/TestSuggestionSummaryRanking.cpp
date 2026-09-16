// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the UNKNOWN_ACTION did-you-mean list.
//
// Measured defect: an agent that wanted to find an asset by name reached for
// `asset.find_by_name` and the live registry answered
//   "Did you mean: asset.find_by_tag, actor.find_by_name, actor.find_by_tag,
//    asset.bulk_rename, actor.find_by_class?"
// `asset.search` — the verb that actually answers a find-by-name question — was
// not in the list at all, and every entry was a bare dotted name, so nothing in
// the response distinguished a UMetaData tag search from a name search. The
// caller followed the top suggestion, got the wrong verb, fell back to
// asset.list on a folder, and spilled several megabytes to disk.
//
// Two causes, both structural rather than a bad ranking constant:
//   1. Scoring whole dotted strings makes the shared namespace prefix dominate
//      the Levenshtein term, so cross-namespace lexical twins (actor.find_by_*)
//      outrank the same-namespace verb that answers the question.
//   2. The registered Summary — the only place the word "name" appears for
//      asset.search — was never consulted, and never shown.
//
// Counterfactual for both tests below: with RankSuggestionsWithSummaries reverted
// to the name-only RankSuggestions, SummaryTierSurfacesSemanticMatch fails on the
// asset.search assertion (no summary text is searched, so the candidate never
// enters the pool) and NameQueryMissSuggestsAssetSearch fails on both the
// asset.search token and the summary-fragment token.

#include "Misc/AutomationTest.h"
#include "Catalog/SuggestionHelpers.h"
#include "Tests/Infra/DispatcherTestHelpers.h"

// ============================================================================
// Unit: the ranker prefers a same-namespace summary match over a cross-namespace
// lexical twin, and carries the summary into the returned entry.
//
// Synthetic pool rather than the live registry so the assertion is about the
// ranking rule and cannot drift when a verb is added elsewhere in the tree.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuggestionSummaryTierTest,
    "PinWright.infra.suggestions.SummaryTierSurfacesSemanticMatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuggestionSummaryTierTest::RunTest(const FString& Parameters)
{
    // Summaries abbreviated from the real registrations; the words that matter
    // are the ones the query tokens have to find ("name" for asset.search).
    const TArray<SuggestionHelpers::FSuggestionCandidate> Pool = {
        { TEXT("asset.search"),           TEXT("Find an asset by name. Case-insensitive substring by default.") },
        { TEXT("asset.find_by_tag"),      TEXT("Search the asset registry for assets whose UMetaData contains a given tag.") },
        { TEXT("asset.list"),             TEXT("Browse a folder: assets plus subfolders under one package path.") },
        { TEXT("actor.find_by_name"),     TEXT("Find a level actor by its display label.") },
        { TEXT("actor.find_by_class"),    TEXT("Find level actors of a class.") },
        { TEXT("material.set_parameter"), TEXT("Set a scalar or vector parameter on a material instance.") },
    };

    const TArray<FString> Ranked =
        SuggestionHelpers::RankSuggestionsWithSummaries(TEXT("asset.find_by_name"), Pool, 5);

    TestTrue(TEXT("ranker returned suggestions"), Ranked.Num() > 0);

    const FString Joined = FString::Join(Ranked, TEXT("; "));

    // (1) The verb that answers the question is present. This is the whole point:
    //     lexically `asset.find_by_tag` is the nearest neighbour of the query, so
    //     no amount of edit-distance tuning surfaces asset.search — only reading
    //     the summary does, because that is where the word "name" lives.
    TestTrue(TEXT("suggestions include asset.search for a name-shaped query"),
        Joined.Contains(TEXT("asset.search")));

    // (2) Each entry carries its summary, so a caller can tell a tag search from
    //     a name search WITHOUT spending a call to find out.
    TestTrue(TEXT("asset.search entry carries its summary text"),
        Joined.Contains(TEXT("Find an asset by name")));

    // (3) Same-namespace candidates outrank cross-namespace ones. That ordering
    //     is what frees the slots the actor.* twins were occupying; assert the
    //     position rather than the absence, since a cross-namespace entry in a
    //     later slot is fine.
    int32 LastAssetIndex = INDEX_NONE;
    int32 FirstNonAssetIndex = INDEX_NONE;
    for (int32 i = 0; i < Ranked.Num(); ++i)
    {
        if (Ranked[i].StartsWith(TEXT("asset.")))
        {
            LastAssetIndex = i;
        }
        else if (FirstNonAssetIndex == INDEX_NONE)
        {
            FirstNonAssetIndex = i;
        }
    }
    if (FirstNonAssetIndex != INDEX_NONE && LastAssetIndex != INDEX_NONE)
    {
        TestTrue(TEXT("every asset.* suggestion ranks above every cross-namespace one"),
            LastAssetIndex < FirstNonAssetIndex);
    }

    // (4) Negative calibration: a query with no namespace and no shared words must
    //     not drag the whole pool in through the keyword tier. Without the >= 2
    //     matched-token floor, a single common word would qualify every candidate.
    const TArray<FString> Garbage =
        SuggestionHelpers::RankSuggestionsWithSummaries(FString::ChrN(64, TEXT('z')), Pool, 5);
    TestEqual(TEXT("pure garbage yields no suggestions"), Garbage.Num(), 0);

    return true;
}

// ============================================================================
// Integration: the same fix, driven through FRpcDispatcher::ProcessRequest against
// the LIVE registry — the layer the contract actually lives on (rpc-design.md §1).
// The unit test above proves the rule; this proves the real 1191-method pool and
// the real registered summaries produce the same answer.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherNameQuerySuggestsAssetSearchTest,
    "PinWright.infra.dispatcher.NameQueryMissSuggestsAssetSearch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDispatcherNameQuerySuggestsAssetSearchTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    // The exact miss the reporter hit.
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Dispatcher.ProcessRequest(TEXT("req-name-search"), TEXT("asset.find_by_name"), Params);

    TestTrue(TEXT("Completion fired"), Sink->bWasCalled);
    TestEqual(TEXT("Error code is UNKNOWN_ACTION"), Sink->ErrorCode, TEXT("UNKNOWN_ACTION"));
    TestTrue(TEXT("Message offers suggestions"),
        Sink->Message.Contains(TEXT("Did you mean:")));

    // The headline assertion: the name-search verb is named.
    TestTrue(TEXT("a name-shaped miss suggests asset.search"),
        Sink->Message.Contains(TEXT("asset.search")));

    // And the caller can tell the suggestions apart. "FIND AN ASSET BY NAME" is
    // the opening of asset.search's registered summary and appears in no other
    // registration, so this proves the summary is both attached and the right one.
    TestTrue(TEXT("the suggestion carries asset.search's summary, not just its name"),
        Sink->Message.Contains(TEXT("FIND AN ASSET BY NAME")));

    return true;
}

// ============================================================================
// The boundary rule BOTH rankers share: a query that shares no character with a
// candidate must never be suggested for it.
//
// Sim normalises the edit distance by the SUMMED length, so a fully disjoint
// pair scores exactly 0.5 once the two lengths are equal — Dist is then
// max(LenA, LenB), the cost of retyping the name outright. `>=` admitted that;
// `>` does not. The rule is written twice — RankSuggestions and
// RankSuggestionsWithSummaries — and was wrong in both copies, which is why the
// counterfactual is pinned here once for both rather than beside either one.
//
// `~` is the disjoint character on purpose: it cannot occur in a dotted
// lower-snake_case method name or in a pwmodel op name (the tokenizer rejects it
// outright, IsPwIdentifierChar in PwModelTokenizer.cpp), so disjointness holds
// whatever ends up registered and the test cannot rot as the registry grows.
//
// ONE QUERY PER LENGTH, not one fixed-length garbage string. 0.5 is reached
// exactly when the lengths are equal, so a single query only probes the
// candidates that happen to match its length — which is precisely how the `>=`
// survived: the existing UnknownActionNoCloseMatch uses a 64-character query, so
// only a 64-character method name could ever trip it, and every shorter garbage
// string was landing on the same defect unseen.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuggestionDisjointQueryTest,
    "PinWright.infra.suggestions.DisjointQueryYieldsNoSuggestion",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuggestionDisjointQueryTest::RunTest(const FString& Parameters)
{
    // Spans both pools the two entry points serve: short pwmodel op names, long
    // dotted RPC method names, and the lengths in between. Synthetic, so the
    // assertion is about the rule and cannot drift when a verb is registered.
    const TArray<FString> NamePool = {
        TEXT("uv"),
        TEXT("box"),
        TEXT("bevel"),
        TEXT("sphere"),
        TEXT("subtract"),
        TEXT("asset.search"),
        TEXT("asset.find_by_tag"),
        TEXT("remove_degenerates"),
        TEXT("material.set_parameter"),
    };

    TArray<SuggestionHelpers::FSuggestionCandidate> SummaryPool;
    SummaryPool.Reserve(NamePool.Num());
    for (const FString& Name : NamePool)
    {
        // A summary made of ordinary words, so the keyword tier has a real
        // haystack to fail against rather than an empty one.
        SummaryPool.Add({ Name, TEXT("Find or change a thing by name, path or tag.") });
    }

    // Past the longest candidate, so every candidate is probed at its own length
    // and a few beyond it.
    for (int32 Len = 1; Len <= 24; ++Len)
    {
        const FString Query = FString::ChrN(Len, TEXT('~'));

        const TArray<FString> Names = SuggestionHelpers::RankSuggestions(Query, NamePool, 5);
        TestEqual(*FString::Printf(
                TEXT("RankSuggestions offers nothing for %d disjoint characters. Got: [%s]"),
                Len, *FString::Join(Names, TEXT(", "))),
            Names.Num(), 0);

        const TArray<FString> Entries =
            SuggestionHelpers::RankSuggestionsWithSummaries(Query, SummaryPool, 5);
        TestEqual(*FString::Printf(
                TEXT("RankSuggestionsWithSummaries offers nothing for %d disjoint characters. Got: [%s]"),
                Len, *FString::Join(Entries, TEXT(", "))),
            Entries.Num(), 0);
    }

    // The other half of the rule, so a future tightening of the cutoff cannot pass
    // this test by suggesting nothing ever. These are the typos the pwmodel
    // did-you-mean exists for; measured Sim is 0.83 / 0.93 / 0.93, nowhere near
    // the boundary, and each must still resolve to its intended target.
    struct FTypoCase { const TCHAR* Query; const TCHAR* Expected; };
    const FTypoCase TypoCases[] = {
        { TEXT("spehre"),  TEXT("sphere")   },  // transposition
        { TEXT("cylnder"), TEXT("cylinder") },  // deletion
        { TEXT("subtact"), TEXT("subtract") },  // deletion
    };
    const TArray<FString> TypoPool = {
        TEXT("box"), TEXT("sphere"), TEXT("cylinder"), TEXT("cone"), TEXT("bevel"),
        TEXT("subtract"), TEXT("union"), TEXT("intersection"), TEXT("spherify"),
    };
    for (const FTypoCase& Case : TypoCases)
    {
        const TArray<FString> Ranked = SuggestionHelpers::RankSuggestions(Case.Query, TypoPool, 1);
        TestEqual(*FString::Printf(TEXT("'%s' still resolves to '%s'"), Case.Query, Case.Expected),
            Ranked.Num() > 0 ? Ranked[0] : FString(), FString(Case.Expected));
    }

    return true;
}
