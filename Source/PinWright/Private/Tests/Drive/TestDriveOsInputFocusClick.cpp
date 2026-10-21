// Copyright (c) 2026 Alexander Penkin. MIT License.

// drive.click os_input into a window that is not (yet) the active one (issue #432). A real press
// into a window the window manager has not made active, or one the editor pumps in the same frame
// as that window's FocusIn or the pointer's enter, gave the button a press and a release but no
// click, while drive.click reported a benign settle outcome. drive.click now waits until the
// target is active enough, moves the pointer in, lets frames pump, and only then presses (or
// refuses, unpressed).
//
//  - JustActivatedWindowIsClicked (failure direction): two fixture windows, a decoy made active
//    with the real pointer parked on it, then in one frame the target's activation is requested
//    and drive.click os_input is started on the target's button. Exactly one click. Before the fix
//    the press went out at once, during the activation: 0 clicks while the response was a success.
//  - InactiveWindowIsActivatedAndClicked (failure direction): the same, but nothing but the click
//    itself activates the target, the agent's case of clicking into another window of this
//    editor. drive.click must activate it (the editor is the focused application) and click once;
//    without its activation the call fails INPUT_FAILED (not active).
//  - MenuEntryIsClicked (failure direction): an entry of a menu pushed with
//    FSlateApplication::PushMenu. Menus are override-redirect X windows that never become the
//    window manager's active window; their toplevel parent does. Exactly one click; comparing the
//    active window against the menu itself refused every menu click with INPUT_FAILED.
//
// Visible X11 editors only: os_input is unavailable offscreen (counted skip no-x-display), and
// without X window ids nothing can be measured (no-x11-window). The editor's toasts are closed
// first: each is transient for the level editor window, and the editor raises toasts on every
// mouse down, which can bring that window over the fixtures.

#include "Misc/AutomationTest.h"

#include "Handlers/Drive/DriveEditorChrome.h"
#include "Handlers/Drive/DriveOsInput.h"
#include "Handlers/Drive/DriveTypes.h"

