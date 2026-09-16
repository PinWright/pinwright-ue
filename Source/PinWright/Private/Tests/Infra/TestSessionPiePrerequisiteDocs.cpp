// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-session-wiki-pie-prerequisite-undocumented:
// the `session` namespace overlay (docs/wiki-src/session.md) previously omitted the
// runtime precondition for its local-player roster methods. add_local_player /
// remove_local_player operate on the active game instance (SessionsHandler.cpp) and
// hard-fail with
//   [NO_GAME_INSTANCE] "No active game instance. Start Play-In-Editor first."
// when PIE is stopped.
//
// The fix adds a `## Prerequisite: ...` section to session.md (a `##` heading, so it
// renders on the namespace page but stays off the root index). These markers live ONLY
// in the authored overlay prose, not in the auto-generated `## Methods` index (whose
// method summaries never spell out NO_GAME_INSTANCE or the "Start Play-In-Editor first"
// remedy), so reverting the session.md edit makes the renderer serve the page without
// them and these assertions fail.
//
// This exercises the live WikiHandler::RenderPage render path (the same entry the HTTP
// gateway uses for doc requests) through WikiOverlay's on-disk load, not a copy of the
// overlay text.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionPiePrerequisiteDocTest,
    "PinWright.infra.wiki_handler.Namespace.SessionPiePrerequisite",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSessionPiePrerequisiteDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("session"), Text))
    {
        return false;
    }

    // The PIE prerequisite for the roster methods, with the exact error code +
    // remedy the handler emits. Both tokens live only in the authored ## section.
    TestTrue(TEXT("session page documents the [NO_GAME_INSTANCE] roster prerequisite"),
        Text.Contains(TEXT("NO_GAME_INSTANCE")));
    TestTrue(TEXT("session page names the remedy (Start Play-In-Editor first)"),
        Text.Contains(TEXT("Start Play-In-Editor first")));

    return true;
}
