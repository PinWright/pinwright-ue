// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-graph-standard-exec-pin-names:
// The blueprint.graph overlay's blanket "always discover pins first" advice did
// not separate the fixed, universal exec-pin vocabulary (then/execute/then_N,
// Branch then/else) from the genuinely-variable cast/struct/param case, so a
// careful agent paid an avoidable get_node_details round-trip before every
// vanilla exec connect_pins. The fix adds a `## Standard exec-pin vocabulary`
// `##` section to docs/wiki-src/blueprint.graph.md publishing the fixed names so
// the pre-discovery is skippable for the common case.
//
// Two of the asserted markers guard reword-level correctness, not just presence:
//   * Branch exec outputs are published as `then`/`else` — the LIVE pin names
//     (EdGraphSchema_K2::PN_Then="then" / PN_Else="else"), NOT the editor display
//     labels `True`/`False` the original ticket proposed. Wiring `connect_pins`
//     to a literal "False" would silently miss the real `else` pin, so the test
//     asserts the section names `else` (the corrected value).
//   * The section states lookup is case-insensitive — the original ticket's
//     "casing failure" premise was false (FindPinByName falls back to
//     ESearchCase::IgnoreCase), so the published rationale must say so.
//
// This exercises the live WikiHandler::RenderPage render path (the same entry the
// HTTP gateway uses for doc requests), not a copy of the overlay text. Every
// marker asserted below is overlay-exclusive: the auto-generated method summaries
// and param descriptions publish no exec-pin vocabulary table, no `then_0..then_N`
// Sequence note, no Branch then/else mapping, and no case-insensitive-lookup
// rationale — so reverting the overlay section makes LoadGroupSections drop these
// markers and these assertions fail.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

// ============================================================================
// Namespace page: the `## Standard exec-pin vocabulary` section lives in a `##`
// block of docs/wiki-src/blueprint.graph.md, so it renders on the blueprint.graph
// namespace page (where connect_pins / create_node work is read).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphExecPinVocabularyDocTest,
    "PinWright.infra.wiki_handler.Namespace.GraphExecPinVocabulary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGraphExecPinVocabularyDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("blueprint.graph"), Text))
    {
        return false;
    }

    // (1) The standard exec-pin vocabulary section exists.
    TestTrue(TEXT("blueprint.graph page carries a Standard exec-pin vocabulary section"),
        Text.Contains(TEXT("Standard exec-pin vocabulary")));

    // (2) It publishes the fixed exec-pin names so discovery can be skipped.
    TestTrue(TEXT("vocab section names the engine exec-pin constants then/execute"),
        Text.Contains(TEXT("`then`")) && Text.Contains(TEXT("`execute`")));
    TestTrue(TEXT("vocab section documents Sequence then_0..then_N exec outputs"),
        Text.Contains(TEXT("then_0")) && Text.Contains(TEXT("then_N")));

    // (3) Branch exec outputs are published as the LIVE pin names `then`/`else`
    //     (PN_Then/PN_Else), NOT the display labels `True`/`False` the original
    //     ticket proposed. The presence of `else` mapped to Branch is the
    //     correctness-of-the-reword guard.
    TestTrue(TEXT("vocab section names the Branch K2Node_IfThenElse node"),
        Text.Contains(TEXT("K2Node_IfThenElse")));
    TestTrue(TEXT("vocab section publishes Branch false exec output as `else` (not False)"),
        Text.Contains(TEXT("`else`")));

    // (4) The published rationale states lookup is case-insensitive — the
    //     original "casing failure" premise was false (FindPinByName falls back
    //     to ESearchCase::IgnoreCase), so the doc must not claim casing fails.
    TestTrue(TEXT("vocab section states pin lookup is case-insensitive"),
        Text.Contains(TEXT("case-insensitive")));

    // (5) Casts remain the documented exception (still require discovery).
    TestTrue(TEXT("vocab section keeps casts as the discovery exception"),
        Text.Contains(TEXT("cast")) && Text.Contains(TEXT("get_node_details")));

    return true;
}
