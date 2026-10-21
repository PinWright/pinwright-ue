// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Drive/DriveActionCommon.h"

#include "Handlers/Drive/DriveConditionEval.h"
#include "Handlers/Drive/DriveFingerprint.h"
#include "Handlers/Drive/DriveHandlerCommon.h"
#include "Handlers/Drive/DriveInput.h"
#include "Handlers/Drive/DriveJson.h"
#include "Handlers/Drive/DriveLiveResolver.h"
#include "Handlers/Drive/DriveOsInput.h"
#include "Handlers/Drive/DriveSettleDriver.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"
#include "Widgets/SWindow.h"

// File-local helpers live in a uniquely-named namespace (not anonymous): the
// plugin's Unity build merges translation units, and a distinct namespace keeps
// these from colliding with same-named helpers in sibling drive units.
namespace DriveActionCommonLocal
{
    // Read mark_cap / journal_since / include_journal consistently with drive.observe.
    int32 ReadMarkCap(const FHandlerContext& Ctx) { return Ctx.GetInt(TEXT("mark_cap"), 50); }
    bool ReadIncludeJournal(const FHandlerContext& Ctx) { return Ctx.GetBool(TEXT("include_journal"), false); }
    uint64 ReadJournalSince(const FHandlerContext& Ctx)
    {
        const int32 Raw = Ctx.GetInt(TEXT("journal_since"), 0);
        return Raw > 0 ? static_cast<uint64>(Raw) : 0;
    }

    // How long a pointer action waits for Slate to route the target's center to the target's own
    // window before refusing. On Linux the platform's window-under-cursor follows a warp only once
    // SDL's enter/leave event is pumped, one or two frames later; a window still owning the point
    // after this is really stacked over the target.
    constexpr double PointerRouteWaitSeconds = 0.3;

