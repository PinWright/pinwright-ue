// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-set-game-view-keeps-editor-billboards.
//
// editor.set_game_view {enabled:true} must actually toggle Game View on the
// active level-editor viewport and report the *real* resulting state. The
// original handler ran GEditor->Exec(World, "ToggleGameView 1"), but
// "ToggleGameView" is NOT an engine exec/console command — it exists only as the
// G-key FUICommandInfo (LevelViewportActions.cpp) bound to
// SLevelViewport::ToggleGameView -> FEditorViewportClient::SetGameView. The exec
// matched no handler, returned false, and silently no-op'd while the handler
// still reported gameViewEnabled:true (a silent false-success). The fix resolves
// the active level viewport client (FLevelEditorModule::GetFirstActiveViewport()
// -> GetAssetViewportClient(), the pattern set_view_mode/set_viewport_realtime
// use) and calls SetGameView(bEnabled) directly, then reports the real
// IsInGameView().
//
// Verification never AddInfo-skips. When a live viewport is resolvable the test
// drives enable+disable and asserts the REAL IsInGameView() followed the request
// and the readback told the truth; when no viewport is present it asserts the
// fixed handler reports an honest failure instead of the old unconditional
// success. Reverting the handler to the phantom exec fails the enabled-state
// assertion (viewport present) or the no-phantom-success assertion (viewport
// absent), because the old body reported success:true+gameViewEnabled:true while
// IsInGameView() never changed.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"

#include "Editor.h"
#include "EditorViewportClient.h"
#include "Handlers/Editor/EditorHandlerUtils.h"
#include "UnrealClient.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSetGameViewTogglesTest,
    "PinWright.editor.set_game_view.TogglesRealViewportState",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorSetGameViewTogglesTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("editor.set_game_view handler registered"),
        IsHandlerRegistered(TEXT("editor.set_game_view")));

    // Resolve the active level-editor viewport client exactly as the handler does — the shared
    // typed resolver, which IS the LevelEditor-module walk the handler's own primary path takes.
    // We read IsInGameView() off THIS client to prove the toggle actually landed and that the
    // handler's readback reports the real state. Never GetActiveViewport()->GetClient(): under PIE
    // that client is the game client and the downcast reads past the end of its allocation (see
    // EditorHandlerUtils.h).
    FEditorViewportClient* ViewportClient =
        EditorHandlerUtils::ResolveActiveLevelViewportClient();

    // --- enabled:true ---
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetBoolField(TEXT("enabled"), true);
        FTestResponseCapture Capture;
        TestTrue(TEXT("editor.set_game_view handler found (enable)"),
            InvokeHandlerWithCapture(TEXT("editor.set_game_view"), Payload, Capture));

        if (ViewportClient)
        {
            // The toggle must land on the real client and the readback must equal
            // the real state. The old phantom exec left IsInGameView()==false while
            // reporting gameViewEnabled:true, so this fails on a revert.
            TestTrue(TEXT("enable: handler reports success"), Capture.bSuccess);
            TestTrue(TEXT("enable: real viewport is now in game view"),
                ViewportClient->IsInGameView());
            bool bReported = false;
            TestTrue(TEXT("enable: response carries gameViewEnabled"),
                Capture.Result.IsValid() &&
                Capture.Result->TryGetBoolField(TEXT("gameViewEnabled"), bReported));
            TestTrue(TEXT("enable: readback reports the true state"), bReported);
            TestEqual(TEXT("enable: readback equals real IsInGameView()"),
                bReported, ViewportClient->IsInGameView());
        }
        else
        {
            // No live viewport in this run: the fixed handler must report an honest
            // failure, not the old unconditional success:true+gameViewEnabled:true.
            TestFalse(TEXT("no-viewport: handler must not report phantom success"),
                Capture.bSuccess);
        }
    }

    // --- enabled:false --- (only observable with a live viewport; leaves Game
    // View off, the default state, so no other viewport test is perturbed)
    if (ViewportClient)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetBoolField(TEXT("enabled"), false);
        FTestResponseCapture Capture;
        TestTrue(TEXT("editor.set_game_view handler found (disable)"),
            InvokeHandlerWithCapture(TEXT("editor.set_game_view"), Payload, Capture));
        TestTrue(TEXT("disable: handler reports success"), Capture.bSuccess);
        TestFalse(TEXT("disable: real viewport left game view"),
            ViewportClient->IsInGameView());
        bool bReported = true;
        TestTrue(TEXT("disable: response carries gameViewEnabled"),
            Capture.Result.IsValid() &&
            Capture.Result->TryGetBoolField(TEXT("gameViewEnabled"), bReported));
        TestFalse(TEXT("disable: readback reports the false state"), bReported);
    }

    return true;
}
