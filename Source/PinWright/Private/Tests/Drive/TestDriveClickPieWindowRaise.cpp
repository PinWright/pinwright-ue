// Copyright (c) 2026 Alexander Penkin. MIT License.

// drive.click on a target covered by a PIE window (issue #426). Multi-instance PIE opens one
// window per instance and the editor overlaps them by default, so a lobby button in client 1's
// window sat under client 2's and drive.click refused it with TARGET_OCCLUDED. Those windows are
// this editor's own, so the click now raises the target's window first, as a user clicking into it
// would; other occluders (a Message Log, a toast) are still refused (click_occlusion tests).
//
//  - CoveredTargetIsRaisedAndClicked (failure direction): PIE runs in the level viewport, whose
//    window is that PIE instance's game-viewport window. A regular fixture window holding a button
//    is placed over the PIE window, then the PIE window is activated over it, so Slate names the PIE
//    window at the button's center. drive.click must raise the fixture and click the button once.
//    Before the fix it answered TARGET_OCCLUDED naming the PIE window, and the button stayed
//    unclicked.
//  - OsInputCoveredTargetIsRaisedAndClicked (failure direction): the same with os_input:true, on a
//    visible editor on X11 (skipped otherwise, like every os_input test). The fixture first waits
//    for X to stack the PIE window's X window over the button; the click must then raise the
//    target, wait for its X window to be on top, and press it with real XTEST input. Before the fix
//    the own-window check refused it with TARGET_OCCLUDED.
//  - OsInputXCoveredSlateTopIsRaisedAndClicked (failure direction): as above, but the target is then
//    activated in Slate only, so Slate's order names the target while X still stacks the PIE window
//    over it (a declined raise, or a window manager restacking on its own). The press must still
//    reach the button once. Without the X check the press went to the PIE window: 0 clicks.
//  The Slate variant also waits, on a host where the PIE window has an X id, for X to stack it over
//  the button, since Slate's routing follows X there.
//
// On a visible GNOME desktop (mutter) the fixture needs four steps a headless run does not: move
// the fixture after it is mapped (mutter places new windows itself), focus the PIE window
// pager-style (mutter declines raises from an unfocused editor), wait until mutter reports it
// active as well as on top (an activation still in flight re-raised it over the target), and close
// the editor's toasts right before the click (each is transient for the level editor window, and
// the editor raises toasts on every mouse down, which brought that window back over the target).

#include "Misc/AutomationTest.h"