#include "Dom/JsonObject.h"
#include "Framework/Application/IMenu.h"
#include "Framework/Application/MenuStack.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/Guid.h"
#include "Tests/AutomationCommon.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace DriveOsInputFocusClickTest
{
    enum class EMode { FixtureActivates, ProductActivates, MenuEntry };

    struct FState
    {
        EMode Mode = EMode::FixtureActivates;
        FString TargetTitle;
        TSharedPtr<SWindow> Target;
        TSharedPtr<SWindow> Decoy;
        TSharedPtr<IMenu> Menu;
        FVector2D TargetPosition = FVector2D::ZeroVector;
        FVector2D DecoyPosition = FVector2D::ZeroVector;
        TSharedRef<int32> Clicks = MakeShared<int32>(0);
        TSharedRef<FTestResponseCapture> Dismiss = MakeShared<FTestResponseCapture>();
        TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
        FString Handle;
        int32 WindowIndex = INDEX_NONE;
        FVector2D Center = FVector2D::ZeroVector;
        bool bWm = true;
        int32 Step = 0;
        uint64 NotBeforeFrame = 0;
        double Deadline = 0.0;

        ~FState()
        {
            if (!FSlateApplication::IsInitialized())
            {
                return;
            }
            if (Menu.IsValid())
            {
                Menu->Dismiss();
            }
            for (const TSharedPtr<SWindow>& Window : { Target, Decoy })
            {
                if (Window.IsValid())
                {
                    FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
                }
            }
        }
    };

    TSharedPtr<SWindow> MakeWindow(const FString& Title, const FVector2D& Position, const TSharedRef<SWidget>& Content)
    {
        TSharedRef<SWindow> Window = SNew(SWindow)
            .Title(FText::FromString(Title))
            .ScreenPosition(Position)
            .ClientSize(FVector2D(360.0f, 200.0f))
            .FocusWhenFirstShown(false)
            .SupportsMaximize(false)
            .SupportsMinimize(false);
        Window->SetContent(Content);
        FSlateApplication::Get().AddWindow(Window, /*bShowImmediately=*/true);
        return Window;
    }

    TSharedRef<SWidget> MakeButton(const TSharedRef<int32>& Clicks, const TCHAR* Label)
    {
        return SNew(SButton)
            .OnClicked_Lambda([Clicks]() { ++(*Clicks); return FReply::Handled(); })
            [
                SNew(STextBlock).Text(FText::FromString(Label))
            ];
    }

    // The clickable button in Window: its handle, center and drive.list_windows index.
    bool ResolveButton(FState& State, const TSharedPtr<SWindow>& Window)
    {
        for (const FDriveWindowInfo& Info : FDriveEditorChrome::ListWindows())
        {
            FDriveWindowSelector Selector;
            Selector.Index = Info.Index;
            TSharedPtr<SWindow> Resolved;
            FString Title;
            FString ErrorCode;
            FString ErrorMessage;
            if (!FDriveEditorChrome::ResolveWindow(Selector, Resolved, Title, ErrorCode, ErrorMessage) || Resolved != Window)
            {
                continue;
            }
            TArray<FDriveElement> Elements;
            FString WindowTitle;
            if (!FDriveEditorChrome::BuildElementList(Selector, Elements, WindowTitle, ErrorCode, ErrorMessage))
            {
                return false;
            }
            for (const FDriveElement& Element : Elements)
            {
                if (Element.Type == TEXT("SButton") && !Element.bGeometryStale && !Element.Handle.Contains(TEXT("SWindowTitleBar")))
                {
                    State.Handle = Element.Handle;
                    State.WindowIndex = Info.Index;
                    State.Center = Element.AbsolutePosition + Element.AbsoluteSize * 0.5;
                    return true;
                }
            }
        }
        return false;
    }

    uint64 XWindowOf(const TSharedPtr<SWindow>& Window)
    {
        return Window.IsValid() ? FDriveOsInput::XWindowIdOf(Window->GetNativeWindow()) : 0;
    }

    // Focus a fixture window: pager-style with a window manager, the X input focus without one.
    void Focus(const FState& State, uint64 XWindow)
    {
        if (State.bWm)
        {
            FDriveOsInput::ActivateXWindow(XWindow);
        }
        else
        {
            FDriveOsInput::FocusXWindow(XWindow);
        }
    }

    // The window whose button is clicked: the menu's window in MenuEntry mode, else the target.
    TSharedPtr<SWindow> ClickWindow(const FState& State)
    {
        return State.Mode == EMode::MenuEntry && State.Menu.IsValid() ? State.Menu->GetOwnedWindow() : State.Target;
    }

    // One step per frame; returns true when the test is over.
    bool Tick(FAutomationTestBase& Test, FState& State)
    {
        const bool bTimedOut = FPlatformTime::Seconds() > State.Deadline;
        const auto Skip = [&Test](const TCHAR* Reason, const TCHAR* Message)
        {
            PinWrightTestSkip::SkipAssertions(Test, Reason, Message);
            return true;
        };
        switch (State.Step)
        {
        case 0:
            InvokeHandlerWithSharedCapture(TEXT("editor.dismiss_notifications"), MakeShared<FJsonObject>(), State.Dismiss);
            State.Step = 1;
            return false;
        case 1:
        {
            if (!State.Dismiss->bWasCalled && !bTimedOut)
            {
                return false;
            }
            // Clear of every window this editor already shows, as the click_occlusion fixture.
            TArray<TSharedRef<SWindow>> Visible;
            FSlateApplication::Get().GetAllVisibleWindowsOrdered(Visible);
            double ClearX = 420.0;
            for (const TSharedRef<SWindow>& Other : Visible)
            {
                ClearX = FMath::Max(ClearX, FVector2D(Other->GetPositionInScreen()).X + FVector2D(Other->GetSizeInScreen()).X + 80.0);
            }
            // But on the screen: a private display (Xephyr 1920x1080) is narrower than the desktop,
            // and a window past its edge leaves the button out of the pointer's reach.
            FDisplayMetrics Metrics;
            FSlateApplication::Get().GetCachedDisplayMetrics(Metrics);
            ClearX = FMath::Min(ClearX, double(Metrics.PrimaryDisplayWidth) - 400.0);
            const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
            State.TargetTitle = FString::Printf(TEXT("PW_OsFocusClick_Target_%s"), *Guid);
            State.TargetPosition = FVector2D(ClearX, 200.0);
            State.DecoyPosition = FVector2D(ClearX, 520.0);
            State.Target = MakeWindow(State.TargetTitle, State.TargetPosition,
                State.Mode == EMode::MenuEntry
                    ? StaticCastSharedRef<SWidget>(SNew(SBorder)[SNew(STextBlock).Text(FText::FromString(TEXT("PwOsFocusMenuOwner")))])
                    : MakeButton(State.Clicks, TEXT("PwOsFocusClickTarget")));
            State.Decoy = MakeWindow(FString::Printf(TEXT("PW_OsFocusClick_Decoy_%s"), *Guid), State.DecoyPosition,
                SNew(STextBlock).Text(FText::FromString(TEXT("PwOsFocusClickDecoy"))));
            State.NotBeforeFrame = GFrameCounter + 10;
            State.Step = 2;
            return false;
        }
        case 2:
            if (GFrameCounter < State.NotBeforeFrame)
            {
                return false;
            }
            // A window manager places new windows itself (mutter centers them); a move of a mapped
            // window it honours.
            State.Target->MoveWindowTo(State.TargetPosition);
            State.Decoy->MoveWindowTo(State.DecoyPosition);
            State.NotBeforeFrame = GFrameCounter + 10;
            State.Step = 3;
            return false;
        case 3:
        {
            const uint64 TargetX = FDriveOsInput::XWindowIdOf(State.Target->GetNativeWindow());
            const uint64 DecoyX = FDriveOsInput::XWindowIdOf(State.Decoy->GetNativeWindow());
            if (TargetX == 0 || DecoyX == 0)
            {
                return Skip(TEXT("no-x11-window"), TEXT("The fixture windows have no X11 window (Wayland or the dummy video driver)."));
            }
            if (GFrameCounter < State.NotBeforeFrame)
            {
                return false;
            }
            // Park the real pointer on the decoy, so the click has to move into its target as an
            // agent's click into another window does (a pointer already resting on the button,
            // left there by an earlier run, hid the defect).
            FDriveInjectFailure MoveFailure;
            const FVector2D DecoyCenter = FVector2D(State.Decoy->GetPositionInScreen()) + FVector2D(State.Decoy->GetSizeInScreen()) * 0.5;
            if (!FDriveOsInput::MoveTo(DecoyCenter, MoveFailure))
            {
                Test.AddError(FString::Printf(TEXT("fixture: could not park the pointer on the decoy: %s %s"), *MoveFailure.Code, *MoveFailure.Message));
                return true;
            }
            // The menu belongs to the target window, so in MenuEntry mode that window is made
            // active (as when an agent opened the menu); otherwise the decoy is. Without a window
            // manager (a private Xvfb / Xephyr) there is nothing to ask: set the X input focus.
            FDriveOsInput::XActiveWindow(&State.bWm);
            Focus(State, State.Mode == EMode::MenuEntry ? TargetX : DecoyX);
            State.Deadline = FPlatformTime::Seconds() + 10.0;
            State.Step = 4;
            return false;
        }
        case 4:
        {
            const uint64 TargetX = FDriveOsInput::XWindowIdOf(State.Target->GetNativeWindow());
            const TSharedPtr<SWindow> Focused = State.Mode == EMode::MenuEntry ? State.Target : State.Decoy;
            if (State.bWm ? FDriveOsInput::XActiveWindow() != XWindowOf(Focused) : !FDriveOsInput::HasSdlKeyboardFocus(Focused->GetNativeWindow()))
            {
                if (!State.bWm)
                {
                    // Without a window manager SDL focuses each window as it shows, possibly after
                    // this fixture's own request; repeat it until SDL reports the focus.
                    FDriveOsInput::FocusXWindow(XWindowOf(Focused));
                }
                return bTimedOut && Skip(TEXT("x-activation-not-observed"), TEXT("The window manager never made the fixture window active."));
            }
            if (State.Mode == EMode::MenuEntry && !State.Menu.IsValid())
            {
                const TSharedRef<SWidget> Owner = State.Target->GetContent();
                FWidgetPath OwnerPath;
                FSlateApplication::Get().FindPathToWidget(Owner, OwnerPath);
                State.Menu = FSlateApplication::Get().PushMenu(Owner, OwnerPath, MakeButton(State.Clicks, TEXT("PwOsFocusMenuEntry")),
                    FVector2D(State.Target->GetPositionInScreen()) + FVector2D(60.0, 60.0),
                    FPopupTransitionEffect(FPopupTransitionEffect::None), /*bFocusImmediately=*/false, FVector2f::ZeroVector,
                    TOptional<EPopupMethod>(EPopupMethod::CreateNewWindow));
                if (!Test.TestTrue(TEXT("fixture: PushMenu opened a menu in its own window"),
                        State.Menu.IsValid() && State.Menu->GetOwnedWindow().IsValid()))
                {
                    return true;
                }
                State.NotBeforeFrame = GFrameCounter + 10;
                return false;
            }
            const TSharedPtr<SWindow> Window = ClickWindow(State);
            if (GFrameCounter < State.NotBeforeFrame || !ResolveButton(State, Window))
            {
                return bTimedOut && Skip(TEXT("fixture-button-not-painted"), TEXT("The fixture button was never listed with live geometry."));
            }
            const uint64 ClickX = FDriveOsInput::XWindowIdOf(Window->GetNativeWindow());
            if (!FDriveOsInput::IsXWindowOnTopAt(State.Center, ClickX))
            {
                return bTimedOut && Skip(TEXT("x-fixture-not-on-top"), TEXT("X never showed the fixture's button window on top at the button."));
            }
            if (State.Mode == EMode::FixtureActivates)
            {
                // In one frame: ask for the target's activation and start the click, so the press
                // races the focus change.
                Focus(State, TargetX);
            }
            TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
            Payload->SetStringField(TEXT("handle"), State.Handle);
            Payload->SetStringField(TEXT("surface"), TEXT("editor_chrome"));
            Payload->SetNumberField(TEXT("window_index"), State.WindowIndex);
            Payload->SetBoolField(TEXT("os_input"), true);
            Test.TestTrue(TEXT("drive.click handler found"),
                InvokeHandlerWithSharedCapture(TEXT("drive.click"), Payload, State.Capture));
            State.Deadline = FPlatformTime::Seconds() + 10.0;
            State.Step = 5;
            return false;
        }
        default:
            if (!State.Capture->bWasCalled)
            {
                if (bTimedOut)
                {
                    Test.AddError(TEXT("drive.click never answered within 10 s."));
                    return true;
                }
                return false;
            }
            Test.TestTrue(FString::Printf(TEXT("the os_input click succeeds (error: %s %s)"), *State.Capture->ErrorCode, *State.Capture->Message),
                State.Capture->bSuccess);
            Test.TestEqual(TEXT("the button received exactly one click"), *State.Clicks, 1);
            return true;
        }
    }

    void Run(FAutomationTestBase& Test, EMode Mode);
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FDriveOsInputFocusClickCommand,
    FAutomationTestBase*, Test, TSharedRef<DriveOsInputFocusClickTest::FState>, State);

