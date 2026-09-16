// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-anim-node-name-convention-undocumented:
// The AnimGraph `nodeName` convention — every mutator (bind_player_asset,
// set_sync_group, set_anim_graph_node_value, set_anim_graph_pin_exposed) resolves
// `nodeName` by a list-view-title match (ResolveAnimGraphNodeByTitle: exact, then
// substring), a player node's title is derived from its bound asset, there is no
// rename verb, and a multi-match is rejected with AMBIGUOUS_NODE — was referenced
// twice in docs/wiki-src/animation.authoring.md ("the same nodeName convention as
// bind_player_asset") but never stated, forcing agents to read plugin C++. The fix
// adds a `## Naming and resolving AnimGraph nodes` namespace section that states the
// convention explicitly, plus steer lines on the bind_player_asset / set_sync_group /
// add_graph_node overlay sections pointing at it.
//
// This exercises the live WikiHandler::RenderPage render path (the same entry the
// HTTP gateway uses for doc requests), not a copy of the overlay text. Every marker
// asserted below is overlay-exclusive: the auto-generated method summaries and param
// descriptions name no title-substring resolution, no asset-derived title, no
// "no rename verb", and no bind-named-asset-at-creation strategy — so reverting the
// overlay section makes LoadGroupPrelude / LoadGroupSections return empty and these
// assertions fail.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

// ============================================================================
// Namespace page: the `## Naming and resolving AnimGraph nodes` section states the
// whole convention — title-substring resolution, asset-derived title, no rename
// verb, the bind-named-asset-at-creation strategy, and the AMBIGUOUS_NODE rejection.
// These markers live only in the namespace `##` section of
// docs/wiki-src/animation.authoring.md.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimGraphNodeNamingNamespaceDocTest,
    "PinWright.infra.wiki_handler.Namespace.AnimGraphNodeNamingConvention",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimGraphNodeNamingNamespaceDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("animation.authoring"), Text))
    {
        return false;
    }

    // The naming section must exist on the namespace page.
    TestTrue(TEXT("animation.authoring page carries a Naming and resolving AnimGraph nodes section"),
        Text.Contains(TEXT("Naming and resolving AnimGraph nodes")));
    // Convention #1: nodeName is matched against the list-view title via the resolver,
    // not a stable id. Naming the live resolver (not the stale FindAnimGraphNodeByTitleSubstring).
    TestTrue(TEXT("animation.authoring page names the ResolveAnimGraphNodeByTitle resolver"),
        Text.Contains(TEXT("ResolveAnimGraphNodeByTitle")));
    TestTrue(TEXT("animation.authoring page states nodeName matches the list-view title, not a stable id"),
        Text.Contains(TEXT("list-view title")) && Text.Contains(TEXT("not a stable id")));
    // Convention #2: a player node's title is asset-derived and there is no rename verb.
    TestTrue(TEXT("animation.authoring page states a player node title is derived from its bound asset"),
        Text.Contains(TEXT("derived from its bound asset")));
    TestTrue(TEXT("animation.authoring page states there is no rename verb for AnimGraph nodes"),
        Text.Contains(TEXT("no rename verb")));
    // Convention #3: the bind-distinctly-named-asset-at-creation resolution strategy.
    TestTrue(TEXT("animation.authoring page gives the bind-named-asset-at-creation strategy"),
        Text.Contains(TEXT("bindAsset")) && Text.Contains(TEXT("add_graph_node")));
    TestTrue(TEXT("animation.authoring page warns against the shared 'Sequence Player' prefix"),
        Text.Contains(TEXT("Sequence Player")));
    // Convention #4: ambiguous nodeName is rejected, not silently picked (the live
    // post-fix behavior, cross-linked to the resolver-code ticket).
    TestTrue(TEXT("animation.authoring page states a multi-match returns AMBIGUOUS_NODE"),
        Text.Contains(TEXT("AMBIGUOUS_NODE")));
    return true;
}

// ============================================================================
// bind_player_asset method page: the steer line pointing at the naming section,
// stating the title is asset-derived and there is no rename verb. The H3 overlay
// for bind_player_asset surfaces only when the method page is rendered directly.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimBindPlayerAssetNamingDocTest,
    "PinWright.infra.wiki_handler.MethodPage.AnimBindPlayerAssetNaming",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimBindPlayerAssetNamingDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("animation.authoring.bind_player_asset"), Text))
    {
        return false;
    }

    // The steer line points at the shared naming convention and restates the no-rename /
    // asset-derived-title facts — overlay-exclusive (the auto summary/param descs say none of this).
    TestTrue(TEXT("bind_player_asset page steers to the Naming and resolving AnimGraph nodes section"),
        Text.Contains(TEXT("Naming and resolving AnimGraph nodes")));
    TestTrue(TEXT("bind_player_asset page states there is no rename verb"),
        Text.Contains(TEXT("no rename verb")));
    return true;
}