    // True when Window shows a PIE instance's game viewport: a client's own window in a
    // multi-instance PIE, or the level editor window for PIE in the viewport (so during
    // in-viewport PIE a floating editor window under the level editor window is raised over it
    // too, which is what a user clicking into that window does).
    bool IsPieViewportWindow(const TSharedPtr<SWindow>& Window)
    {
        if (!Window.IsValid() || !GEngine)
        {
            return false;
        }
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            if (Context.WorldType == EWorldType::PIE && Context.GameViewport && Context.GameViewport->GetWindow() == Window)
            {
                return true;
            }
        }
        return false;
    }

    // Multi-instance PIE opens one window per instance, overlapping by default (the editor centers
    // the first and puts each next one right of the last, clamped to the work area: PlayLevel.cpp
    // UEditorEngine::GetWindowSizeAndPositionForInstanceIndex), so a target in one PIE window is
    // often under another. The Slate path: when Occluder is a PIE window, raise the target's window
    // over it, as a user clicking into it does, and return true; any other occluder (a Message Log,
    // a toast) is left in place for the caller to refuse. The native raise does not activate
    // (RaiseXWindow; no X id: SWindow::BringToFront). The target is also moved to the front of
    // Slate's window order at once: with no platform window under the cursor (offscreen) Slate
    // routes by that order, which otherwise follows only an OS activation. Its side effects are a
    // user's click into the window: the previous PIE client loses focus
    // (UGameViewportClient::LostFocus: audio focus dropped, pressed keys flushed if configured),
    // pointer capture is released and open menus close. The route wait still gates injection.
    bool RaiseOverPieWindow(const TSharedPtr<SWindow>& Target, const TSharedPtr<SWindow>& Occluder)
    {
        if (!Occluder.IsValid() || Occluder == Target || !IsPieViewportWindow(Occluder))
        {
            return false;
        }
        const uint64 TargetXWindow = FDriveOsInput::XWindowIdOf(Target->GetNativeWindow());
        if (!FDriveOsInput::RaiseXWindow(TargetXWindow))
        {
            Target->BringToFront(/*bForce=*/false);
        }
        FSlateApplication::Get().ProcessWindowActivatedEvent(
            FWindowActivateEvent(FWindowActivateEvent::EA_Activate, Target.ToSharedRef()));
        return true;
    }

    // Whether XWindow belongs to one of this editor's visible windows.
    bool IsOwnXWindow(uint64 XWindow)
    {
        TArray<TSharedRef<SWindow>> Windows;
        FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
        return XWindow != 0 && Windows.ContainsByPredicate([XWindow](const TSharedRef<SWindow>& Window)
            { return FDriveOsInput::XWindowIdOf(Window->GetNativeWindow()) == XWindow; });
    }

    // This editor's visible window (top-level or child) whose X window is on the X route at a
    // point, top-most first; null when none is (the X window there is another process's).
    TSharedPtr<SWindow> OwnWindowOnXPath(const TArray<uint64>& XPath)
    {
        TArray<TSharedRef<SWindow>> Windows;
        FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
        for (int32 Index = Windows.Num() - 1; Index >= 0; --Index)
        {
            const uint64 XWindow = FDriveOsInput::XWindowIdOf(Windows[Index]->GetNativeWindow());
            if (XWindow != 0 && XPath.Contains(XWindow))
            {
                return Windows[Index];
            }
        }
        return nullptr;
    }

    // How long a pointer action waits after raising the target over a PIE window, for X to put
    // the target's X window on top at the point (os_input) or for Slate's routing to follow
    // (Slate path): the window manager restacks asynchronously, through the compositor.
    constexpr double RaiseWaitSeconds = 1.0;

    // Frames os_input lets pass between the target becoming active (X and SDL agree) and the press.
    constexpr uint64 FocusSettleFrames = 3;

    // Ticks the os_input gate always waits before refusing, so a single long frame right after the
    // call (a PIE start or shader-compile hitch) does not end the wait before anything was pumped.
    constexpr uint64 MinGateTicks = FocusSettleFrames + 2;

    // How often os_input repeats the raise while waiting for X to show the raised target on top.
    constexpr double ReRaiseSeconds = 0.25;

    // bRaiseAttempted: the target's window was raised over Under (a PIE window), but the window
    // manager did not stack it on top within WaitSeconds. Mutter declines an XRaiseWindow while the
    // editor is not the focused application and honours it once the editor is; a pager-style
    // _NET_ACTIVE_WINDOW (source 2, what xdotool windowactivate sends) focuses it from outside
    // (measured on GNOME/mutter, 2026-10-20). No PinWright verb focuses a window, so the recovery
    // names that external step with the target's X window id when there is one.
    void SendOccluded(const FAsyncResponseToken& Token, const FString& Handle, const FVector2D& Point,
        const TSharedPtr<SWindow>& Under, bool bRaiseAttempted = false, double WaitSeconds = 0.0,
        uint64 TargetXWindow = 0)
    {
        TArray<TSharedRef<SWindow>> Visible;
        if (FSlateApplication::IsInitialized())
        {
            FSlateApplication::Get().GetAllVisibleWindowsOrdered(Visible);
        }
        const bool bToastOpen = Visible.ContainsByPredicate([](const TSharedRef<SWindow>& Window)
            { return Window->GetType() == EWindowType::Notification; });
        const FString ToastStep = bToastOpen ? FString(TEXT("close the editor's toasts (editor.dismiss_notifications), then ")) : FString();
        const FString FocusStep = TargetXWindow != 0
            ? FString::Printf(TEXT("focus the editor once (click it, or run 'xdotool windowactivate 0x%llx' on its X display; no PinWright verb focuses a window), or move the PIE windows apart, then retry"), TargetXWindow)
            : FString(TEXT("focus the editor once (click it; no PinWright verb focuses a window), or move the PIE windows apart, then retry"));
        const FString Title = Under.IsValid() ? Under->GetTitle().ToString() : FString();
        const FString Type = Under.IsValid() ? FDriveEditorChrome::WindowTypeToString(Under->GetType()) : FString();
        // A notification toast is untitled, and the notification manager re-places and re-sizes it
        // every frame, so editor.resize_window cannot clear it; only dismissing it can.
        const bool bNotification = Under.IsValid() && Under->GetType() == EWindowType::Notification;
        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
        Details->SetStringField(TEXT("handle"), Handle);
        Details->SetNumberField(TEXT("x"), Point.X);
        Details->SetNumberField(TEXT("y"), Point.Y);
        if (Under.IsValid())
        {
            Details->SetStringField(TEXT("occluding_window"), Title);
            Details->SetStringField(TEXT("occluding_window_type"), Type);
            Details->SetStringField(TEXT("recovery"), bNotification
                ? TEXT("editor.dismiss_notifications {}")
                : bRaiseAttempted
                ? *(ToastStep + FocusStep)
                : TEXT("editor.resize_window (shrink it) or close it, then retry"));
        }
        if (bRaiseAttempted)
        {
            Details->SetBoolField(TEXT("raise_attempted"), true);
            if (TargetXWindow != 0)
            {
                Details->SetNumberField(TEXT("target_window_id"), static_cast<double>(TargetXWindow));
            }
        }
        Token.SendError(ErrorCodes::ERR_TARGET_OCCLUDED,
            bRaiseAttempted
                ? FString::Printf(
                    TEXT("Element '%s' is covered at (%.0f, %.0f) by window '%s'. The target's window was raised but the window manager did not stack it on top within %.1f s (it refuses raises while the editor is not the focused application%s). Nothing was injected; %s%s."),
                    *Handle, Point.X, Point.Y, *Title, WaitSeconds,
                    bToastOpen ? TEXT(", and the open editor toast pulls the level editor window back up whenever the editor raises it") : TEXT(""),
                    *ToastStep, *FocusStep)
            : bNotification
                ? FString::Printf(
                    TEXT("Element '%s' is covered at (%.0f, %.0f) by an editor notification toast (window '%s', type Notification), which would receive the input instead. Dismiss it with editor.dismiss_notifications {}, then retry."),
                    *Handle, Point.X, Point.Y, *Title)
            : Under.IsValid()
                ? FString::Printf(
                    TEXT("Element '%s' is covered at (%.0f, %.0f) by window '%s' (type %s), which would receive the input instead. Shrink it with editor.resize_window or close it (drive.list_windows lists it), then retry."),
                    *Handle, Point.X, Point.Y, *Title, *Type)
                : FString::Printf(
                    TEXT("No window accepts pointer input at element '%s''s center (%.0f, %.0f), so the input would land nowhere."),
                    *Handle, Point.X, Point.Y),
            Details);
    }

    // os_input's TARGET_OCCLUDED: the same contract as SendOccluded, for an X window of
    // another process, which Slate cannot see and which would take the real input.
    void SendOsOccluded(const FAsyncResponseToken& Token, const FString& Handle, const FVector2D& Point,
        const FDriveOsInput::FForeignWindow& Foreign)
    {
        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
        Details->SetStringField(TEXT("handle"), Handle);
        Details->SetNumberField(TEXT("x"), Point.X);
        Details->SetNumberField(TEXT("y"), Point.Y);
        Details->SetStringField(TEXT("occluding_window"), Foreign.Title);
        Details->SetNumberField(TEXT("occluding_window_id"), static_cast<double>(Foreign.WindowId));
        Details->SetNumberField(TEXT("occluding_pid"), Foreign.Pid);
        Token.SendError(ErrorCodes::ERR_TARGET_OCCLUDED,
            FString::Printf(
                TEXT("Element '%s' is covered at (%.0f, %.0f) by OS window 0x%llx '%s' (pid %u), which is not this editor's and would receive the real input. Nothing was injected. Raise this editor's window or move that one, then retry."),
                *Handle, Point.X, Point.Y, Foreign.WindowId, *Foreign.Title, Foreign.Pid),
            Details);
    }

    // The window whose activation a press into Window depends on: the nearest ancestor that is
    // neither a menu nor a window that never activates. Editor menus and dropdowns are popup
    // SWindows (MenuStack.cpp, Type Menu) that SDL maps override-redirect, so they are never the
    // window manager's active window; their toplevel parent is. A toast (ActivationPolicy Never)
    // is a native child of the level editor window (NotificationManager.cpp, AddWindowAsNativeChild)
    // and likewise never active; a parentless one stays itself, and a press into it is refused.
    TSharedPtr<SWindow> ActivationWindowOf(const TSharedPtr<SWindow>& Window)
    {
        TSharedPtr<SWindow> Current = Window;
        while (Current.IsValid() && Current->GetParentWindow().IsValid()
            && (Current->GetType() == EWindowType::Menu || Current->ActivationPolicy() == EWindowActivationPolicy::Never))
        {
            Current = Current->GetParentWindow();
        }
        return Current;
    }

    // os_input's last gate before a real action (#426, #432). X routes the input by stacking, but
    // a press into a window that is not the active one makes the window manager focus it under
    // the press, and a press the editor pumps in the same frame as the FocusIn, or as the pointer's
    // enter, also gave the button a press and a release but no click (measured on mutter). Either
    // way the call used to report a benign settle outcome. So, on the core ticker:
    //  1. wait (RaiseWaitSeconds, and at least MinGateTicks ticks so one long hitch does not
    //     refuse before the editor pumped anything) until the target's X window is on top at the
    //     point and, for a press (bRequireActive; not for a hover), active enough: SDL holds
    //     keyboard focus on the target or its activation window, and the window manager, when one
    //     reports an active window, names the activation window (FDriveOsInput::IsActiveEnough);
    //  2. move the real pointer onto the target and wait (RaiseWaitSeconds more) until the
    //     editor's window-under-cursor names the target and FocusSettleFrames frames have pumped;
    //  3. inject.
    // Refusals, none of them after anything was pressed: TARGET_CHANGED when the target goes away
    // (a stopped PIE destroys the widget but can leave its window, the level editor's, open),
    // TARGET_OCCLUDED naming what X has on top, INPUT_FAILED when the target never becomes active
    // or the pointer never registers inside it.
    void PressWhenActive(const TSharedRef<FAsyncResponseToken>& Token, const FString& Handle,
        const TSharedPtr<SWindow>& TargetWindow, const TSharedPtr<SWidget>& TargetWidget, uint64 TargetXWindow,
        const FVector2D& TargetCenter, bool bRaiseAttempted, bool bRequireActive,
        const FDriveActionCommon::FInjectOnTarget& Inject,
        const TFunction<void(const TSharedRef<FAsyncResponseToken>&)>& StartSettle, const TCHAR* InjectFailedMessage)
    {
        const double Deadline = FPlatformTime::Seconds() + RaiseWaitSeconds;
        const uint64 MinFrame = GFrameCounter + MinGateTicks;
        const TWeakPtr<SWindow> WeakTargetWindow = TargetWindow;
        const TWeakPtr<SWindow> WeakActivationWindow = ActivationWindowOf(TargetWindow);
        const TWeakPtr<SWidget> WeakTargetWidget = TargetWidget;
        const TSharedRef<uint64> ReadyFrame = MakeShared<uint64>(0);
        const TSharedRef<double> EnterDeadline = MakeShared<double>(0.0);
        const TSharedRef<double> NextRaise = MakeShared<double>(FPlatformTime::Seconds() + ReRaiseSeconds);
        FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
            [Token, WeakTargetWindow, WeakActivationWindow, WeakTargetWidget, TargetXWindow, TargetCenter, bRaiseAttempted, bRequireActive, Deadline, MinFrame, ReadyFrame, EnterDeadline, NextRaise, Inject, StartSettle, Handle, InjectFailedMessage](float) -> bool
            {
                const TSharedPtr<SWindow> Target = WeakTargetWindow.Pin();
                const TSharedPtr<SWindow> ActivationWindow = WeakActivationWindow.Pin();
                if (!Target.IsValid() || !ActivationWindow.IsValid() || !WeakTargetWidget.IsValid())
                {
                    Token->SendError(ErrorCodes::ERR_TARGET_CHANGED,
                        FString::Printf(TEXT("Element '%s' or its window went away before the input could be delivered."), *Handle));
                    return false;
                }
                const bool bGateExpired = FPlatformTime::Seconds() >= Deadline && GFrameCounter >= MinFrame;
                const TArray<uint64> XPath = FDriveOsInput::XWindowPathAt(TargetCenter);
                if (!XPath.Contains(TargetXWindow))
                {
                    if (!bGateExpired)
                    {
                        // The editor itself can restack the covering window after the raise (it
                        // raises its toasts on every focus change, and the window manager raises a
                        // toast's parent, the level editor window, with it), so repeat the plain
                        // raise while waiting.
                        if (bRaiseAttempted && FPlatformTime::Seconds() >= *NextRaise)
                        {
                            // Another process's window over the point is not ours to fight.
                            FDriveOsInput::FForeignWindow Foreign;
                            if (FDriveOsInput::FindForeignWindowAt(TargetCenter, Foreign))
                            {
                                SendOsOccluded(*Token, Handle, TargetCenter, Foreign);
                                return false;
                            }
                            FDriveOsInput::RaiseXWindow(TargetXWindow);
                            *NextRaise = FPlatformTime::Seconds() + ReRaiseSeconds;
                        }
                        return true;
                    }
                    // Name whatever is on top now: another process's window may have come up.
                    FDriveOsInput::FForeignWindow Foreign;
                    if (FDriveOsInput::FindForeignWindowAt(TargetCenter, Foreign))
                    {
                        SendOsOccluded(*Token, Handle, TargetCenter, Foreign);
                    }
                    else
                    {
                        SendOccluded(*Token, Handle, TargetCenter, OwnWindowOnXPath(XPath), bRaiseAttempted, RaiseWaitSeconds, TargetXWindow);
                    }
                    return false;
                }
                if (bRequireActive)
                {
                    bool bWmReports = false;
                    const uint64 Active = FDriveOsInput::XActiveWindow(&bWmReports);
                    const uint64 ActivationXWindow = FDriveOsInput::XWindowIdOf(ActivationWindow->GetNativeWindow());
                    const bool bSdlFocus = FDriveOsInput::HasSdlKeyboardFocus(Target->GetNativeWindow())
                        || FDriveOsInput::HasSdlKeyboardFocus(ActivationWindow->GetNativeWindow());
                    if (!FDriveOsInput::IsActiveEnough(bWmReports, Active, ActivationXWindow, bSdlFocus))
                    {
                        if (!bGateExpired)
                        {
                            return true;
                        }
                        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
                        Details->SetStringField(TEXT("handle"), Handle);
                        Details->SetNumberField(TEXT("target_window_id"), static_cast<double>(ActivationXWindow));
                        Details->SetNumberField(TEXT("active_window_id"), static_cast<double>(Active));
                        Details->SetBoolField(TEXT("sdl_focus"), bSdlFocus);
                        Details->SetStringField(TEXT("recovery"), FString::Printf(
                            TEXT("focus the editor once (click it, or run 'xdotool windowactivate 0x%llx' on its X display; no PinWright verb focuses a window), then retry"), ActivationXWindow));
                        Token->SendError(ErrorCodes::ERR_INPUT_FAILED,
                            FString::Printf(TEXT("Element '%s''s window 0x%llx did not become the active window within %.1f s (active: 0x%llx, editor keyboard focus there: %s), so a real press would only focus it and click nothing. Nothing was pressed. This editor activates the target itself only while it is the focused application; focus the editor once (click it, or run 'xdotool windowactivate 0x%llx' on its X display; no PinWright verb focuses a window), then retry."),
                                *Handle, ActivationXWindow, RaiseWaitSeconds, Active, bSdlFocus ? TEXT("yes") : TEXT("no"), ActivationXWindow),
                            Details);
                        return false;
                    }
                }
                if (*ReadyFrame == 0)
                {
                    FDriveInjectFailure MoveFailure{ ErrorCodes::ERR_INPUT_FAILED, InjectFailedMessage };
                    if (!FDriveOsInput::MoveTo(TargetCenter, MoveFailure))
                    {
                        Token->SendError(MoveFailure.Code, MoveFailure.Message, MoveFailure.Details);
                        return false;
                    }
                    *ReadyFrame = GFrameCounter + FocusSettleFrames;
                    *EnterDeadline = FPlatformTime::Seconds() + RaiseWaitSeconds;
                    return true;
                }
                if (GFrameCounter < *ReadyFrame || FDriveInput::WindowUnderPoint(TargetCenter) != Target)
                {
                    if (GFrameCounter < *ReadyFrame || FPlatformTime::Seconds() < *EnterDeadline)
                    {
                        return true;
                    }
                    Token->SendError(ErrorCodes::ERR_INPUT_FAILED,
                        FString::Printf(TEXT("The pointer was moved onto element '%s' but the editor did not report it inside the element's window within %.1f s, so nothing was pressed. Retry."),
                            *Handle, RaiseWaitSeconds));
                    return false;
                }
                FDriveInjectFailure Failure{ ErrorCodes::ERR_INPUT_FAILED, InjectFailedMessage };
                if (Inject(TargetCenter, WeakTargetWidget.Pin(), Failure))
                {
                    StartSettle(Token);
                }
                else
                {
                    Token->SendError(Failure.Code, Failure.Message, Failure.Details);
                }
                return false;
            }));
    }
}

