// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression: board B-set-window-state-cannot-restore-minimized.
//
// THE DEFECT. editor.set_window_state {state:'restored'} advertises "un-maximize AND un-minimize",
// and a minimized window is the one state it exists to un-do — yet it could not target one. The
// shared window selector (FDriveEditorChrome::ResolveWindow -> ResolveSelectedWindow) built its
// entire candidate list from FSlateApplication::GetAllVisibleWindowsOrdered, which filters
// `IsVisible() && !IsWindowMinimized()` (SlateApplication.cpp:3789-3799 on UE 5.8, re-applied
// recursively to child windows at :3803). The deciding clause is the SECOND one, not the first:
// SWindow::IsVisible() (SWindow.cpp:1496) forwards to the native window's IsVisible(), which on
// Windows is the Show/Hide flag `bIsVisible` (WindowsWindow.cpp:915-918) and stays TRUE while the
// window is iconic. A minimized window is a perfectly valid, still-visible SWindow that the
// enumeration drops purely for being minimized. With the whole editor minimized the list came back
// empty and the resolver answered NO_WINDOWS, so SWindow::Restore() was structurally unreachable
// and the only recovery was Win32 ShowWindowAsync(hwnd, SW_RESTORE) from outside the RPC surface.
//
// TWO TESTS, AND WHAT EACH ONE PROVES.
//
//  1. MinimizedTolerantSelectorOrder — PURE. Exercises the ordering rule
//     (FDriveEditorChrome::AppendMinimizedTolerantWindowCandidates) over a CONSTRUCTED window set:
//     bare SNew(SWindow) widgets that are never added to FSlateApplication, never shown, and have
//     no native window. It therefore minimizes and restores nothing, which is deliberate — an
//     automation test that drives a real editor window's window-manager state can blind or wedge
//     the suite host. It proves the two properties the fix is built on: every index that existed
//     over the visible enumeration keeps its exact value (so no other verb's window_index changes
//     meaning), and an EMPTY visible list no longer yields an empty candidate list — which is
//     precisely the observed condition (whole editor minimized) that used to short-circuit into
//     NO_WINDOWS. It does NOT prove anything about live minimized windows; only the merge rule.
//
//  2. RestoresMinimizedWindow — LIVE, and narrow. It creates its OWN uniquely-titled fixture
//     window and only ever addresses that window by title, so it can never target the main editor
//     window; and it restores whatever it changed (the scope guard restores the fixture before
//     destroying it, so no iconic window can outlive the test even on a failing path).
//     Its always-on half asserts the response contract: `state` (the state MEASURED after the
//     call, not the one requested) and `stateMatchesRequest`. Both are absent pre-fix, so this
//     half FAILS against the pre-fix handler on any host.
//     Its minimized half is the real differential — pre-fix the verb answers WINDOW_NOT_FOUND
//     ("No visible window title contains ...") for a fixture that demonstrably exists — but native
//     minimize is best-effort under -RenderOffscreen -unattended. When Minimize() does not take,
//     that half CANNOT be asserted on this host and the test says so through the shared skip
//     emitter (PINWRIGHT_ASSERTIONS_SKIPPED) rather than passing quietly; check_suite_log then
//     refuses to classify the run COMPLETED_CLEAN.
//     Note the fixture is driven into the minimized state with SWindow::Minimize() DIRECTLY, not
//     through the verb: editor.set_window_state deliberately refuses state='minimized' while
//     GIsAutomationTesting is set, because a minimized window stops rendering and poisons every
//     render/profiling number a running suite takes afterwards.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "Handlers/Drive/DriveEditorChrome.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SWindow.h"

#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

// Named (not anonymous) helper namespace: Unity merges .cpp files into one TU, so a same-named
// helper in another test file's unnamed namespace would be a redefinition. The plugin's other
// test-helper clusters do the same.
namespace MinimizedToleranceTestLocal
{
    // Identity comparison over the shared refs the selector traffics in. Title is not usable here:
    // two windows may carry the same title, and the merge rule is defined on identity.
    bool IsSameWindowInstance(const TSharedRef<SWindow>& A, const TSharedRef<SWindow>& B)
    {
        return &A.Get() == &B.Get();
    }
}

using namespace MinimizedToleranceTestLocal;

