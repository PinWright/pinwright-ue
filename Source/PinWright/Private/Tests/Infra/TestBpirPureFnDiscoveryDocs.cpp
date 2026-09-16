// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-bpir-pure-fn-name-undiscoverable:
// the bpir.* pages a compile_bpir author reads (bpir.pure-impure classifies pure
// vs impure by syntax shape) carried no pointer to the documented
// blueprint.build_api_index -> blueprint.search_api discovery chain, and named no
// pure string-concat function, so agents grepped engine headers to learn a pure
// UFunction name (e.g. Concat_StrStr). The redirect existed only on
// docs/wiki-src/blueprint.graph.md — a page BPIR authors don't read. The fix ports
// that redirect onto docs/wiki-src/bpir.pure-impure.md as a `## Discovering the
// concrete pure function name` section that (1) points authors at
// build_api_index -> search_api (searching the plain verb) instead of engine
// headers, (2) states the float-pins-are-doubles `_DoubleDouble` vs `_FloatFloat`
// convention, and (3) carries a trimmed convenience table naming the highest-
// frequency pure utilities including the previously-absent string concat
// Concat_StrStr.
//
// This exercises the live WikiHandler::RenderPage render path (the same entry the
// HTTP gateway uses for doc requests), not a copy of the overlay text. bpir.pure-impure
// is a standalone TOPIC page, so its render body is H1 + overlay only — there is no
// auto-generated method/param content that could supply these markers. Every marker
// asserted below is therefore overlay-exclusive: reverting the section makes
// LoadGroupPrelude drop them and these assertions fail.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

// ============================================================================
// Topic page: the discovery-redirect section lives in a `##` (H2) section of
// docs/wiki-src/bpir.pure-impure.md, which the topic-page render path
// (RenderTopicPage -> LoadGroupPrelude, body up to the first H3) includes.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirPureFnDiscoveryDocTest,
    "PinWright.infra.wiki_handler.Topic.BpirPureFnDiscovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirPureFnDiscoveryDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("bpir.pure-impure"), Text))
    {
        return false;
    }

    // (1) CORE fix: the page redirects pure-name discovery to the documented
    // build_api_index -> search_api chain, searching the plain verb, NOT engine headers.
    TestTrue(TEXT("bpir.pure-impure redirects pure-name discovery to build_api_index/search_api"),
        Text.Contains(TEXT("build_api_index")) && Text.Contains(TEXT("search_api")));
    TestTrue(TEXT("bpir.pure-impure tells authors to search the plain verb"),
        Text.Contains(TEXT("plain verb")));
    TestTrue(TEXT("bpir.pure-impure steers authors off engine-header grepping"),
        Text.Contains(TEXT("engine header")));

    // (2) The genuinely-absent pure string-concat name is now on the page (the
    // exact function the reporter had to grep KismetStringLibrary.h for).
    TestTrue(TEXT("bpir.pure-impure names the pure string-concat function Concat_StrStr"),
        Text.Contains(TEXT("Concat_StrStr")));

    // (3) The float-pins-are-doubles convention (previously only on blueprint.graph.md)
    // is stated on the BPIR page so the _FloatFloat worked examples no longer mislead.
    TestTrue(TEXT("bpir.pure-impure states the _DoubleDouble vs _FloatFloat float-pin convention"),
        Text.Contains(TEXT("_DoubleDouble")) && Text.Contains(TEXT("_FloatFloat")));

    return true;
}
