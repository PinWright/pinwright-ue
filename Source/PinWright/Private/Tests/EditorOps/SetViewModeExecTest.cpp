// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-set-view-mode-exec-failed.
//
// editor.set_view_mode {viewMode:"Wireframe"} must return success with the
// chosen mode echoed back. The original handler issued GEditor->Exec(nullptr,
// "viewmode Wireframe"), which returns false because `viewmode` is a
// per-viewport-client command that a nullptr-world exec never routes to the
// active level viewport — yielding EXEC_FAILED. The fix resolves the active
// level viewport client via FLevelEditorModule::GetFirstActiveViewport() and
// calls SetViewMode directly.
//
// The strict success assertion only fires when an active level viewport is
// resolvable (the path the fix exercises). Headless runs with no viewport are
// skipped so the test doesn't false-negative; reverting the fix to the naive
// nullptr-world exec fails the assertion whenever a viewport is present.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

#include "Editor.h"
#include "EditorViewportClient.h"
#include "IAssetViewport.h"
#include "LevelEditor.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSetViewModeExecTest,
    "PinWright.editor.set_view_mode.WireframeReachesViewport",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorSetViewModeExecTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Skipped: GEditor is null; no editor to resolve a level viewport against."));
        return true;
    }

    FLevelEditorModule& LevelEditorModule =
        FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
    TSharedPtr<IAssetViewport> ActiveViewport = LevelEditorModule.GetFirstActiveViewport();
    if (!ActiveViewport.IsValid())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-level-viewport"),
            TEXT("Skipped: no active level viewport; the fix's GetFirstActiveViewport path is unreachable in this run."));
        return true;
    }

    // The active level viewport's view mode is GLOBAL editor state, and this test drives it to
    // Wireframe. Leaving it there made every later suite test that reads the level viewport back
    // - effect.step_and_capture and the render.capture family - measure a wireframe frame, which
    // on a sparse level is indistinguishable from a black one. Restored on every exit path, the
    // same way the projection-slot and collision tests already restore theirs.
    FEditorViewportClient& ViewportClient = ActiveViewport->GetAssetViewportClient();
    const EViewModeIndex PreviousViewMode = ViewportClient.GetViewMode();
    ON_SCOPE_EXIT
    {
        ViewportClient.SetViewMode(PreviousViewMode);
        ViewportClient.Invalidate();
    };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("viewMode"), TEXT("Wireframe"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("editor.set_view_mode handler found"),
        InvokeHandlerWithCapture(TEXT("editor.set_view_mode"), Payload, Capture));

    // Counterfactual: reverting the handler to GEditor->Exec(nullptr, "viewmode Wireframe")
    // returns false and emits EXEC_FAILED, so bSuccess is false here.
    TestTrue(TEXT("set_view_mode reports success"), Capture.bSuccess);

    if (Capture.Result.IsValid())
    {
        FString ReportedMode;
        Capture.Result->TryGetStringField(TEXT("viewMode"), ReportedMode);
        TestEqual(TEXT("viewMode echoed back as Wireframe"), ReportedMode, FString(TEXT("Wireframe")));
    }
    return true;
}
