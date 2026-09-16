// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-game-framework-info-not-asset-readback:
// the `game_framework` namespace's get_game_framework_info verb is a level-spawn
// snapshot (RPC_NO_PARAMS; reads WorldSettings->DefaultGameMode + PlayerStart
// actors at GameFrameworkHandler.cpp:865-911), NOT a read-back of the standalone
// GameMode asset the set_*/configure_* write verbs mutate (each takes a required
// gameModeBlueprint path and persists onto that asset's CDO). The base
// game_framework.md overlay was a bare 2-sentence prelude that never documented
// this level-vs-asset asymmetry, so the natural configure-then-get_game_framework_info
// confirm loop could not close and agents had to discover blueprint.inspect on their own.
//
// The fix is docs-only: the `## Reading back a configured GameMode asset` namespace
// section in docs/wiki-src/game_framework.md now states that get_game_framework_info
// reports the active LEVEL's spawn state and does NOT echo the configured asset,
// that a "(default)" / unrelated-GameMode answer post-configure is expected (not a
// failure), and routes the asset-CDO read-back to
// blueprint.inspect {assetPath, includeProperties:true} (the F-rpc-blueprint-class-properties path).
//
// This exercises the live WikiHandler::RenderPage render path (the same entry the
// HTTP gateway uses for doc requests), not a copy of the overlay text. The base
// game_framework.md prelude is a 2-sentence namespace blurb that names no method and
// no readback coverage, so every marker asserted below is overlay-exclusive: reverting
// the `## Reading back a configured GameMode asset` section makes the renderer return
// the prelude only and these assertions fail.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

// ============================================================================
// Namespace page: the `## Reading back a configured GameMode asset` section
// documents that get_game_framework_info is a level snapshot, not an asset
// read-back, and points at blueprint.inspect {includeProperties} for the CDO.
// These markers live only in the namespace `##` section of game_framework.md.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameFrameworkAssetReadbackNamespaceDocTest,
    "PinWright.infra.wiki_handler.Namespace.GameFrameworkAssetReadback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameFrameworkAssetReadbackNamespaceDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("game_framework"), Text))
    {
        return false;
    }

    // The asset-read-back section must exist on the namespace page.
    TestTrue(TEXT("game_framework page carries a Reading back a configured GameMode asset section"),
        Text.Contains(TEXT("Reading back a configured GameMode asset")));

    // It must name the write-verb input (gameModeBlueprint asset CDO) the level
    // reader cannot echo, so the level-vs-asset asymmetry is explicit.
    TestTrue(TEXT("game_framework page contrasts the gameModeBlueprint asset CDO with the level reader"),
        Text.Contains(TEXT("gameModeBlueprint")) && Text.Contains(TEXT("CDO")));

    // It must spell out that get_game_framework_info reports the active level's
    // spawn state (WorldSettings->DefaultGameMode + PlayerStarts), not the asset.
    TestTrue(TEXT("game_framework page states get_game_framework_info reports the active level spawn state"),
        Text.Contains(TEXT("get_game_framework_info")) && Text.Contains(TEXT("WorldSettings->DefaultGameMode")));

    // It must mark the post-configure "(default)" answer as expected, not a failure.
    TestTrue(TEXT("game_framework page calls the post-configure (default) answer expected, not a failure"),
        Text.Contains(TEXT("(default)")) && Text.Contains(TEXT("expected, not a failure")));

    // It must route the CDO read-back to blueprint.inspect {includeProperties}.
    TestTrue(TEXT("game_framework page routes the asset CDO read-back to blueprint.inspect includeProperties"),
        Text.Contains(TEXT("blueprint.inspect")) && Text.Contains(TEXT("includeProperties")));
    return true;
}