#include "Handlers/Drive/DriveEditorChrome.h"
#include "Handlers/Drive/DriveInput.h"
#include "Handlers/Drive/DriveOsInput.h"
#include "Handlers/Drive/DriveTypes.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/Guid.h"
#include "Tests/AutomationEditorCommon.h"
#include "Tests/Drive/HostNeutralPie.h"
#include "Tests/Drive/PieViewportClearance.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Utils/PieState.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace DriveClickPieWindowRaiseTest
{
    // How long no toast may be open before the click. A toast open during the click makes the
    // editor keep raising it, and the window manager raises its parent, the level editor window
    // (this PIE window), over the target with it (a verbose run showed the target and that window
    // trading places every few hundred ms). One opens a varying time after PIE start.
    constexpr double ToastQuietSeconds = 3.0;

    // The whole toast phase's budget.
    constexpr double ToastPhaseSeconds = 15.0;

    struct FState
    {
        FString TargetTitle;
        TSharedPtr<SWindow> TargetWindow;
        TSharedPtr<SWindow> PieWindow;
        TSharedRef<int32> Clicks = MakeShared<int32>(0);
        TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
        TSharedPtr<FTestResponseCapture> DismissCapture;
        double ToastQuietSince = 0.0;
        double ToastDeadline = 0.0;
        FPieViewportClearance Clearance;
        bool bOsInput = false;
        bool bSlateStale = false;
        FString Handle;
        FVector2D Center = FVector2D::ZeroVector;
        FVector2D FixturePosition = FVector2D::ZeroVector;
        bool bFixtureMoved = false;
        int32 Step = 0;
        double Deadline = 0.0;
        uint64 NotBeforeFrame = 0;
        double CleanupDeadline = 0.0;

        void Release()
        {
            if (TargetWindow.IsValid() && FSlateApplication::IsInitialized())
            {
                FSlateApplication::Get().RequestDestroyWindow(TargetWindow.ToSharedRef());
            }
            TargetWindow.Reset();
            PieWindow.Reset();
            Clearance.Restore();
        }
    };

    // The fixture button's drive handle in the target window; empty until it is painted.
    FString FindButtonHandle(const FState& State)
    {
        FDriveWindowSelector Selector;
        Selector.Title = State.TargetTitle;
        TArray<FDriveElement> Elements;
        FString WindowTitle;
        FString ErrorCode;
        FString ErrorMessage;
        if (FDriveEditorChrome::BuildElementList(Selector, Elements, WindowTitle, ErrorCode, ErrorMessage))
        {
            for (const FDriveElement& Element : Elements)
            {
                if (Element.Type == TEXT("SButton") && !Element.bGeometryStale
                    && !Element.Handle.Contains(TEXT("SWindowTitleBar")))
                {
                    return Element.Handle;
                }
            }
        }
        return FString();
    }

    // The fixture button's center in screen space, from its live geometry.
    bool ButtonCenter(const FState& State, const FString& Handle, FVector2D& OutCenter)
    {
        FDriveWindowSelector Selector;
        Selector.Title = State.TargetTitle;
        TArray<FDriveElement> Elements;
        FString WindowTitle;
        FString ErrorCode;
        FString ErrorMessage;
        FDriveEditorChrome::BuildElementList(Selector, Elements, WindowTitle, ErrorCode, ErrorMessage);
        for (const FDriveElement& Element : Elements)
        {
            if (Element.Handle == Handle)
            {
                OutCenter = Element.AbsolutePosition + Element.AbsoluteSize * 0.5;
                return true;
            }
        }
        return false;
    }

    // Once PIE has bound its game viewport: clears this editor's own windows off it, then opens the
    // regular (not top-most) target window at the PIE window's center. Returns false while waiting.
    bool Spawn(FAutomationTestBase& Test, FState& State, bool& bOutDone)
    {
        bOutDone = false;
        UGameViewportClient* Client = GEngine ? GEngine->GameViewport.Get() : nullptr;
        const TSharedPtr<SViewport> Viewport = Client ? Client->GetGameViewportWidget() : nullptr;
        State.PieWindow = Client ? Client->GetWindow() : nullptr;
        if (!GEditor || !GEditor->PlayWorld || !Viewport.IsValid() || !State.PieWindow.IsValid())
        {
            if (FPlatformTime::Seconds() >= State.Deadline)
            {
                PinWrightTestSkip::SkipAssertions(Test, TEXT("owned_pie_viewport_unavailable"),
                    TEXT("The owned PIE session did not bind a game viewport in a window."));
                bOutDone = true;
            }
            return false;
        }

        State.Clearance.Clear();

        // The PIE window's own center, not the viewport's: in a visible run on GNOME the viewport's
        // tick-space geometry still described a larger layout (center (1315, 750)) while the window
        // itself was 1072x640 at (70, 27) in both Slate and X, so the fixture sat outside it.
        const FVector2D Center = FVector2D(State.PieWindow->GetPositionInScreen()) + FVector2D(State.PieWindow->GetSizeInScreen()) * 0.5;
        const FVector2D Size(360.0, 200.0);
        State.TargetTitle = FString::Printf(TEXT("PW_ClickPieRaise_Target_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const TSharedRef<int32> Clicks = State.Clicks;
        TSharedRef<SWindow> Window = SNew(SWindow)
            .Title(FText::FromString(State.TargetTitle))
            .ScreenPosition(Center - Size * 0.5)
            .ClientSize(Size)
            .FocusWhenFirstShown(false)
            .SupportsMaximize(false)
            .SupportsMinimize(false);
        Window->SetContent(SNew(SButton)
            .OnClicked_Lambda([Clicks]() { ++(*Clicks); return FReply::Handled(); })
            [
                SNew(STextBlock).Text(FText::FromString(TEXT("PwPieRaiseTargetButton")))
            ]);
        FSlateApplication::Get().AddWindow(Window, /*bShowImmediately=*/true);
        State.TargetWindow = Window;
        State.FixturePosition = Center - Size * 0.5;
        State.NotBeforeFrame = GFrameCounter + 10;
        State.Deadline = FPlatformTime::Seconds() + 10.0;
        return true;
    }

    // Once the button is painted: stacks the PIE window over the target. Returns false while
    // waiting.
    bool CoverWithPie(FAutomationTestBase& Test, FState& State, bool& bOutDone)
    {
        bOutDone = false;
        // A window manager places a new window itself (mutter centered the fixture on the screen,
        // outside the PIE window), but honours a move of a mapped one, so move it once it is shown.
        if (!State.bFixtureMoved)
        {
            if (GFrameCounter < State.NotBeforeFrame)
            {
                return false;
            }
            State.TargetWindow->MoveWindowTo(State.FixturePosition);
            State.bFixtureMoved = true;
            State.NotBeforeFrame = GFrameCounter + 10;
            return false;
        }
        State.Handle = GFrameCounter >= State.NotBeforeFrame ? FindButtonHandle(State) : FString();
        if (State.Handle.IsEmpty())
        {
            if (GFrameCounter >= State.NotBeforeFrame && FPlatformTime::Seconds() >= State.Deadline)
            {
                PinWrightTestSkip::SkipAssertions(Test, TEXT("fixture-button-not-painted"),
                    TEXT("The fixture button was not listed with live geometry; this host did not paint the window."));
                bOutDone = true;
            }
            return false;
        }
        if (!Test.TestTrue(TEXT("fixture: the button's center resolves"), ButtonCenter(State, State.Handle, State.Center)))
        {
            bOutDone = true;
            return false;
        }
        const FSlateRect PieRect = State.PieWindow->GetRectInScreen();
        if (!PieRect.ContainsPoint(FVector2f(State.Center)))
        {
            if (FPlatformTime::Seconds() >= State.Deadline)
            {
                PinWrightTestSkip::SkipAssertions(Test, TEXT("fixture-not-over-pie-window"),
                    FString::Printf(TEXT("The fixture button's center (%.0f, %.0f) stayed outside the PIE window (%.0f, %.0f)-(%.0f, %.0f): the window manager did not honour the fixture's move."),
                        State.Center.X, State.Center.Y, PieRect.Left, PieRect.Top, PieRect.Right, PieRect.Bottom));
                bOutDone = true;
            }
            return false;
        }
        // Mutter declines a raise from an editor that is not the focused application (the visible
        // run on :0 skipped here while GNOME's desktop-icons window was active), so focus the PIE
        // window pager-style: the editor then is the focused application, as when a user drives PIE.
        FDriveOsInput::ActivateXWindow(FDriveOsInput::XWindowIdOf(State.PieWindow->GetNativeWindow()));
        State.PieWindow->BringToFront(/*bForce=*/true);
        FSlateApplication::Get().ProcessWindowActivatedEvent(
            FWindowActivateEvent(FWindowActivateEvent::EA_Activate, State.PieWindow.ToSharedRef()));
        State.Deadline = FPlatformTime::Seconds() + 3.0;
        return true;
    }

    // Checks the fixture and starts drive.click. The Slate variant runs in the frame that stacked
    // the PIE window, so no OS activation reorders the windows in between; os_input first waits
    // for X to stack the PIE window's X window over the button. Returns false while waiting.
    bool StartClick(FAutomationTestBase& Test, FState& State, bool& bOutDone)
    {
        bOutDone = false;
        // On a visible X host Slate routes by the platform's window-under-cursor, which follows X,
        // so the PIE window must cover the button in X too, or the Slate variant would pass with
        // the fix reverted.
        const uint64 PieXWindow = FDriveOsInput::XWindowIdOf(State.PieWindow->GetNativeWindow());
        const uint64 TargetXWindow = FDriveOsInput::XWindowIdOf(State.TargetWindow->GetNativeWindow());
        if (State.bOsInput && (PieXWindow == 0 || TargetXWindow == 0))
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("no-x11-window"),
                TEXT("The PIE or fixture window has no X11 window (Wayland or the dummy video driver)."));
            bOutDone = true;
            return false;
        }
        // Wait for the window manager to finish the PIE window's activation too, or that request,
        // still in flight, raises the PIE window back over the target right after the click's own
        // raise (seen on mutter: the fixture was restacked under it 20 ms after being focused).
        if (PieXWindow != 0 && (!FDriveOsInput::IsXWindowOnTopAt(State.Center, PieXWindow)
            || FDriveOsInput::XActiveWindow() != PieXWindow))
        {
            if (FPlatformTime::Seconds() >= State.Deadline)
            {
                PinWrightTestSkip::SkipAssertions(Test, TEXT("x-restack-not-observed"),
                    TEXT("The window manager never stacked the PIE window's X window over the fixture button, or never made it the active window."));
                bOutDone = true;
            }
            return false;
        }

        // A toast is a window transient for the level editor window (LinuxWindow.cpp gives it that
        // parent), and the editor raises every toast on each mouse down and focus change
        // (LinuxApplication.cpp, RaiseNotificationWindows). The window manager raises the toast's
        // parent with it, so the level editor window, this PIE window, came back over the target
        // during the press (measured: the button saw press then release 4 ms apart, no click). One
        // opens at PIE start, at a varying time after the fixture, so close toasts until none has
        // been open for ToastQuietSeconds, as an agent would.
        if (PieXWindow != 0)
        {
            // One fixed deadline for the whole toast phase, never extended: a Notification window
            // editor.dismiss_notifications does not own, or one that never closes, must end the
            // test (counted skip) instead of keeping it alive.
            const double Now = FPlatformTime::Seconds();
            if (State.ToastDeadline == 0.0)
            {
                State.ToastDeadline = Now + ToastPhaseSeconds;
            }
            if (Now > State.ToastDeadline)
            {
                TArray<TSharedRef<SWindow>> Open;
                FSlateApplication::Get().GetAllVisibleWindowsOrdered(Open);
                FString Toasts;
                for (const TSharedRef<SWindow>& Window : Open)
                {
                    if (Window->GetType() == EWindowType::Notification)
                    {
                        Toasts += FString::Printf(TEXT(" [%s at %s]"), *FVector2D(Window->GetSizeInScreen()).ToString(), *FVector2D(Window->GetPositionInScreen()).ToString());
                    }
                }
                PinWrightTestSkip::SkipAssertions(Test, TEXT("toast-not-dismissed"),
                    FString::Printf(TEXT("An editor toast kept reopening or never closed before the click:%s; last dismiss: %s %s"), *Toasts,
                        State.DismissCapture.IsValid() ? *State.DismissCapture->ErrorCode : TEXT("-"),
                        State.DismissCapture.IsValid() ? *State.DismissCapture->Message.Left(200) : TEXT("-")));
                bOutDone = true;
                return false;
            }
            if (State.DismissCapture.IsValid() && !State.DismissCapture->bWasCalled)
            {
                return false;
            }
            TArray<TSharedRef<SWindow>> Visible;
            FSlateApplication::Get().GetAllVisibleWindowsOrdered(Visible);
            const bool bToastOpen = Visible.ContainsByPredicate([](const TSharedRef<SWindow>& Window)
                { return Window->GetType() == EWindowType::Notification; });
            if (bToastOpen)
            {
                State.DismissCapture = MakeShared<FTestResponseCapture>();
                InvokeHandlerWithSharedCapture(TEXT("editor.dismiss_notifications"), MakeShared<FJsonObject>(), State.DismissCapture.ToSharedRef());
                State.ToastQuietSince = 0.0;
                // Closing a toast can move focus, so the X wait above gets a fresh window; the
                // toast phase as a whole stays bounded by ToastDeadline.
                State.Deadline = Now + 5.0;
                return false;
            }
            if (State.ToastQuietSince == 0.0)
            {
                State.ToastQuietSince = Now;
            }
            if (Now - State.ToastQuietSince < ToastQuietSeconds)
            {
                return false;
            }
        }

        if (State.bSlateStale)
        {
            // Slate names the target on top while X still stacks the PIE window over it.
            FSlateApplication::Get().ProcessWindowActivatedEvent(
                FWindowActivateEvent(FWindowActivateEvent::EA_Activate, State.TargetWindow.ToSharedRef()));
        }
        const TSharedPtr<SWindow> Expected = State.bSlateStale ? State.TargetWindow : State.PieWindow;
        const TSharedPtr<SWindow> Top = FDriveInput::TopWindowAtPoint(State.Center);
        if (Top.IsValid() && Top != State.PieWindow && Top != State.TargetWindow)
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("fixture-window-under-host-window"),
                FString::Printf(TEXT("Host window '%s' covers the fixture button."), *Top->GetTitle().ToString()));
            bOutDone = true;
            return false;
        }
        if (!Test.TestTrue(TEXT("fixture: Slate's window order names the expected window at the button"), Top == Expected)
            || (State.bSlateStale && !Test.TestFalse(TEXT("fixture: X does not route the button's center through the target"),
                FDriveOsInput::IsXWindowOnTopAt(State.Center, TargetXWindow))))
        {
            bOutDone = true;
            return false;
        }

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("handle"), State.Handle);
        Payload->SetStringField(TEXT("surface"), TEXT("editor_chrome"));
        Payload->SetStringField(TEXT("window_title"), State.TargetTitle);
        if (State.bOsInput)
        {
            Payload->SetBoolField(TEXT("os_input"), true);
        }
        Test.TestTrue(TEXT("drive.click handler found"),
            InvokeHandlerWithSharedCapture(TEXT("drive.click"), Payload, State.Capture));
        State.Deadline = FPlatformTime::Seconds() + 10.0;
        return true;
    }

    void Check(FAutomationTestBase& Test, const FState& State)
    {
        // A real press that landed in the PIE viewport instead (the fix reverted) can leave it
        // holding the pointer (PDS captures permanently, including the initial mouse down); say
        // so, since that grab is what the next os_input test then fails on.
        FDriveOsInput::FGrabState Grabs;
        if (State.bOsInput && FDriveOsInput::ProbeGrabs(Grabs) && Grabs.bPointerGrabbed)
        {
            Test.AddInfo(FString::Printf(TEXT("After the click an X pointer grab is held (by this editor: %s); a press into the PIE viewport takes one."),
                Grabs.bHeldByThisEditor.IsSet() ? (Grabs.bHeldByThisEditor.GetValue() ? TEXT("yes") : TEXT("no")) : TEXT("unknown")));
        }
        const FTestResponseCapture& Capture = *State.Capture;
        FString Occluder;
        if (!Capture.bSuccess && Capture.ErrorCode == TEXT("TARGET_OCCLUDED") && Capture.Result.IsValid()
            && Capture.Result->TryGetStringField(TEXT("occluding_window"), Occluder)
            && Occluder != State.PieWindow->GetTitle().ToString())
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("fixture-window-under-host-window"),
                FString::Printf(TEXT("A window the fixture does not own ('%s') covers the button: %s"), *Occluder, *Capture.Message));
            return;
        }
        Test.TestTrue(FString::Printf(TEXT("a click under a PIE window succeeds (error: %s %s)"), *Capture.ErrorCode, *Capture.Message),
            Capture.bSuccess);
        Test.TestEqual(TEXT("the raised target's button received exactly one click"), *State.Clicks, 1);
    }
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FDriveClickPieWindowRaiseRunCommand,
    FAutomationTestBase*, Test, TSharedRef<DriveClickPieWindowRaiseTest::FState>, State);

