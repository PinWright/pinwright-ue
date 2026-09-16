// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-level-build-lighting-async-poll-undocumented:
// level.build_lighting is an async-job verb that returns a ticket synchronously
// (LevelHandler.cpp Ctx.StartJob) and runs the lightmap bake in the background.
// (Its build_level_lighting duplicate was removed in the RPC cull.) The `level` namespace overlay
// (docs/wiki-src/level.md) previously carried a stale "build_lighting blocks the
// editor until the lightmap bake completes ... prepared to wait" gotcha that
// directly CONTRADICTED that async model, never mentioned build_level_lighting,
// and never cross-linked the already-existing system.job_status poll contract.
//
// The fix rewrites the level.md `**Gotchas**` block (under the `## Cross-cluster
// overlap` section, so it renders on the namespace page but not the root index) to
// (a) state both lighting-build verbs are async ticket-returning jobs, NOT blocking
// calls, (b) name build_level_lighting alongside build_lighting, (c) point at
// system.job_status / the system `## Long-running jobs` section for the poll loop.
//
// This exercises the live WikiHandler::RenderPage render path (the same entry the
// HTTP gateway uses for doc requests), not a copy of the overlay text. Reverting
// the level.md edit makes the renderer serve the old stale gotcha and these
// assertions fail: the stale "blocks the editor" phrasing reappears (NegBlocks),
// build_level_lighting / system.job_status / the poll guidance vanish (Pos checks).
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

// ============================================================================
// Namespace page: the `## Cross-cluster overlap` → `**Gotchas**` block documents
// both lighting-build verbs as async ticket-returning jobs and cross-links the
// system.job_status poll contract. These markers live only in the level.md overlay.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelLightingBuildAsyncDocTest,
    "PinWright.infra.wiki_handler.Namespace.LevelLightingBuildAsync",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelLightingBuildAsyncDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("level"), Text))
    {
        return false;
    }

    // The level overlay must name the lighting-build verb (build_level_lighting was
    // removed as a superseded duplicate in the RPC cull) and teach the async
    // ticket -> system.job_status poll contract: ticket-returning, not blocking,
    // pointing at the poll as the follow-up.
    static const TCHAR* const LightingVerbs[] = { TEXT("level.build_lighting") };
    WikiDocTestHelpers::AssertAsyncTicketPollContract(*this, TEXT("level"), Text, LightingVerbs);

    // It must NOT carry the stale "blocks the editor ... prepared to wait" gotcha that
    // contradicts the async model. (The word "block" survives only in the negated
    // "do NOT block the editor" phrasing; we pin the specific stale claims instead.)
    TestFalse(TEXT("level page no longer claims build_lighting blocks the editor until the bake completes"),
        Text.Contains(TEXT("blocks the editor until the lightmap bake completes")));
    TestFalse(TEXT("level page no longer carries the stale 'prepared to wait' blocking gotcha"),
        Text.Contains(TEXT("prepared to wait")));
    return true;
}
