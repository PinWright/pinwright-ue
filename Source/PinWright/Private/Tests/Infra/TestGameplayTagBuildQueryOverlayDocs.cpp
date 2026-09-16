// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-build-query-overlay-not-merged-into-served-page:
// the served `gameplay_tags.build_query` method page was a param-only stub. Its rich
// op-token vocabulary had been authored as a STANDALONE docs/wiki-src/gameplay_tags.build_query.md
// topic file whose slug equals the registered method, so ClassifyNode (method-before-topic)
// permanently shadowed it and RenderMethodPage — which appends only the
// `### gameplay_tags.build_query` H3 section of gameplay_tags.md via
// WikiOverlay::LoadMethodSection — served nothing. The fix moved that body into an
// `### gameplay_tags.build_query` H3 section inside docs/wiki-src/gameplay_tags.md (the
// established per-method-overlay mechanism, mirroring the sibling `### gameplay_tags.list`)
// and deleted the redundant standalone file.
//
// This renders `gameplay_tags.build_query` through the live WikiHandler::RenderPage path
// (the same entry the HTTP gateway serves doc requests from), exercising RenderMethodPage
// -> LoadMethodSection against the real overlay file, not a copy. Every marker asserted
// below is overlay-exclusive: the auto-generated registry summary ("Build an
// FGameplayTagQuery from a recursive JSON expression tree ...") and the four param
// descriptions name no op token, no tokenStreamBytes/wrote/resolvedTargetPath result
// field, no UNKNOWN_OP/MIXED_PAYLOAD error code, and no FGameplayTagQueryExpression engine
// method. Reverting the H3 section makes LoadMethodSection return empty, drops the
// `## Notes` block, and these assertions fail.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameplayTagBuildQueryOverlayDocTest,
    "PinWright.infra.wiki_handler.MethodPage.GameplayTagBuildQueryOverlay",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameplayTagBuildQueryOverlayDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("gameplay_tags.build_query"), Text))
    {
        return false;
    }

    // The overlay body surfaces only via the `### gameplay_tags.build_query` H3 section,
    // rendered under `## Notes` on the method page. No `## Notes` => the section was dropped.
    TestTrue(TEXT("build_query method page carries the overlay `## Notes` block"),
        Text.Contains(TEXT("## Notes")));

    // (1) The six op tokens — the method's core vocabulary — must be enumerated. None of
    // these literals appear in the registry summary or param descriptions.
    const TCHAR* OpTokens[] = {
        TEXT("any_tags_match"), TEXT("all_tags_match"), TEXT("no_tags_match"),
        TEXT("any_expressions_match"), TEXT("all_expressions_match"), TEXT("no_expressions_match"),
    };
    for (const TCHAR* Op : OpTokens)
    {
        TestTrue(*FString::Printf(TEXT("build_query page enumerates the `%s` op token"), Op),
            Text.Contains(Op));
    }

    // (2) The op-to-engine-method mapping (overlay-exclusive: the summary says
    // "FGameplayTagQuery" but never "FGameplayTagQueryExpression").
    TestTrue(TEXT("build_query page maps ops to FGameplayTagQueryExpression engine methods"),
        Text.Contains(TEXT("FGameplayTagQueryExpression")));

    // (3) The result fields a caller cannot otherwise discover.
    TestTrue(TEXT("build_query page documents the tokenStreamBytes/wrote result fields"),
        Text.Contains(TEXT("tokenStreamBytes")) && Text.Contains(TEXT("wrote")));
    TestTrue(TEXT("build_query page documents the resolvedTargetPath CDO-redirect field"),
        Text.Contains(TEXT("resolvedTargetPath")));

    // (4) The error-code vocabulary.
    TestTrue(TEXT("build_query page documents the UNKNOWN_OP / MIXED_PAYLOAD error codes"),
        Text.Contains(TEXT("UNKNOWN_OP")) && Text.Contains(TEXT("MIXED_PAYLOAD")));

    return true;
}
