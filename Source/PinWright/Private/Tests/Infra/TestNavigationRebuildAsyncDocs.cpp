// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-navigation-rebuild-async-poll-undocumented:
// navigation.rebuild_navigation is an async-job verb that returns a ticket
// synchronously (NavigationHandler.cpp Ctx.StartJob — :335) and runs the nav-mesh
// rebuild in the background. The `navigation` namespace overlay
// (docs/wiki-src/navigation.md) previously mentioned rebuild_navigation only as the
// final step of the level-nav-setup order, with no note that the step is async, no
// mention of the job ticket it returns, and no cross-link to the already-existing
// system.job_status poll contract.
//
// The fix adds a `## Rebuilding navigation is async` section to navigation.md (a
// `##` heading, so it renders on the namespace page but not the root index) that
// (a) states rebuild_navigation is an async ticket-returning job, NOT a blocking
// call, (b) points at system.job_status as the poll follow-up, and annotates the
// setup-order line so the final rebuild step is marked async.
//
// This exercises the live WikiHandler::RenderPage render path (the same entry the
// HTTP gateway uses for doc requests), not a copy of the overlay text. Reverting
// the navigation.md edit makes the renderer serve the page without the async
// section and these assertions fail.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

// ============================================================================
// Namespace page: the `## Rebuilding navigation is async` section documents
// rebuild_navigation as an async ticket-returning job and cross-links the
// system.job_status poll contract. These markers live only in the navigation.md overlay.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavigationRebuildAsyncDocTest,
    "PinWright.infra.wiki_handler.Namespace.NavigationRebuildAsync",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNavigationRebuildAsyncDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("navigation"), Text))
    {
        return false;
    }

    // The navigation overlay must name the rebuild verb and teach the async ticket ->
    // system.job_status poll contract (ticket-returning, async, points at the poll).
    static const TCHAR* const NavVerbs[] = { TEXT("rebuild_navigation") };
    WikiDocTestHelpers::AssertAsyncTicketPollContract(*this, TEXT("navigation"), Text, NavVerbs);
    return true;
}