using namespace DriveActionCommonLocal;

bool FDriveActionCommon::IsActionable(const FDriveElement& Element)
{
    // The geometry term is not folded into bVisible on purpose: it states the invariant every
    // caller of "the element's center" depends on - never aim at a rect Slate did not measure
    // this frame - so it keeps holding if staleness ever stops implying invisible.
    return Element.bVisible && Element.bEnabled && !Element.bGeometryStale;
}

bool FDriveActionCommon::IsAbsenceUnverifiable(const TOptional<FDriveCondition>& WaitFor,
    const TArray<FDriveElement>& Baseline)
{
    // An empty baseline (the pre-action sample failed) carries no evidence either way.
    return WaitFor.IsSet() && WaitFor->Type == EDriveConditionType::WidgetAbsent
        && Baseline.Num() > 0
        && !Baseline.ContainsByPredicate([&WaitFor](const FDriveElement& Element)
            { return FDriveConditionEval::ElementMatchesTarget(Element, WaitFor->Target); });
}

EDriveObserveMode FDriveActionCommon::ParseObserveMode(const FHandlerContext& Ctx)
{
    // DEFAULT changed to none: action / wait verbs no longer embed a full observation the
    // caller did not ask for. observe=list / list+screenshot re-enables it.
    const FString Token = Ctx.GetString(TEXT("observe"), TEXT("none"));
    if (Token.Equals(TEXT("none"), ESearchCase::IgnoreCase))
    {
        return EDriveObserveMode::None;
    }
    if (Token.Equals(TEXT("list"), ESearchCase::IgnoreCase))
    {
        return EDriveObserveMode::List;
    }
    // "list+screenshot" / "list_screenshot" / "screenshot" / anything else.
    return EDriveObserveMode::ListAndScreenshot;
}

