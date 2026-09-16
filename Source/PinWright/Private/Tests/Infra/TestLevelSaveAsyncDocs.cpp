// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for the two level-save response modes and their retained
// core-ticker safe-point continuation. These tests exercise WikiHandler::RenderPage,
// the production path used by call("level.save") and call("level.save_as").
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

namespace LevelSaveAsyncDocsTests
{
    void AssertResponseModesAndSafePoint(
        FAutomationTestBase& Test, const TCHAR* PageLabel, const FString& Text)
    {
        const FString Lower = Text.ToLower();
        Test.TestTrue(*FString::Printf(TEXT("%s documents the immediate running-ticket mode"),
            PageLabel),
            Lower.Contains(TEXT("plain json")) && Lower.Contains(TEXT("wait:false"))
                && Lower.Contains(TEXT("status:\"running\""))
                && Lower.Contains(TEXT("ticket_id"))
                && Lower.Contains(TEXT("system.job_status")));
        Test.TestTrue(*FString::Printf(TEXT("%s documents the complete streaming gate"),
            PageLabel),
            Lower.Contains(TEXT("progresstoken"))
                && Lower.Contains(TEXT("accept: text/event-stream"))
                && Lower.Contains(TEXT("wait:true")));
        Test.TestTrue(*FString::Printf(TEXT("%s documents the streamed terminal response"),
            PageLabel),
            Lower.Contains(TEXT("suppresses the immediate ticket"))
                && Lower.Contains(TEXT("terminal job response"))
                && Lower.Contains(TEXT("successful result"))
                && Lower.Contains(TEXT("failure"))
                && Lower.Contains(TEXT("original stream")));
        Test.TestTrue(*FString::Printf(TEXT("%s documents retained core-ticker safety"),
            PageLabel),
            Lower.Contains(TEXT("retained core-ticker safe-point continuation")));
        Test.TestTrue(*FString::Printf(TEXT("%s documents exactly one scheduled continuation"),
            PageLabel),
            Lower.Contains(TEXT("exactly one retained core-ticker safe-point continuation"))
                && Lower.Contains(TEXT("is scheduled")));
        Test.TestTrue(*FString::Printf(TEXT("%s documents the safe execution boundary"),
            PageLabel),
            Lower.Contains(TEXT("outside"))
                && Lower.Contains(TEXT("uworld::tick"))
                && Lower.Contains(TEXT("named-thread task pumps")));
        Test.TestTrue(*FString::Printf(TEXT("%s documents the existing bounded retry policy"),
            PageLabel),
            Lower.Contains(TEXT("mcpsafelevelsave"))
                && Lower.Contains(TEXT("retry policy"))
                && Lower.Contains(TEXT("up to five attempts")));
        Test.TestFalse(*FString::Printf(TEXT("%s has no stale AsyncTask wording"), PageLabel),
            Lower.Contains(TEXT("asynctask")));
    }
}

// ============================================================================
// Per-method page: the level.save method page (rendered from the `### level.save`
// overlay section) must document both response modes and the safe continuation.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelSaveAsyncDocTest,
    "PinWright.infra.wiki_handler.Method.LevelSaveAsync",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelSaveAsyncDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("level.save"), Text))
    {
        return false;
    }

    // The level.save per-method page must teach the async ticket -> system.job_status
    // poll contract: it names the verb, says the call is async / returns a ticket, and
    // points at system.job_status as the follow-up poll.
    static const TCHAR* const SaveVerb[] = { TEXT("level.save") };
    WikiDocTestHelpers::AssertAsyncTicketPollContract(*this, TEXT("level.save"), Text, SaveVerb);
    LevelSaveAsyncDocsTests::AssertResponseModesAndSafePoint(
        *this, TEXT("level.save"), Text);
    return true;
}

// ============================================================================
// Per-method page: level.save_as is rendered separately and must document the
// same two response modes and retained safe continuation as level.save.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelSaveAsAsyncDocTest,
    "PinWright.infra.wiki_handler.Method.LevelSaveAsAsync",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelSaveAsAsyncDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("level.save_as"), Text))
    {
        return false;
    }

    // The level.save_as per-method page must teach the same async ticket ->
    // system.job_status poll contract: it names the verb, says the call is async /
    // returns a ticket, and points at system.job_status as the follow-up poll.
    static const TCHAR* const SaveAsVerb[] = { TEXT("level.save_as") };
    WikiDocTestHelpers::AssertAsyncTicketPollContract(*this, TEXT("level.save_as"), Text, SaveAsVerb);
    LevelSaveAsyncDocsTests::AssertResponseModesAndSafePoint(
        *this, TEXT("level.save_as"), Text);
    return true;
}