bool FDriveOsInputFocusClickCommand::Update()
{
    return DriveOsInputFocusClickTest::Tick(*Test, *State);
}

void DriveOsInputFocusClickTest::Run(FAutomationTestBase& Test, EMode Mode)
{
    FString Unavailable;
    if (!FDriveOsInput::IsAvailable(Unavailable))
    {
        PinWrightTestSkip::SkipAssertions(Test, TEXT("no-x-display"),
            FString::Printf(TEXT("os_input is unavailable on this host: %s"), *Unavailable));
        return;
    }
    if (!FSlateApplication::IsInitialized())
    {
        PinWrightTestSkip::SkipAssertions(Test, TEXT("slate_not_initialized"), TEXT("FSlateApplication is not initialized."));
        return;
    }
    const TSharedRef<FState> State = MakeShared<FState>();
    State->Mode = Mode;
    State->Deadline = FPlatformTime::Seconds() + 15.0;
    ADD_LATENT_AUTOMATION_COMMAND(FDriveOsInputFocusClickCommand(&Test, State));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsInputFocusClickTest,
    "PinWright.drive.os_input_focus.JustActivatedWindowIsClicked",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsInputFocusClickTest::RunTest(const FString& Parameters)
{
    DriveOsInputFocusClickTest::Run(*this, DriveOsInputFocusClickTest::EMode::FixtureActivates);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsInputFocusProductActivatesTest,
    "PinWright.drive.os_input_focus.InactiveWindowIsActivatedAndClicked",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsInputFocusProductActivatesTest::RunTest(const FString& Parameters)
{
    DriveOsInputFocusClickTest::Run(*this, DriveOsInputFocusClickTest::EMode::ProductActivates);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveOsInputFocusMenuEntryTest,
    "PinWright.drive.os_input_focus.MenuEntryIsClicked",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveOsInputFocusMenuEntryTest::RunTest(const FString& Parameters)
{
    DriveOsInputFocusClickTest::Run(*this, DriveOsInputFocusClickTest::EMode::MenuEntry);
    return true;
}
