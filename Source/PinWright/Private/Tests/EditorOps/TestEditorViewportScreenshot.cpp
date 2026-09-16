// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for F-editor-viewport-screenshot: editor.screenshot must
// capture the active level-editor viewport when no game/PIE viewport exists,
// instead of failing with NO_VIEWPORT.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "State/PluginState.h"
#include "State/JobRegistry.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "HAL/FileManager.h"
#include "IAssetViewport.h"
#include "LevelEditor.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"

namespace
{
    bool HasPngMagic(const TArray<uint8>& Bytes)
    {
        // Standard PNG signature: 89 50 4E 47 0D 0A 1A 0A
        static const uint8 Magic[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
        if (Bytes.Num() < 8) return false;
        for (int32 I = 0; I < 8; ++I)
        {
            if (Bytes[I] != Magic[I]) return false;
        }
        return true;
    }

    // Shared verification for both editor.screenshot regressions: invoke the
    // handler, confirm the synchronous "running" envelope carries a ticket_id,
    // then assert the job is terminal (never hung in "running") and — when it
    // completed — that it wrote a valid PNG under Screenshots/<Filename> with
    // positive dimensions. bAllowFailedTerminal distinguishes the two cases: the
    // level-viewport fallback (a live viewport is guaranteed by the caller's skip
    // guard) must end "completed"; the PIE path accepts "failed" with a real error
    // code as an alternative terminal status, the only forbidden outcome being the
    // "running" hang the ticket reports.
    void RunScreenshotTerminalCheck(FAutomationTestBase& Test, const FString& Filename,
        bool bAllowFailedTerminal)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("filename"), Filename);

        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("editor.screenshot handler found"),
            InvokeHandlerWithCapture(TEXT("editor.screenshot"), Payload, Capture));
        Test.TestTrue(TEXT("running envelope returned"), Capture.bSuccess);
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            return;
        }

        FString TicketId;
        Test.TestTrue(TEXT("running envelope carries ticket_id"),
            Capture.Result->TryGetStringField(TEXT("ticket_id"), TicketId));
        if (TicketId.IsEmpty())
        {
            return;
        }

        FJobTicket Ticket;
        Test.TestTrue(TEXT("job ticket exists in registry"),
            FPluginState::Get().GetJobRegistry().Get(TicketId, Ticket));

        // The capture runs synchronously inside StartJob, so the ticket is already
        // terminal on return. The reverted async handler leaves it "running" here.
        if (bAllowFailedTerminal)
        {
            Test.TestTrue(TEXT("job is terminal, not hung in running"),
                Ticket.Status != TEXT("running"));
            if (Ticket.Status == TEXT("running"))
            {
                return;
            }
            if (Ticket.Status != TEXT("completed"))
            {
                return; // "failed" with a concrete error code is an acceptable terminal status.
            }
        }
        else
        {
            Test.TestEqual(TEXT("job is terminal completed"), Ticket.Status, FString(TEXT("completed")));
            if (Ticket.Status != TEXT("completed"))
            {
                return;
            }
        }

        // Completed: it wrote a valid PNG under Screenshots/<Filename>.
        Test.TestTrue(TEXT("completed job has a result"), Ticket.Result.IsValid());
        if (!Ticket.Result.IsValid())
        {
            return;
        }

        FString Path;
        Test.TestTrue(TEXT("result has path"), Ticket.Result->TryGetStringField(TEXT("path"), Path));
        Test.TestTrue(TEXT("path is under Screenshots/"), Path.Contains(TEXT("Screenshots/")));
        Test.TestTrue(TEXT("path keeps requested filename"), Path.Contains(Filename));

        const int32 Width = static_cast<int32>(Ticket.Result->GetNumberField(TEXT("width")));
        const int32 Height = static_cast<int32>(Ticket.Result->GetNumberField(TEXT("height")));
        Test.TestTrue(TEXT("width is positive"), Width > 0);
        Test.TestTrue(TEXT("height is positive"), Height > 0);

        ON_SCOPE_EXIT
        {
            if (!Path.IsEmpty())
            {
                IFileManager::Get().Delete(*Path, false, true);
            }
        };

        Test.TestTrue(TEXT("PNG file exists on disk"), IFileManager::Get().FileExists(*Path));
        TArray<uint8> Bytes;
        Test.TestTrue(TEXT("PNG file loaded"), FFileHelper::LoadFileToArray(Bytes, *Path));
        Test.TestTrue(TEXT("PNG file is non-empty"), Bytes.Num() > 0);
        Test.TestTrue(TEXT("PNG file has PNG signature"), HasPngMagic(Bytes));
    }
}