// ============================================================================
// Pure: the minimized-tolerant merge rule, over a constructed window set
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveMinimizedTolerantSelectorOrderTest,
    "PinWright.drive.editorchrome.MinimizedTolerantSelectorOrder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveMinimizedTolerantSelectorOrderTest::RunTest(const FString& Parameters)
{
    // Never added to FSlateApplication, never shown: no native window is created and the suite
    // host's real windows are untouched.
    TSharedRef<SWindow> W0 = SNew(SWindow);
    TSharedRef<SWindow> W1 = SNew(SWindow);
    TSharedRef<SWindow> W2 = SNew(SWindow);
    TSharedRef<SWindow> W3 = SNew(SWindow);

    // The load-bearing case: some windows visible, some only reachable through the widened
    // enumeration, and one window present in BOTH inputs.
    {
        const TArray<TSharedRef<SWindow>> Visible = { W1, W3 };
        const TArray<TSharedRef<SWindow>> Extra = { W0, W1, W2 };

        TArray<TSharedRef<SWindow>> Candidates;
        FDriveEditorChrome::AppendMinimizedTolerantWindowCandidates(Visible, Extra, Candidates);

        TestEqual(TEXT("every visible window plus each not-already-present extra is a candidate"),
            Candidates.Num(), 4);
        if (Candidates.Num() == 4)
        {
            // Index stability. This is the property that lets the widening be safe for the other
            // editor-chrome verbs: window_index 0 and 1 still name exactly the windows they named
            // before minimized tolerance existed.
            TestTrue(TEXT("candidate 0 is still visible window 0 (window_index is unchanged)"),
                IsSameWindowInstance(Candidates[0], W1));
            TestTrue(TEXT("candidate 1 is still visible window 1 (window_index is unchanged)"),
                IsSameWindowInstance(Candidates[1], W3));
            // The newly reachable windows land strictly AFTER, in enumeration order.
            TestTrue(TEXT("the first extra window is appended after the visible ones"),
                IsSameWindowInstance(Candidates[2], W0));
            TestTrue(TEXT("the second unique extra window follows it in enumeration order"),
                IsSameWindowInstance(Candidates[3], W2));
        }

        // A window in both inputs occupies exactly one index, or the appended tail would shift
        // every index a caller had already been handed.
        int32 W1Occurrences = 0;
        for (const TSharedRef<SWindow>& Candidate : Candidates)
        {
            if (IsSameWindowInstance(Candidate, W1))
            {
                ++W1Occurrences;
            }
        }
        TestEqual(TEXT("a window present in both inputs is not duplicated"), W1Occurrences, 1);
    }

    // Nothing minimized: the candidate list must be the visible enumeration verbatim, so the
    // widening is inert on the overwhelmingly common case.
    {
        const TArray<TSharedRef<SWindow>> Visible = { W0, W1 };
        const TArray<TSharedRef<SWindow>> Extra;

        TArray<TSharedRef<SWindow>> Candidates;
        FDriveEditorChrome::AppendMinimizedTolerantWindowCandidates(Visible, Extra, Candidates);

        TestEqual(TEXT("with no extra windows the candidate list is the visible list"),
            Candidates.Num(), 2);
        if (Candidates.Num() == 2)
        {
            TestTrue(TEXT("visible window 0 keeps index 0"), IsSameWindowInstance(Candidates[0], W0));
            TestTrue(TEXT("visible window 1 keeps index 1"), IsSameWindowInstance(Candidates[1], W1));
        }
    }

    // THE OBSERVED CONDITION: the whole editor minimized, so GetAllVisibleWindowsOrdered returns
    // nothing at all. Pre-fix this is exactly where the resolver short-circuited into NO_WINDOWS
    // with a live window sitting right there.
    {
        const TArray<TSharedRef<SWindow>> Visible;
        const TArray<TSharedRef<SWindow>> Extra = { W0, W1 };

        TArray<TSharedRef<SWindow>> Candidates;
        FDriveEditorChrome::AppendMinimizedTolerantWindowCandidates(Visible, Extra, Candidates);

        TestEqual(TEXT("an empty visible enumeration still yields the minimized windows as candidates"),
            Candidates.Num(), 2);
        if (Candidates.Num() == 2)
        {
            TestTrue(TEXT("the first minimized window is selectable at index 0"),
                IsSameWindowInstance(Candidates[0], W0));
            TestTrue(TEXT("the second minimized window is selectable at index 1"),
                IsSameWindowInstance(Candidates[1], W1));
        }
    }

    return true;
}