bool FDriveClickPieWindowRaiseRunCommand::Update()
{
    using namespace DriveClickPieWindowRaiseTest;
    FState& S = *State;
    const auto Finish = [&S]() { S.Release(); return true; };
    bool bDone = false;
    switch (S.Step)
    {
    case 0:
        if (!Spawn(*Test, S, bDone)) { return bDone ? Finish() : false; }
        S.Step = 1;
        return false;
    case 1:
        if (!CoverWithPie(*Test, S, bDone)) { return bDone ? Finish() : false; }
        S.Step = 2;
        // Offscreen (no X id) the Slate variant clicks in this frame, before any OS activation.
        return FDriveOsInput::XWindowIdOf(S.PieWindow->GetNativeWindow()) != 0 ? false : Update();
    case 2:
        if (!StartClick(*Test, S, bDone)) { return bDone ? Finish() : false; }
        S.Step = 3;
        return false;
    case 3:
        if (!S.Capture->bWasCalled)
        {
            if (FPlatformTime::Seconds() < S.Deadline) { return false; }
            Test->AddError(TEXT("drive.click never answered within 10 s."));
            return Finish();
        }
        Check(*Test, S);
        return Finish();
    default:
        return Finish();
    }
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FDriveClickPieWindowRaiseCleanupCommand,
    FAutomationTestBase*, Test, TSharedRef<DriveClickPieWindowRaiseTest::FState>, State);

