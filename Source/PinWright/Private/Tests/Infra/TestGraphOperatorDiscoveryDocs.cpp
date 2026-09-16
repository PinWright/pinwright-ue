// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-create-node-operator-symbol-discovery:
// blueprint.graph.list_node_types enumerates only K2Node CONTAINER class names
// (CallFunction, Branch, VariableGet) and gives no mapping from a human operator
// (`-`, `<=`) to the KismetMathLibrary UFunction (`Subtract_DoubleDouble`,
// `LessEqual_DoubleDouble`) / pin (`A`/`B`/`ReturnValue`, `InString`) that a
// `CallFunction` actually targets — so agents read engine headers instead of using
// the documented blueprint.build_api_index -> blueprint.search_api chain. The fix
// adds, to docs/wiki-src/blueprint.graph.md: (1) a `## Operator cheat-sheet
// (CallFunction targets)` namespace section mapping the common operator symbols to
// their <Op>_DoubleDouble / PrintString functions and pins, and (2) a redirect in
// the `## Cross-cluster overlap` block pointing an agent at list_node_types toward
// build_api_index -> search_api (searching the plain verb).
//
// This exercises the live WikiHandler::RenderPage render path (the same entry the
// HTTP gateway uses for doc requests), not a copy of the overlay text. Every marker
// asserted below is overlay-exclusive: the auto-generated method summaries and param
// descriptions name no operator symbol, no <Op>_DoubleDouble function, no A/B/
// ReturnValue/InString pins, and no search_api redirect — so reverting the overlay
// section makes LoadGroupPrelude / LoadGroupSections drop these markers and these
// assertions fail.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

// ============================================================================
// Namespace page: the operator cheat-sheet section and the list_node_types ->
// search_api redirect both live in `##` sections of docs/wiki-src/blueprint.graph.md,
// so they render on the blueprint.graph namespace page.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphOperatorDiscoveryDocTest,
    "PinWright.infra.wiki_handler.Namespace.GraphOperatorDiscovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGraphOperatorDiscoveryDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("blueprint.graph"), Text))
    {
        return false;
    }

    // (1) The operator cheat-sheet section exists and maps symbols to UFunctions.
    TestTrue(TEXT("blueprint.graph page carries an Operator cheat-sheet section"),
        Text.Contains(TEXT("Operator cheat-sheet")));
    TestTrue(TEXT("blueprint.graph cheat-sheet maps `-` to Subtract_DoubleDouble"),
        Text.Contains(TEXT("Subtract_DoubleDouble")));
    TestTrue(TEXT("blueprint.graph cheat-sheet maps `<=` to LessEqual_DoubleDouble"),
        Text.Contains(TEXT("LessEqual_DoubleDouble")));
    TestTrue(TEXT("blueprint.graph cheat-sheet maps print to PrintString"),
        Text.Contains(TEXT("PrintString")));
    // The pin names are stated so the next connect_pins call doesn't have to guess.
    TestTrue(TEXT("blueprint.graph cheat-sheet states math A/B/ReturnValue pins"),
        Text.Contains(TEXT("ReturnValue")) && Text.Contains(TEXT("InString")));
    // The cheat-sheet attributes the functions to the right standard libraries.
    TestTrue(TEXT("blueprint.graph cheat-sheet names KismetMathLibrary / KismetSystemLibrary"),
        Text.Contains(TEXT("KismetMathLibrary")) && Text.Contains(TEXT("KismetSystemLibrary")));

    // (2) The list_node_types -> search_api redirect: an agent at the catalog is
    // pointed at the discovery chain (search the plain verb), not engine headers.
    TestTrue(TEXT("blueprint.graph page redirects CallFunction-target discovery to search_api"),
        Text.Contains(TEXT("build_api_index")) && Text.Contains(TEXT("search_api")));
    TestTrue(TEXT("blueprint.graph page tells agents to search the plain verb"),
        Text.Contains(TEXT("plain verb")));

    return true;
}