// ============================================================================
// Live: the production verb, against its OWN fixture window only
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSetWindowStateRestoresMinimizedTest,
    "PinWright.editor.set_window_state.RestoresMinimizedWindow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorSetWindowStateRestoresMinimizedTest::RunTest(const FString& Parameters)
{
    if (!FSlateApplication::IsInitialized())
    {
        AddError(TEXT("Slate application not initialized; cannot exercise editor.set_window_state."));
        return false;
    }
    FSlateApplication& SlateApp = FSlateApplication::Get();

    // Uniquely titled so every call below targets EXACTLY this fixture. The verb is never invoked
    // without a selector in this test: a selector-less call resolves the active top-level window,
    // which on a developer host is the main editor frame.
    const FString Title = FString::Printf(TEXT("PW_SetWindowStateMinimized_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    TSharedRef<SWindow> Window = SNew(SWindow)
        .Title(FText::FromString(Title))
        .ScreenPosition(FVector2D(140.0f, 140.0f))
        .ClientSize(FVector2D(480.0f, 320.0f))
        .FocusWhenFirstShown(false)
        .SupportsMaximize(true)
        .SupportsMinimize(true);

    SlateApp.AddWindow(Window, /*bShowImmediately=*/true);
    ON_SCOPE_EXIT
    {
        // Restore whatever this test changed BEFORE tearing the fixture down, on every path
        // including a failing one, so no iconic window can outlive the test.
        if (Window->IsWindowMinimized())
        {
            Window->Restore();
            for (int32 Tick = 0; Tick < 8; ++Tick)
            {
                SlateApp.Tick(ESlateTickType::All);
            }
        }
        SlateApp.RequestDestroyWindow(Window);
    };

    for (int32 Tick = 0; Tick < 8; ++Tick)
    {
        SlateApp.Tick(ESlateTickType::All);
    }

    // ---- Always-on: the response must publish the MEASURED state, not just the requested one ----
    // Runs against the still-normal fixture, so it needs no window-manager cooperation of any kind
    // and asserts on every host. Pre-fix the handler emits neither field.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("window_title"), Title);
        Payload->SetStringField(TEXT("state"), TEXT("restored"));

        FTestResponseCapture Capture;
        const bool bFound =
            InvokeHandlerWithCapture(TEXT("editor.set_window_state"), Payload, Capture);
        TestTrue(TEXT("editor.set_window_state handler is registered"), bFound);
        TestTrue(TEXT("set_window_state{state:'restored'} succeeds on the fixture window"),
            Capture.bSuccess);

        if (bFound && Capture.bSuccess && Capture.Result.IsValid())
        {
            FString RequestedState;
            TestTrue(TEXT("response names the REQUESTED state separately"),
                Capture.Result->TryGetStringField(TEXT("requestedState"), RequestedState));
            TestEqual(TEXT("requestedState echoes what was asked for"),
                RequestedState, FString(TEXT("restored")));

            FString MeasuredState;
            const bool bHasMeasured =
                Capture.Result->TryGetStringField(TEXT("state"), MeasuredState);
            TestTrue(TEXT("response publishes the MEASURED window state read back after the call"),
                bHasMeasured);
            if (bHasMeasured)
            {
                TestEqual(TEXT("the measured state of a restored window is 'restored'"),
                    MeasuredState, FString(TEXT("restored")));
            }

            bool bMatches = false;
            const bool bHasMatch =
                Capture.Result->TryGetBoolField(TEXT("stateMatchesRequest"), bMatches);
            TestTrue(TEXT("response says whether the measured state matches the requested one"),
                bHasMatch);
            if (bHasMatch)
            {
                TestTrue(TEXT("measured and requested agree after a restore"), bMatches);
            }
        }
    }

    // ---- The defect itself: a MINIMIZED window must be resolvable by the same selector ----
    Window->Minimize();
    for (int32 Tick = 0; Tick < 8; ++Tick)
    {
        SlateApp.Tick(ESlateTickType::All);
    }
    const bool bMinimizeTook = Window->IsWindowMinimized();

    if (!bMinimizeTook)
    {
        // The host would not iconify an offscreen window, so the minimized-resolution assertions
        // measured nothing. Say so on the wire rather than returning a quiet green.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("window-minimize-did-not-take-on-host"),
            FString::Printf(
                TEXT("SWindow::Minimize() left '%s' non-minimized (best-effort for an offscreen "
                     "window under -RenderOffscreen -unattended), so the minimized-window "
                     "resolution assertions of editor.set_window_state could not run. The measured-"
                     "state response assertions above DID run."), *Title));
        return true;
    }

    // Pre-fix this call answers WINDOW_NOT_FOUND — "No visible window title contains '<Title>'" —
    // for a window that demonstrably exists and is sitting minimized on the same Slate application.
    TSharedPtr<FJsonObject> RestorePayload = MakeShared<FJsonObject>();
    RestorePayload->SetStringField(TEXT("window_title"), Title);
    RestorePayload->SetStringField(TEXT("state"), TEXT("restored"));

    FTestResponseCapture RestoreCapture;
    const bool bRestoreFound =
        InvokeHandlerWithCapture(TEXT("editor.set_window_state"), RestorePayload, RestoreCapture);
    TestTrue(TEXT("editor.set_window_state handler is registered"), bRestoreFound);
    TestTrue(*FString::Printf(
        TEXT("set_window_state resolves a MINIMIZED window rather than refusing it (code '%s')"),
        *RestoreCapture.ErrorCode), RestoreCapture.bSuccess);

    if (RestoreCapture.bSuccess && RestoreCapture.Result.IsValid())
    {
        bool bWasMinimized = false;
        TestTrue(TEXT("response reports the window WAS minimized before the call"),
            RestoreCapture.Result->TryGetBoolField(TEXT("wasMinimized"), bWasMinimized)
                && bWasMinimized);

        FString MeasuredState;
        if (RestoreCapture.Result->TryGetStringField(TEXT("state"), MeasuredState))
        {
            TestEqual(TEXT("the measured state after restoring a minimized window is 'restored'"),
                MeasuredState, FString(TEXT("restored")));
        }
    }

    // Ground truth, independent of the response shape: the window itself is no longer minimized.
    TestFalse(TEXT("the window is no longer minimized after the restore"),
        Window->IsWindowMinimized());

    return true;
}
