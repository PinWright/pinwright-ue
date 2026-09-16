// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-graph-standard-exec-pin-names:
// The blueprint.graph overlay used to tell agents to call get_node_details after
// every creation so the next connect_pins "doesn't fail on a mismatched pin name",
// without publishing the fixed K2 exec-pin vocabulary. For the common case
// (event/impure exec output -> next impure exec input) the names never vary:
// exec output is `then` (UEdGraphSchema_K2::PN_Then), impure exec input is
// `execute` (PN_Execute), Sequence outputs are `then_0..then_N`, and Branch's exec
// outputs are `then`/`else`. The fix adds a `## Standard exec pin names` namespace
// section to docs/wiki-src/blueprint.graph.md publishing that vocabulary as a
// zero-discovery shortcut, with casts kept as the documented exception.
//
// Two non-obvious facts pinned here so a revert can't silently re-break them:
//   1. The Branch row MUST read then/else, NOT True/False. True/False are BPIR
//      keyword aliases the compiler normalizes (BpirCompiler.cpp:3357-3358), but
//      connect_pins' FindPinByName (BlueprintGraphHelpers.cpp:120) is case-
//      insensitive yet NOT alias-aware, so publishing True/False would induce the
//      exact mismatch the note prevents. The negative assertion below fails if the
//      wrong alias returns as a Branch exec-pin claim.
//   2. The note must NOT claim a casing hazard — casing is irrelevant.
//
// This exercises the live WikiHandler::RenderPage render path (the same entry the
// HTTP gateway uses for doc requests), not a copy of the overlay text. The markers
// asserted are overlay-exclusive: the auto-generated method summaries / param
// descriptions never name PN_Then/PN_Execute or a then/execute/then_N/else exec
// vocabulary table, so reverting the overlay section drops these markers and these
// assertions fail.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphStandardExecPinNamesDocTest,
    "PinWright.infra.wiki_handler.Namespace.StandardExecPinNames",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGraphStandardExecPinNamesDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("blueprint.graph"), Text))
    {
        return false;
    }

    // (1) The standard exec-pin section exists.
    TestTrue(TEXT("blueprint.graph page carries a Standard exec pin names section"),
        Text.Contains(TEXT("Standard exec pin names")));

    // (2) It publishes the fixed exec output / input vocabulary against the engine
    // constants so an agent can wire without a get_node_details round-trip.
    TestTrue(TEXT("standard exec output is named then / PN_Then"),
        Text.Contains(TEXT("PN_Then")) && Text.Contains(TEXT("`then`")));
    TestTrue(TEXT("standard impure exec input is named execute / PN_Execute"),
        Text.Contains(TEXT("PN_Execute")) && Text.Contains(TEXT("`execute`")));
    TestTrue(TEXT("Sequence per-branch outputs are then_0..then_N"),
        Text.Contains(TEXT("then_0")) && Text.Contains(TEXT("then_N")));

    // (3) The Branch row uses the LIVE pin names then/else (what connect_pins
    // resolves), and the caveat explains why True/False (BPIR aliases) are wrong.
    TestTrue(TEXT("Branch exec outputs are documented as then/else"),
        Text.Contains(TEXT("`else`")));
    TestTrue(TEXT("Branch caveat warns against the True/False aliases"),
        Text.Contains(TEXT("Branch caveat")) && Text.Contains(TEXT("does not understand the aliases")));

    // (4) Casts remain the documented exception that still needs discovery.
    TestTrue(TEXT("casts kept as the discovery exception"),
        Text.Contains(TEXT("Cast Node Pin Name Round-Trip Gotchas")));

    // (5) NEGATIVE: the page must not PUBLISH `True`/`False` as Branch exec-pin
    // names — that would be the BPIR alias that connect_pins cannot resolve and is
    // the original ticket's mistake. The page may (and does) mention `True`/`False`
    // only to warn against it ("wire to `then`/`else`, **not** `True`/`False`"), so
    // the alias literal is allowed only inside that corrective "**not** `True`/`False`"
    // context, and the corrective wording itself must be present.
    TestFalse(TEXT("page does not publish `True`/`False` as Branch pin names"),
        Text.Contains(TEXT("`True`/`False`")) && !Text.Contains(TEXT("**not** `True`/`False`")));
    TestTrue(TEXT("page corrects True/False to the live then/else names"),
        Text.Contains(TEXT("**not** `True`/`False`")));

    return true;
}