FString FDriveActionCommon::SettleOutcomeToString(EDriveSettleOutcome Outcome)
{
    switch (Outcome)
    {
    case EDriveSettleOutcome::SettledChanged:        return TEXT("settled_changed");
    case EDriveSettleOutcome::NoChangeWithinBudget:  return TEXT("no_change_within_budget");
    case EDriveSettleOutcome::WaitForMet:            return TEXT("wait_for_met");
    case EDriveSettleOutcome::Timeout:               return TEXT("timeout");
    case EDriveSettleOutcome::ChangedUnsettled:      return TEXT("changed_unsettled");
    case EDriveSettleOutcome::Continue:
    default:                                         return TEXT("continue");
    }
}

TSharedPtr<FJsonObject> FDriveActionCommon::WriteSettleResult(FDriveSettleResult Result, const FDriveDiff& Diff, bool bFullDiff)
{
    if (!Diff.IsEmpty() && !Result.bChanged)
    {
        Result.bChanged = true;
        if (Result.Outcome == EDriveSettleOutcome::NoChangeWithinBudget)
        {
            Result.Outcome = EDriveSettleOutcome::SettledChanged;
            Result.bSettled = true;
        }
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("outcome"), SettleOutcomeToString(Result.Outcome));
    Resp->SetBoolField(TEXT("changed"), Result.bChanged);
    Resp->SetBoolField(TEXT("settled"), Result.bSettled);
    Resp->SetBoolField(TEXT("condition_met"), Result.bConditionMet);
    Resp->SetNumberField(TEXT("elapsed_ms"), Result.ElapsedMs);
    Resp->SetNumberField(TEXT("ticks"), Result.Ticks);
    // Compact summary by default; full handle lists only when full_diff was set.
    Resp->SetObjectField(TEXT("diff"), bFullDiff ? FDriveJson::WriteDiffFull(Diff) : FDriveJson::WriteDiffSummary(Diff));
    // The quiet budget only proves nothing changed while it ran: a click whose screen
    // transition starts later reads exactly like a click that did nothing. Say so, and name
    // the cheap way to tell them apart.
    if (Result.Outcome == EDriveSettleOutcome::NoChangeWithinBudget)
    {
        Resp->SetStringField(TEXT("hint"),
            TEXT("The input was injected, but the UI did not change before quiet_budget_ms ran out. A transition that starts later (screen navigation, menu animation, level load) is not seen, so this does not prove the action was ignored. To check, call drive.wait_for on the expected result (e.g. widget_present) instead of repeating the action; next time pass wait_for to the action or raise quiet_budget_ms."));
    }
    else if (Result.Outcome == EDriveSettleOutcome::ChangedUnsettled)
    {
        Resp->SetStringField(TEXT("hint"),
            TEXT("The UI changed after the input but was still moving (or too few frames ran) when settle_budget_ms ran out. A change does not prove the input caused it; confirm the expected end state with drive.wait_for or drive.expect before repeating the action, or raise settle_budget_ms."));
    }
    return Resp;
}

