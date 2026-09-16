// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression: drive.list_windows must expose each top-level window's maximized state
// (SWindow::IsWindowMaximized() — the same signal editor.resize_window's WINDOW_MAXIMIZED
// gate reads) so an agent reading the discovery verb can tell a window is maximized.
// Board B-list-windows-maximized-geometry: pre-fix each per-window record carries no
// maximized / window-state field at all, so list_windows presents a maximized window as a
// plain one and directly contradicts editor.resize_window's WINDOW_MAXIMIZED refusal.
//
// Differential: pre-fix every window record omits the `maximized` field, so the per-record
// presence assertion fails. Post-fix the handler emits it from IsWindowMaximized(), so the field
// is present and equals the window's real state (the value check is gated on the field existing,
// so it only runs — and proves behavior — on the fixed handler).
//
// No `minimized` coverage: list_windows enumerates via FSlateApplication::GetAllVisibleWindowsOrdered,
// which excludes minimized windows (IsVisible() && !IsWindowMinimized()), so a minimized window can
// never reach the handler's output and a per-record minimized flag could only ever read false — the
// field is deliberately not emitted, so there is nothing to assert here.
//
// A uniquely-titled in-code SWindow is driven into the maximized state to exercise the true
// case (anti-gaming: a hardcoded `maximized:false` fix fails the maximized-window value
// assertion). Native maximize of an OFFSCREEN window is best-effort under -RenderOffScreen
// -unattended; whether it flipped is recorded, but the field-presence + value-equality-
// against-live-ground-truth assertions fail pre-fix regardless of whether Maximize() took.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SWindow.h"

#include "Tests/TestUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveListWindowsReportsMaximizedTest,
    "PinWright.drive.editorint.ListWindowsReportsMaximizedState",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveListWindowsReportsMaximizedTest::RunTest(const FString& Parameters)
{
    if (!FSlateApplication::IsInitialized())
    {
        AddError(TEXT("Slate application not initialized; cannot exercise drive.list_windows."));
        return false;
    }
    FSlateApplication& SlateApp = FSlateApplication::Get();

    // Uniquely-titled so it is found unambiguously in the list_windows output and no other
    // open editor window collides with the title match.
    const FString Title = FString::Printf(TEXT("PW_ListWindowsMaximized_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    TSharedRef<SWindow> Window = SNew(SWindow)
        .Title(FText::FromString(Title))
        .ScreenPosition(FVector2D(120.0f, 120.0f))
        .ClientSize(FVector2D(480.0f, 320.0f))
        .FocusWhenFirstShown(false)
        .SupportsMaximize(true)
        .SupportsMinimize(true);

    SlateApp.AddWindow(Window, /*bShowImmediately=*/true);
    ON_SCOPE_EXIT { SlateApp.RequestDestroyWindow(Window); };

    // Realize the window, then drive it into the maximized state the discovery verb must report.
    for (int32 Tick = 0; Tick < 8; ++Tick)
    {
        SlateApp.Tick(ESlateTickType::All);
    }
    Window->Maximize();
    for (int32 Tick = 0; Tick < 8; ++Tick)
    {
        SlateApp.Tick(ESlateTickType::All);
    }
    const bool bMaximizeTook = Window->IsWindowMaximized();

    // Invoke the production discovery verb.
    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("drive.list_windows"), MakeShared<FJsonObject>(), Capture);
    TestTrue(TEXT("drive.list_windows handler is registered"), bFound);
    TestTrue(TEXT("drive.list_windows succeeds"), Capture.bSuccess);
    if (!bFound || !Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Windows = nullptr;
    if (!Capture.Result->TryGetArrayField(TEXT("windows"), Windows) || !Windows)
    {
        AddError(TEXT("drive.list_windows response carried no windows array."));
        return false;
    }

    // Ground truth in the exact order ListWindows enumerates (record.index i <-> LiveWindows[i]).
    TArray<TSharedRef<SWindow>> LiveWindows;
    SlateApp.GetAllVisibleWindowsOrdered(LiveWindows);

    // The defect: EVERY window record must carry a maximized bool an agent can read, and it
    // must equal the corresponding live window's IsWindowMaximized(). Pre-fix the field is
    // absent from all records, so the presence assertion fails.
    bool bFoundFixtureRecord = false;
    for (const TSharedPtr<FJsonValue>& Value : *Windows)
    {
        const TSharedPtr<FJsonObject>* EntryPtr = nullptr;
        if (!Value.IsValid() || !Value->TryGetObject(EntryPtr) || !EntryPtr || !(*EntryPtr).IsValid())
        {
            AddError(TEXT("a window entry is not a JSON object."));
            continue;
        }
        const TSharedPtr<FJsonObject>& Entry = *EntryPtr;

        FString EntryTitle;
        Entry->TryGetStringField(TEXT("title"), EntryTitle);

        bool bMaximizedField = false;
        const bool bHasField = Entry->TryGetBoolField(TEXT("maximized"), bMaximizedField);
        TestTrue(*FString::Printf(
            TEXT("window record '%s' carries a 'maximized' boolean field"), *EntryTitle),
            bHasField);

        // Behavior (only meaningful once the field exists): the reported state must equal the
        // live window's real IsWindowMaximized(), matched by the index the handler assigned.
        double EntryIndexD = -1.0;
        if (bHasField && Entry->TryGetNumberField(TEXT("index"), EntryIndexD))
        {
            const int32 EntryIndex = static_cast<int32>(EntryIndexD);
            if (LiveWindows.IsValidIndex(EntryIndex)
                && LiveWindows[EntryIndex]->GetTitle().ToString() == EntryTitle)
            {
                TestTrue(*FString::Printf(
                    TEXT("window record '%s' maximized matches SWindow::IsWindowMaximized()"), *EntryTitle),
                    bMaximizedField == LiveWindows[EntryIndex]->IsWindowMaximized());
            }
        }

        if (EntryTitle == Title)
        {
            bFoundFixtureRecord = true;
            // Anti-gaming true case: this fixture window is (best-effort) maximized, so its
            // record must report its real state. A hardcoded maximized:false fix fails this
            // whenever Maximize() took on the platform.
            if (bHasField)
            {
                TestTrue(TEXT("the maximized fixture window's record reports its real IsWindowMaximized()"),
                    bMaximizedField == Window->IsWindowMaximized());
            }
        }
    }

    TestTrue(TEXT("the fixture window appears in drive.list_windows output"), bFoundFixtureRecord);

    if (bMaximizeTook)
    {
        AddInfo(TEXT("Maximize() took effect; list_windows was checked against a genuinely maximized window."));
    }
    else
    {
        AddInfo(TEXT("Maximize() did not take on this offscreen window; the maximized-field presence + "
                     "value-equality assertions still gate the fix (they fail pre-fix regardless)."));
    }
    return true;
}