// editor.screenshot outside PIE (no GEngine->GameViewport) must complete by
// capturing the active level-editor viewport, returning {path, width, height}
// and writing a valid PNG.
//
// Counterfactual: the reverted (GameViewport-only) handler returns
// status:"failed", error:"NO_VIEWPORT" here, which fails the strict
// completion/PNG assertions below. Headless runs with no resolvable level
// viewport are skipped up front so the test never false-negatives — exactly
// the live-viewport guard pattern used by SetViewModeExecTest.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorScreenshotLevelViewportFallbackTest,
    "PinWright.editor.screenshot.LevelViewportFallback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorScreenshotLevelViewportFallbackTest::RunTest(const FString& Parameters)
{
    // This test exercises the no-game-viewport fallback; it only makes sense
    // when there is genuinely no game/PIE viewport bound (the normal editor state).
    if (GEngine && GEngine->GameViewport)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("game-viewport-present"),
            TEXT("Game viewport present (PIE); fallback path not exercised."));
        return true;
    }

    // Skip when no active level viewport is resolvable: the fallback's
    // GetFirstActiveViewport path is unreachable (e.g. headless capture not
    // possible), so asserting completion would false-negative. With the fix
    // reverted but a viewport present, the strict assertions below still fail.
    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Skipped: GEditor is null; no editor to resolve a level viewport against."));
        return true;
    }
    FLevelEditorModule& LevelEditorModule =
        FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
    TSharedPtr<IAssetViewport> ActiveViewport = LevelEditorModule.GetFirstActiveViewport();
    if (!ActiveViewport.IsValid() || !ActiveViewport->GetSharedActiveViewport().IsValid())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-level-viewport"),
            TEXT("Skipped: no active level viewport; the fallback's capture path is unreachable in this run."));
        return true;
    }

    // A live level viewport is present (guarded above), so the fix must complete
    // the job. The reverted handler returns failed/NO_VIEWPORT and fails here.
    RunScreenshotTerminalCheck(*this, TEXT("test_level_viewport.png"), /*bAllowFailedTerminal=*/false);
    return true;
}

// Regression for B-editor-screenshot-no-completion-signal #4/#5: with a game/PIE
// viewport active, editor.screenshot must complete the job synchronously (capture
// via Viewport->ReadPixels, like ui.screenshot does on the same viewport) — NOT
// hang in "running" waiting on an OnScreenshotCaptured delegate that never fires
// under PIE-in-editor.
//
// Counterfactual: the reverted handler binds GameViewport->OnScreenshotCaptured()
// and returns without completing, so the ticket is still "running" right after
// StartJob — the strict "terminal completed immediately on return" assertions
// below fail. With the fix, the synchronous in-line capture means the ticket is
// already "completed" (or "failed" with a real error code) the moment the handler
// returns. The test is skipped when no game viewport is bound, so it never
// false-negatives in the normal headless/editor state.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorScreenshotGameViewportSyncTest,
    "PinWright.editor.screenshot.GameViewportCompletesSynchronously",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorScreenshotGameViewportSyncTest::RunTest(const FString& Parameters)
{
    // Only meaningful when a game/PIE viewport is actually bound — that is the
    // branch this regression covers. Skip otherwise (the LevelViewportFallback
    // test above covers the no-game-viewport path).
    if (!GEngine || !GEngine->GameViewport || !GEngine->GameViewport->Viewport)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-game-viewport"),
            TEXT("Skipped: no game/PIE viewport bound; PIE capture path not exercised."));
        return true;
    }

    // With the fix the game/PIE capture is synchronous, so the ticket is already
    // terminal the instant the handler returns; the reverted async-delegate
    // handler leaves it "running" (the hang the ticket reports). "failed" with a
    // concrete error code is an acceptable terminal status here — only "running"
    // is forbidden — so allow a failed terminal.
    RunScreenshotTerminalCheck(*this, TEXT("test_game_viewport.png"), /*bAllowFailedTerminal=*/true);
    return true;
}