FDriveSettleDriver::FGetElements FDriveActionCommon::MakeGetElements(
    EDriveSurface Surface, const FDriveRootSelector& Selector,
    const FDriveWindowSelector& WindowSelector)
{
    return [Surface, Selector, WindowSelector]() -> TArray<FDriveElement>
    {
        TArray<FDriveElement> Elements;
        FString RootName;
        FString ErrorCode;
        FString ErrorMessage;
        FDriveHandlerCommon::GetElementsForSurface(Surface, Selector, Elements, RootName, ErrorCode, ErrorMessage, WindowSelector);
        return Elements;
    };
}

FDriveSettleDriver::FIsWaitForMet FDriveActionCommon::MakeIsWaitForMet(
    EDriveSurface Surface, const FDriveRootSelector& Selector, const FDriveCondition& Condition,
    const FDriveWindowSelector& WindowSelector)
{
    return [Surface, Selector, Condition, WindowSelector]() -> bool
    {
        TArray<FDriveElement> Elements;
        FString RootName;
        FString ErrorCode;
        FString ErrorMessage;
        FDriveHandlerCommon::GetElementsForSurface(Surface, Selector, Elements, RootName, ErrorCode, ErrorMessage, WindowSelector);

        // Journal conditions read the live tail; widget/text/count/geometry
        // conditions only read Elements, so the delta stays empty for those.
        FDriveJournalDelta Journal;
        const bool bJournalCondition =
            Condition.Type == EDriveConditionType::JournalEvent ||
            Condition.Type == EDriveConditionType::JournalSeverity;
        if (bJournalCondition)
        {
            FDriveHandlerCommon::GetJournalDelta(0, Journal);
        }

        return FDriveConditionEval::Evaluate(Condition, Elements, Journal).bMet;
    };
}

TSharedPtr<FJsonObject> FDriveActionCommon::BuildObservationField(
    EDriveSurface Surface, const FDriveRootSelector& Selector,
    EDriveObserveMode ObserveMode, int32 MarkCap,
    const FDriveWindowSelector& WindowSelector)
{
    if (ObserveMode == EDriveObserveMode::None)
    {
        return nullptr;
    }

    const bool bScreenshot = (ObserveMode == EDriveObserveMode::ListAndScreenshot);
    FDriveObservation Observation;
    FString ErrorCode;
    FString ErrorMessage;
    // Journal rides as a top-level field (MaybeAttachJournal), never inside the
    // observation here, so the delta surfaces even with observe=none.
    if (!FDriveHandlerCommon::BuildObservation(
            Surface, Selector, bScreenshot, MarkCap, /*bIncludeJournal=*/false, /*JournalSince=*/0,
            Observation, ErrorCode, ErrorMessage, WindowSelector))
    {
        return nullptr;
    }
    return FDriveJson::WriteObservation(Observation);
}

void FDriveActionCommon::MaybeAttachJournal(
    const TSharedPtr<FJsonObject>& Resp, bool bIncludeJournal, uint64 JournalSince)
{
    if (!bIncludeJournal || !Resp.IsValid())
    {
        return;
    }
    FDriveJournalDelta Delta;
    FDriveHandlerCommon::GetJournalDelta(JournalSince, Delta);
    Resp->SetObjectField(TEXT("journal"), FDriveJson::WriteJournalDelta(Delta));
}

void FDriveActionCommon::RunAction(FHandlerContext& Ctx, const FString& Handle, const FInject& Inject,
    const TCHAR* InputPath, const TSharedPtr<FJsonObject>& InjectFields, bool bPress)
{
    RunAction(Ctx, Handle,
        [Inject](const FVector2D& TargetCenter, const TSharedPtr<SWidget>&, FDriveInjectFailure& OutFailure)
        {
            return Inject(TargetCenter, OutFailure);
        },
        InputPath, InjectFields, bPress);
}