bool FDriveClickPieWindowRaiseCleanupCommand::Update()
{
    if (!PinWrightPieState::IsPlayInEditorActive())
    {
        return true;
    }
    if (State->CleanupDeadline == 0.0)
    {
        State->CleanupDeadline = FPlatformTime::Seconds() + 15.0;
    }
    if (FPlatformTime::Seconds() >= State->CleanupDeadline)
    {
        Test->AddError(TEXT("Timed out waiting for the owned PIE session to stop."));
        return true;
    }
    return false;
}

namespace DriveClickPieWindowRaiseTest
{
    void Run(FAutomationTestBase& Test, bool bOsInput, bool bSlateStale = false)
    {
        if (!GEditor || !FSlateApplication::IsInitialized())
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("editor_or_slate_unavailable"),
                TEXT("Owned PIE requires GEditor and Slate."));
            return;
        }
        if (GEditor->PlayWorld || GEditor->IsPlaySessionInProgress())
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("preexisting_pie_session"),
                TEXT("The test never borrows or stops ambient PIE."));
            return;
        }
        if (!GEditor->GetActiveViewport())
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("level_viewport_unavailable"),
                TEXT("PIE in the level viewport requires an active level viewport."));
            return;
        }

        const TSharedRef<FState> State = MakeShared<FState>();
        State->bOsInput = bOsInput;
        State->bSlateStale = bSlateStale;
        State->Deadline = FPlatformTime::Seconds() + 20.0;
        ADD_LATENT_AUTOMATION_COMMAND(FStartHostNeutralPieCommand(&Test));
        ADD_LATENT_AUTOMATION_COMMAND(FDriveClickPieWindowRaiseRunCommand(&Test, State));
        ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
        ADD_LATENT_AUTOMATION_COMMAND(FDriveClickPieWindowRaiseCleanupCommand(&Test, State));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveClickPieWindowRaiseTest,
    "PinWright.drive.click_pie_window.CoveredTargetIsRaisedAndClicked",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveClickPieWindowRaiseTest::RunTest(const FString& Parameters)
{
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this)) { return true; }
    DriveClickPieWindowRaiseTest::Run(*this, /*bOsInput=*/false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveClickPieWindowRaiseOsInputTest,
    "PinWright.drive.click_pie_window.OsInputCoveredTargetIsRaisedAndClicked",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveClickPieWindowRaiseOsInputTest::RunTest(const FString& Parameters)
{
    FString Unavailable;
    if (!FDriveOsInput::IsAvailable(Unavailable))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-x-display"),
            FString::Printf(TEXT("os_input is unavailable on this host: %s"), *Unavailable));
        return true;
    }
    DriveClickPieWindowRaiseTest::Run(*this, /*bOsInput=*/true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveClickPieWindowRaiseOsInputSlateStaleTest,
    "PinWright.drive.click_pie_window.OsInputXCoveredSlateTopIsRaisedAndClicked",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveClickPieWindowRaiseOsInputSlateStaleTest::RunTest(const FString& Parameters)
{
    FString Unavailable;
    if (!FDriveOsInput::IsAvailable(Unavailable))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-x-display"),
            FString::Printf(TEXT("os_input is unavailable on this host: %s"), *Unavailable));
        return true;
    }
    DriveClickPieWindowRaiseTest::Run(*this, /*bOsInput=*/true, /*bSlateStale=*/true);
    return true;
}