void FDriveActionCommon::RunAction(FHandlerContext& Ctx, const FString& Handle, const FInjectOnTarget& Inject,
    const TCHAR* InputPath, const TSharedPtr<FJsonObject>& InjectFields, bool bPress)
{
    const FString InputPathLabel = InputPath ? FString(InputPath) : FString();

    const EDriveSurface Surface = FDriveHandlerCommon::ResolveSurface(Ctx);
    const FDriveRootSelector Selector = FDriveHandlerCommon::ParseRootSelector(Ctx);
    // Editor chrome is addressed by window title / index (game ignores this selector).
    const FDriveWindowSelector WindowSelector = FDriveHandlerCommon::ParseWindowSelector(Ctx);

    // Game/PIE and editor chrome are wired for live action; reuse the provider switch's
    // canonical SURFACE_NOT_SUPPORTED error for everything else (web arrives later).
    if (Surface != EDriveSurface::Game && Surface != EDriveSurface::EditorChrome)
    {
        TArray<FDriveElement> Unused;
        FString RootName;
        FString ErrorCode;
        FString ErrorMessage;
        FDriveHandlerCommon::GetElementsForSurface(Surface, Selector, Unused, RootName, ErrorCode, ErrorMessage, WindowSelector);
        Ctx.SendError(ErrorCode, ErrorMessage);
        return;
    }

    // Settle / wait_for tuning. ParseSettleConfig reads the budgets and an optional
    // `wait_for` sub-object; a present-but-malformed wait_for is a CONDITION_INVALID.
    FDriveSettleConfig Config;
    FString WaitForError;
    if (!FDriveJson::ParseSettleConfig(Ctx.GetRawPayload(), Config, &WaitForError))
    {
        Ctx.SendError(ErrorCodes::ERR_CONDITION_INVALID, TEXT("Invalid 'wait_for': ") + WaitForError);
        return;
    }
    // The action-verb wire name for the wait_for timeout is `timeout_ms`; honor it
    // over the config's wait_for_timeout_ms when present.
    const TOptional<int32> TimeoutMs = Ctx.GetIntFirstOf({ TEXT("timeout_ms") });
    if (TimeoutMs.IsSet())
    {
        Config.WaitForTimeoutMs = TimeoutMs.GetValue();
    }

    const EDriveObserveMode ObserveMode = ParseObserveMode(Ctx);
    const int32 MarkCap = ReadMarkCap(Ctx);
    const bool bIncludeJournal = ReadIncludeJournal(Ctx);
    const uint64 JournalSince = ReadJournalSince(Ctx);
    // The diff defaults to the compact summary (counts + samples); full_diff=true swaps in
    // the complete appeared/disappeared/changed handle lists.
    const bool bFullDiff = Ctx.GetBool(TEXT("full_diff"), false);

    FVector2D TargetCenter = FVector2D::ZeroVector;
    // The target's own top-level window; pointer input must be routed there to reach it.
    TSharedPtr<SWindow> TargetWindow;
    TSharedPtr<SWidget> TargetWidget;

    // Stale-state guard: re-resolve the target NOW (only when an action targets a
    // handle; drive.key without a handle acts on the focused widget). The resolve is
    // surface-aware, so an editor-chrome handle re-walks the selected editor window.
    if (!Handle.IsEmpty())
    {
        const FDriveResolveResult Resolved = FDriveHandlerCommon::ResolveForSurface(Surface, Ctx, Handle);
        switch (Resolved.Status)
        {
        case EDriveResolveStatus::Found:
            break;
        case EDriveResolveStatus::NotFound:
            Ctx.SendError(ErrorCodes::ERR_TARGET_NOT_FOUND,
                FString::Printf(TEXT("No live element carries handle '%s'."), *Handle));
            return;
        case EDriveResolveStatus::Ambiguous:
            Ctx.SendError(ErrorCodes::ERR_TARGET_AMBIGUOUS,
                Resolved.ErrorMessage.IsEmpty()
                    ? FString(TEXT("The live-UI root selector matched more than one root; disambiguate with instance_name / root_index."))
                    : Resolved.ErrorMessage);
            return;
        case EDriveResolveStatus::NoLiveUi:
        default:
            // The resolver's own code is the specific reason (PIE_NOT_RUNNING, etc.).
            Ctx.SendError(Resolved.ErrorCode, Resolved.ErrorMessage);
            return;
        }

        // The element must still be actionable; a hidden/disabled/unmeasured re-resolve is a
        // changed target, not a silent no-op.
        if (!FDriveActionCommon::IsActionable(Resolved.Element))
        {
            Ctx.SendError(ErrorCodes::ERR_TARGET_CHANGED,
                FString::Printf(
                    TEXT("Element '%s' is no longer actionable (visible=%s, enabled=%s, geometry_stale=%s)."),
                    *Handle,
                    Resolved.Element.bVisible ? TEXT("true") : TEXT("false"),
                    Resolved.Element.bEnabled ? TEXT("true") : TEXT("false"),
                    Resolved.Element.bGeometryStale ? TEXT("true") : TEXT("false")));
            return;
        }

        TargetCenter = Resolved.Element.AbsolutePosition + Resolved.Element.AbsoluteSize * 0.5;
        if (Resolved.Widget.IsValid() && FSlateApplication::IsInitialized())
        {
            TargetWindow = FSlateApplication::Get().FindWidgetWindow(Resolved.Widget.ToSharedRef());
        }
        TargetWidget = Resolved.Widget;
    }

    // Pre-action baseline for the diff. For a target-less action this sample is
    // also the live-UI guard: a failure means there is nothing to drive.
    TArray<FDriveElement> PreElements;
    {
        FString RootName;
        FString ErrorCode;
        FString ErrorMessage;
        if (!FDriveHandlerCommon::GetElementsForSurface(Surface, Selector, PreElements, RootName, ErrorCode, ErrorMessage, WindowSelector))
        {
            if (Handle.IsEmpty())
            {
                Ctx.SendError(ErrorCode, ErrorMessage);
                return;
            }
            // With a resolved handle the live UI is known good; treat a baseline
            // miss as an empty baseline rather than failing the action.
            PreElements.Reset();
        }
    }

    // A widget_absent wait whose target is not there before the action would be met on the
    // first tick whatever the action did. Refuse before injecting instead.
    if (IsAbsenceUnverifiable(Config.WaitFor, PreElements))
    {
        Ctx.SendError(ErrorCodes::ERR_CONDITION_INVALID,
            FString::Printf(
                TEXT("wait_for widget_absent target '%s' matches no element before the action (%d observed), so its absence would prove nothing. Use a handle, a handle segment (the UMG widget name) or a label from drive.observe."),
                *Config.WaitFor->Target, PreElements.Num()));
        return;
    }

    // Everything after a successful injection: run the settle loop and resolve the request
    // through Token once it reaches a terminal outcome.
    const auto StartSettle =
        [Config, Surface, Selector, WindowSelector, PreElements, ObserveMode, MarkCap, bIncludeJournal, JournalSince, bFullDiff, InputPathLabel, InjectFields]
        (const TSharedRef<FAsyncResponseToken>& Token)
    {
        FDriveSettleDriver::FIsWaitForMet IsWaitForMet = nullptr;
        if (Config.WaitFor.IsSet())
        {
            IsWaitForMet = MakeIsWaitForMet(Surface, Selector, Config.WaitFor.GetValue(), WindowSelector);
        }

        FDriveSettleDriver::FOnComplete OnComplete =
            [Token, Surface, Selector, WindowSelector, PreElements, ObserveMode, MarkCap, bIncludeJournal, JournalSince, bFullDiff, InputPathLabel, InjectFields]
            (const FDriveSettleResult& Result, const TArray<FDriveElement>& FinalElements)
            {
                TSharedPtr<FJsonObject> Resp = WriteSettleResult(
                    Result, FDriveChangeDetector::Diff(PreElements, FinalElements), bFullDiff);
                if (!InputPathLabel.IsEmpty())
                {
                    Resp->SetStringField(TEXT("input_path"), InputPathLabel);
                }
                if (InjectFields.IsValid())
                {
                    for (const auto& Field : InjectFields->Values)
                    {
                        Resp->SetField(Field.Key, Field.Value);
                    }
                }
                // XWayland passes IsAvailable, but real-input delivery there is unverified, so a
                // no_change outcome on Wayland may mean the events never arrived.
                if (InputPathLabel == TEXT("os_x11"))
                {
                    const FString Session = FDriveOsInput::SessionType();
                    Resp->SetStringField(TEXT("session"), Session);
                    if (Session == TEXT("wayland"))
                    {
                        Resp->SetStringField(TEXT("warning"),
                            TEXT("Wayland session: this editor's X display is XWayland, where XTEST delivery, pointer grabs and confinement are compositor-mediated and unverified, so the input may not have reached the editor. Log in to an X11 session, or start the editor on a private X display (editor_start display:\"xvfb\"/\"xephyr\")."));
                    }
                }

                if (TSharedPtr<FJsonObject> Observation =
                        BuildObservationField(Surface, Selector, ObserveMode, MarkCap, WindowSelector))
                {
                    Resp->SetObjectField(TEXT("observation"), Observation);
                }

                MaybeAttachJournal(Resp, bIncludeJournal, JournalSince);

                Token->SendSuccess(Resp);
            };

        // The ticker started by Start() holds the only owning reference to the driver;
        // it lives exactly until Tick() reaches a terminal outcome and fires OnComplete,
        // so we deliberately drop the returned ref here.
        FDriveSettleDriver::Start(
            Config,
            MakeGetElements(Surface, Selector, WindowSelector),
            MoveTemp(IsWaitForMet),
            MoveTemp(OnComplete));
    };

    const TCHAR* InjectFailedMessage =
        TEXT("Synthetic input injection failed (Slate not initialized or no window under the point).");

    // Slate routes pointer input to whichever top-level window LocateWindowUnderMouse names for
    // the point, which need not be the target's own: a window stacked over it (the editor opens a
    // Message Log on PIE start) takes the click, and on Linux the platform's window-under-cursor
    // still names the window the pointer was over before the warp until SDL's enter event is
    // pumped, so the first click into another window is hit-tested in the old one and lands on
    // nothing. Either way the widget never sees it and the settle loop reports a benign
    // no_change_within_budget. So move the pointer first and inject only once the routing names
    // the target's window. os_input skips that wait: it never warps the Slate cursor, and the X
    // server routes its events itself; it gets the two window-order checks below instead. A retainer's virtual window never appears in that routing.
    // os_input's counterpart: the X server routes real input to the top-most X window at the
    // point, which may be another process's (a peer editor or game on a shared display).
    // Check before anything is injected; ClickAt re-checks before the press.
    // The X check is about process ownership only, so another window of THIS editor stacked over
    // the target (a Message Log, a plugin window, a notification toast) would still take the real
    // click. Slate's window order mirrors the X stacking of this editor's own windows (it brings a
    // window to front on activation, and keeps top-most windows above), so refuse when it names
    // another window at the point. Checked first so the occluder named is this editor's own.
    // Slate's order and X's stacking can disagree (Slate reorders only on activation, the window
    // manager restacks on its own), so when the target has an X window the X route at the point
    // must pass through it too; the window X names instead is found among this editor's windows by
    // X id. A PIE window there is raised over: X restacks asynchronously, so press only once X
    // routes the point through the target's own X window.
    if (InputPathLabel == FDriveOsInput::InputPathLabel() && TargetWindow.IsValid() && !TargetWindow->IsVirtualWindow())
    {
        // A held pointer grab (a system-modal prompt, a game's capture) takes real input whatever
        // the stacking, and _NET_ACTIVE_WINDOW still names the last app window under a GNOME
        // system modal; refuse with POINTER_GRABBED before activating or waiting on anything.
        FDriveInjectFailure GrabFailure{ ErrorCodes::ERR_INPUT_FAILED, FString() };
        if (!FDriveOsInput::CheckPointerFree(GrabFailure))
        {
            Ctx.SendError(GrabFailure.Code, GrabFailure.Message, GrabFailure.Details);
            return;
        }
        const uint64 TargetXWindow = FDriveOsInput::XWindowIdOf(TargetWindow->GetNativeWindow());
        const TSharedPtr<SWindow> ActivationWindow = ActivationWindowOf(TargetWindow);
        const uint64 ActivationXWindow = ActivationWindow.IsValid() ? FDriveOsInput::XWindowIdOf(ActivationWindow->GetNativeWindow()) : 0;
        bool bWmReportsActive = false;
        const uint64 ActiveXWindow = FDriveOsInput::XActiveWindow(&bWmReportsActive);
        // Activate (pager-style) only for a press, only with a window manager to ask, and only
        // while this editor already is the focused application, so no other application loses
        // focus. With no window manager at all (a private Xvfb / Xephyr from editor_start) nothing
        // would ever move focus to the target, so a press gives it the X input focus directly, as
        // SDL does on such a display.
        const bool bMayActivate = bPress && bWmReportsActive && IsOwnXWindow(ActiveXWindow);
        // Called only once the occlusion refusals below have passed, so a refused call changes
        // nothing; and only while the X input focus is None, PointerRoot or already this editor's.
        const auto FocusWithoutWm = [bPress, bWmReportsActive, ActivationXWindow]()
        {
            const uint64 Focus = FDriveOsInput::XInputFocus();
            if (bPress && !bWmReportsActive && ActivationXWindow != 0 && (Focus <= 1 || IsOwnXWindow(Focus)))
            {
                FDriveOsInput::FocusXWindow(ActivationXWindow);
            }
        };
        TSharedPtr<SWindow> Occluder = FDriveInput::TopWindowAtPoint(TargetCenter);
        if (Occluder == TargetWindow)
        {
            Occluder.Reset();
        }
        if (!Occluder.IsValid() && TargetXWindow != 0)
        {
            const TArray<uint64> XPath = FDriveOsInput::XWindowPathAt(TargetCenter);
            FDriveOsInput::FForeignWindow Foreign;
            if (!XPath.Contains(TargetXWindow) && !FDriveOsInput::FindForeignWindowAt(TargetCenter, Foreign))
            {
                Occluder = OwnWindowOnXPath(XPath);
                if (!Occluder.IsValid())
                {
                    TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
                    Details->SetStringField(TEXT("handle"), Handle);
                    Details->SetNumberField(TEXT("x"), TargetCenter.X);
                    Details->SetNumberField(TEXT("y"), TargetCenter.Y);
                    Ctx.SendError(ErrorCodes::ERR_TARGET_OCCLUDED,
                        FString::Printf(TEXT("Element '%s' is covered at (%.0f, %.0f) by an X window of this editor that is not the target's and that no visible Slate window owns. Nothing was injected."),
                            *Handle, TargetCenter.X, TargetCenter.Y),
                        Details);
                    return;
                }
            }
        }
        if (Occluder.IsValid())
        {
            if (TargetXWindow == 0 || !IsPieViewportWindow(Occluder))
            {
                SendOccluded(*Ctx.MakeAsyncToken(), Handle, TargetCenter, Occluder);
                return;
            }
            // The target must end up on top AND active (PressWhenActive). While this editor is the
            // focused application, activating its own window takes focus from no one: ask the
            // window manager pager-style, which mutter honours. Otherwise only a plain
            // XRaiseWindow, which never moves focus off another application; mutter declines it
            // there, and the call is refused with a recovery. Mutter was seen to focus the target
            // on the activation without restacking it, so the raise is always sent too.
            if (bMayActivate)
            {
                FDriveOsInput::ActivateXWindow(ActivationXWindow);
            }
            FDriveOsInput::RaiseXWindow(TargetXWindow);
            FocusWithoutWm();
            PressWhenActive(Ctx.MakeAsyncToken(), Handle, TargetWindow, TargetWidget, TargetXWindow, TargetCenter,
                /*bRaiseAttempted=*/true, /*bRequireActive=*/bPress, Inject, StartSettle, InjectFailedMessage);
            return;
        }
        if (TargetXWindow != 0)
        {
            FDriveOsInput::FForeignWindow Foreign;
            if (FDriveOsInput::FindForeignWindowAt(TargetCenter, Foreign))
            {
                SendOsOccluded(*Ctx.MakeAsyncToken(), Handle, TargetCenter, Foreign);
                return;
            }
            // Uncovered, but a press into a window that is not the active one clicks nothing
            // (#432): activate it under the same rule as the raise above, then press once it is.
            if (bMayActivate && ActiveXWindow != ActivationXWindow)
            {
                FDriveOsInput::ActivateXWindow(ActivationXWindow);
            }
            FocusWithoutWm();
            PressWhenActive(Ctx.MakeAsyncToken(), Handle, TargetWindow, TargetWidget, TargetXWindow, TargetCenter,
                /*bRaiseAttempted=*/false, /*bRequireActive=*/bPress, Inject, StartSettle, InjectFailedMessage);
            return;
        }
    }
    FDriveOsInput::FForeignWindow Foreign;
    if (InputPathLabel == FDriveOsInput::InputPathLabel() && FDriveOsInput::FindForeignWindowAt(TargetCenter, Foreign))
    {
        SendOsOccluded(*Ctx.MakeAsyncToken(), Handle, TargetCenter, Foreign);
        return;
    }

    const bool bGatePointer = TargetWindow.IsValid() && !TargetWindow->IsVirtualWindow()
        && InputPathLabel != FDriveOsInput::InputPathLabel();
    // The native raise does not activate, so a window manager that declines raises from an
    // unfocused editor leaves the PIE window on top and the route wait below refuses.
    bool bRaised = false;
    if (bGatePointer)
    {
        bRaised = RaiseOverPieWindow(TargetWindow, FDriveInput::TopWindowAtPoint(TargetCenter));
        FDriveInput::MoveTo(TargetCenter);
    }

    if (!bGatePointer || FDriveInput::WindowUnderPoint(TargetCenter) == TargetWindow)
    {
        // Inject the input. A failure here (Slate down mid-flight, an os_input refusal) is a
        // clean error.
        FDriveInjectFailure Failure{ ErrorCodes::ERR_INPUT_FAILED, InjectFailedMessage };
        if (!Inject(TargetCenter, TargetWidget, Failure))
        {
            Ctx.SendError(Failure.Code, Failure.Message, Failure.Details);
            return;
        }
        // From here the request resolves asynchronously: open it now so the transport
        // keeps it alive until OnComplete fires on the game thread.
        StartSettle(Ctx.MakeAsyncToken());
        return;
    }

    // Routed elsewhere: give the platform a few frames to catch up with the warp, then refuse
    // rather than inject into the wrong window.
    TSharedRef<FAsyncResponseToken> Token = Ctx.MakeAsyncToken();
    const double WaitSeconds = bRaised ? RaiseWaitSeconds : PointerRouteWaitSeconds;
    const double Deadline = FPlatformTime::Seconds() + WaitSeconds;
    const TWeakPtr<SWindow> WeakTargetWindow = TargetWindow;
    const TWeakPtr<SWidget> WeakTargetWidget = TargetWidget;
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
        [Token, WeakTargetWindow, WeakTargetWidget, TargetCenter, Deadline, WaitSeconds, bRaised, Inject, StartSettle, Handle, InjectFailedMessage](float) -> bool
        {
            const TSharedPtr<SWindow> Target = WeakTargetWindow.Pin();
            if (!Target.IsValid())
            {
                Token->SendError(ErrorCodes::ERR_TARGET_CHANGED,
                    FString::Printf(TEXT("The window holding element '%s' closed before the input could be delivered."), *Handle));
                return false;
            }
            // The widget can go while its window stays (a stopped PIE in the level editor window).
            if (!WeakTargetWidget.IsValid())
            {
                Token->SendError(ErrorCodes::ERR_TARGET_CHANGED,
                    FString::Printf(TEXT("Element '%s' went away before the input could be delivered."), *Handle));
                return false;
            }
            const TSharedPtr<SWindow> Under = FDriveInput::WindowUnderPoint(TargetCenter);
            if (Under == Target)
            {
                FDriveInjectFailure Failure{ ErrorCodes::ERR_INPUT_FAILED, InjectFailedMessage };
                if (Inject(TargetCenter, WeakTargetWidget.Pin(), Failure))
                {
                    StartSettle(Token);
                }
                else
                {
                    Token->SendError(Failure.Code, Failure.Message, Failure.Details);
                }
                return false;
            }
            if (FPlatformTime::Seconds() < Deadline)
            {
                return true;
            }
            SendOccluded(*Token, Handle, TargetCenter, Under, bRaised, WaitSeconds,
                FDriveOsInput::XWindowIdOf(Target->GetNativeWindow()));
            return false;
        }));
}
