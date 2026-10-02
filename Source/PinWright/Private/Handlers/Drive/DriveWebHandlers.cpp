// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Drive/DriveWebHandlers.h"

#include "Handlers/Drive/DriveActionCommon.h"     // EDriveObserveMode, ParseObserveMode
#include "Handlers/Drive/DriveConditionEval.h"
#include "Handlers/Drive/DriveFingerprint.h"        // FDriveDiff, FDriveChangeDetector
#include "Handlers/Drive/DriveHandlerCommon.h"      // GetJournalDelta
#include "Handlers/Drive/DriveInput.h"
#include "Handlers/Drive/DriveJson.h"
#include "Handlers/Drive/DriveSetOfMarkRenderer.h"
#include "Handlers/Drive/DriveSettleDriver.h"
#include "Handlers/Drive/DriveTypes.h"
#include "Handlers/Drive/DriveWebBridge.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/ICursor.h"
#include "Input/HittestGrid.h"
#include "HAL/PlatformTime.h"
#include "Layout/WidgetPath.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersionComparison.h"
#include "WebBrowser.h"
#include "Widgets/SWindow.h"

// File-local helpers in a uniquely-named namespace (not anonymous): the plugin's Unity
// build merges translation units, and a distinct namespace keeps these from colliding with
// same-named helpers in sibling drive units.
namespace DriveWebHandlersLocal
{
    // Per-round-trip CEF console timeout for an observe/action DOM query.
    constexpr double GQueryTimeoutSeconds = 5.0;
    // Poll interval for WaitForWeb's DOM polling (one CEF round-trip per interval).
    constexpr double GWaitPollIntervalSeconds = 0.25;

    // Build the `observation` JSON for a web element set honoring ObserveMode. Returns null
    // for EDriveObserveMode::None. The Set-of-Mark screenshot captures the GAME viewport (the
    // CEF HUD composites into it via OSR) and is best-effort: a capture miss just omits the
    // screenshot. Journal rides as a top-level field elsewhere, never inside this object.
    TSharedPtr<FJsonObject> BuildWebObservationJson(
        const TArray<FDriveElement>& Elements, EDriveObserveMode ObserveMode, int32 MarkCap)
    {
        if (ObserveMode == EDriveObserveMode::None)
        {
            return nullptr;
        }

        FDriveObservation Observation;
        Observation.Surface = EDriveSurface::Web;
        Observation.Elements = Elements;

        if (ObserveMode == EDriveObserveMode::ListAndScreenshot)
        {
            FDriveScreenshot Screenshot;
            FString ScreenshotErrorCode;
            if (FDriveSetOfMarkRenderer::CaptureAnnotated(
                    Observation.Elements, MarkCap, Screenshot, ScreenshotErrorCode, EDriveSurface::Web))
            {
                Observation.Screenshot = MoveTemp(Screenshot);
            }
        }

        Observation.Frame = static_cast<int64>(GFrameCounter);
        Observation.Timestamp = FDateTime::UtcNow();
        return FDriveJson::WriteObservation(Observation);
    }

    // True when Condition is a journal condition (reads the live tail) rather than an
    // element condition (reads only the queried DOM elements).
    bool IsJournalCondition(const FDriveCondition& Condition)
    {
        return Condition.Type == EDriveConditionType::JournalEvent ||
               Condition.Type == EDriveConditionType::JournalSeverity;
    }

    // Evaluate a condition over a web element set, folding in the live journal tail only
    // for journal conditions.
    bool EvaluateOverElements(const FDriveCondition& Condition, const TArray<FDriveElement>& Elements)
    {
        FDriveJournalDelta Journal;
        if (IsJournalCondition(Condition))
        {
            FDriveHandlerCommon::GetJournalDelta(0, Journal);
        }
        return FDriveConditionEval::Evaluate(Condition, Elements, Journal).bMet;
    }

    // Resolve an async token with WEB_BROWSER_NOT_FOUND. Sent synchronously (before any
    // round-trip) so the no-live-browser case is a clean error, never a hang.
    void SendNoBrowser(FHandlerContext& Ctx, int32 BrowserIndex)
    {
        Ctx.SendError(ErrorCodes::ERR_WEB_BROWSER_NOT_FOUND,
            FString::Printf(
                TEXT("No live CEF web browser at browser_index %d. surface=web needs a running "
                     "WebUI/CEF browser (e.g. a PIE HUD); none was discovered."),
                BrowserIndex));
    }

    // What every web action verb reads from Ctx besides its own input params.
    struct FWebActionParams
    {
        FDriveSettleConfig Config;
        EDriveObserveMode ObserveMode = EDriveObserveMode::None;
        int32 MarkCap = 50;
        bool bFullDiff = false;
        bool bIncludeJournal = false;
        uint64 JournalSince = 0;
        // Echoed as `input_path`: "slate" for real input routed through Slate into CEF, "dom" for
        // drive.type's DOM value write.
        FString InputPath;
    };

    // A refused or failed injection, sent as the action's error. An empty Code means the input
    // was delivered.
    struct FWebInjectOutcome
    {
        FString Code;
        FString Message;
        TSharedPtr<FJsonObject> Details;
    };
    using FWebInjectDone = TFunction<void(const FWebInjectOutcome&)>;
    using FWebInject = TFunction<void(UWebBrowser* Browser, FWebInjectDone Done)>;

    // Select the browser and read the shared action params. Returns null AFTER sending the error
    // (no live browser, malformed wait_for).
    UWebBrowser* ReadWebAction(FHandlerContext& Ctx, const TCHAR* InputPath, FWebActionParams& Out)
    {
        const int32 BrowserIndex = Ctx.GetInt(TEXT("browser_index"), 0);
        UWebBrowser* Browser = FDriveWebBridge::SelectBrowser(BrowserIndex);
        if (!Browser)
        {
            SendNoBrowser(Ctx, BrowserIndex);
            return nullptr;
        }
        if (!FDriveJson::ParseSettleConfig(Ctx.GetRawPayload(), Out.Config))
        {
            Ctx.SendError(ErrorCodes::ERR_CONDITION_INVALID,
                TEXT("The 'wait_for' object has an unrecognized 'type', or has no 'target' (every type but journal_severity needs one)."));
            return nullptr;
        }
        const TOptional<int32> TimeoutMs = Ctx.GetIntFirstOf({ TEXT("timeout_ms") });
        if (TimeoutMs.IsSet())
        {
            Out.Config.WaitForTimeoutMs = TimeoutMs.GetValue();
        }
        Out.ObserveMode = FDriveActionCommon::ParseObserveMode(Ctx);
        Out.MarkCap = Ctx.GetInt(TEXT("mark_cap"), 50);
        Out.bFullDiff = Ctx.GetBool(TEXT("full_diff"), false);
        Out.bIncludeJournal = Ctx.GetBool(TEXT("include_journal"), false);
        const int32 SinceRaw = Ctx.GetInt(TEXT("journal_since"), 0);
        Out.JournalSince = SinceRaw > 0 ? static_cast<uint64>(SinceRaw) : 0;
        Out.InputPath = InputPath;
        return Browser;
    }

    // os_input drives the X server, not a page; refuse it rather than accept and ignore it.
    bool RefuseOsInput(FHandlerContext& Ctx)
    {
        if (!Ctx.GetBool(TEXT("os_input"), false))
        {
            return false;
        }
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("os_input is not supported on surface=web; web input is routed through Slate into the browser."));
        return true;
    }

    // The error for a locate that did not resolve to a deliverable point.
    FWebInjectOutcome LocateFailure(const FString& Handle, const FDriveWebLocateResult& Located)
    {
        FWebInjectOutcome Out;
        Out.Code = Located.Code.IsEmpty() ? FString(ErrorCodes::ERR_ACTION_FAILED) : Located.Code;
        if (Out.Code == ErrorCodes::ERR_TARGET_OCCLUDED)
        {
            Out.Details = MakeShared<FJsonObject>();
            Out.Details->SetStringField(TEXT("handle"), Handle);
            Out.Details->SetNumberField(TEXT("x"), Located.CssPoint.X);
            Out.Details->SetNumberField(TEXT("y"), Located.CssPoint.Y);
            Out.Details->SetStringField(TEXT("occluding_element"), Located.Occluder);
            Out.Message = Located.Occluder.IsEmpty()
                ? FString::Printf(TEXT("No page element is under element '%s''s center (%.0f, %.0f CSS px), so the input would land nowhere. Nothing was injected."),
                    *Handle, Located.CssPoint.X, Located.CssPoint.Y)
                : FString::Printf(TEXT("Element '%s' is covered at its center (%.0f, %.0f CSS px) by '%s', which would receive the input instead. Nothing was injected."),
                    *Handle, Located.CssPoint.X, Located.CssPoint.Y, *Located.Occluder);
            return Out;
        }
        Out.Message = FString::Printf(TEXT("Web action on '%s' did not apply (%s)%s%s"), *Handle, *Out.Code,
            Located.Detail.IsEmpty() ? TEXT(".") : TEXT(": "), *Located.Detail);
        return Out;
    }

    // The desktop point Slate input must hit to reach a located page point.
    FVector2D LocatedToScreen(UWebBrowser* Browser, const FDriveWebLocateResult& Located)
    {
        const FGeometry Geometry = Browser->GetCachedGeometry();
        const FVector2D Local = FDriveWebBridge::CssToLocal(
            Located.CssPoint, Located.CssViewport, FVector2D(Geometry.GetLocalSize()));
        return FVector2D(Geometry.LocalToAbsolute(Local));
    }

    // The Slate widget path a pointer event at ScreenPos is meant for, by Slate's own window order
    // and the window's hit-test grid. The platform's window-under-cursor is deliberately not
    // consulted: it follows a pointer warp only once the platform reports the enter (a frame or two
    // later on Linux), and under -RenderOffScreen's dummy SDL driver it never does, staying on
    // whichever window first took mouse focus.
    FWidgetPath SlatePathAt(const FVector2D& ScreenPos)
    {
        const TSharedPtr<SWindow> Window = FDriveInput::TopWindowAtPoint(ScreenPos);
        if (!Window.IsValid())
        {
            return FWidgetPath();
        }
        FSlateApplication& SlateApp = FSlateApplication::Get();
        TArray<FWidgetAndPointer> Bubble = Window->GetHittestGrid().GetBubblePath(
            ScreenPos, SlateApp.GetCursorRadius(), /*bIgnoreEnabledStatus=*/false, SlateApp.GetUserIndexForMouse());
        return FWidgetPath(Bubble);
    }

    bool PathReaches(const FWidgetPath& Path, const SWidget* Widget)
    {
        for (int32 Index = 0; Index < Path.Widgets.Num(); ++Index)
        {
            if (&Path.Widgets[Index].Widget.Get() == Widget)
            {
                return true;
            }
        }
        return false;
    }

    // Slate-level occlusion: the point belongs to another window, or to a widget drawn over the
    // browser inside its own window.
    FWebInjectOutcome SlateOccluded(const FString& Handle, const FVector2D& ScreenPos, const FWidgetPath& Path,
        const TSharedPtr<SWindow>& BrowserWindow)
    {
        FWebInjectOutcome Out;
        Out.Code = ErrorCodes::ERR_TARGET_OCCLUDED;
        Out.Details = MakeShared<FJsonObject>();
        Out.Details->SetStringField(TEXT("handle"), Handle);
        Out.Details->SetNumberField(TEXT("x"), ScreenPos.X);
        Out.Details->SetNumberField(TEXT("y"), ScreenPos.Y);
        const TSharedPtr<SWindow> Under = Path.IsValid() ? TSharedPtr<SWindow>(Path.GetWindow()) : nullptr;
        if (!Under.IsValid())
        {
            Out.Message = FString::Printf(TEXT("No window accepts pointer input at element '%s''s center (%.0f, %.0f), so the input would land nowhere."),
                *Handle, ScreenPos.X, ScreenPos.Y);
        }
        else if (Under != BrowserWindow)
        {
            const FString Title = Under->GetTitle().ToString();
            Out.Details->SetStringField(TEXT("occluding_window"), Title);
            Out.Message = FString::Printf(TEXT("Element '%s' is covered at (%.0f, %.0f) by window '%s', which would receive the input instead. Close or move that window (drive.list_windows lists it), then retry."),
                *Handle, ScreenPos.X, ScreenPos.Y, *Title);
        }
        else
        {
            const FString Widget = Path.Widgets.Last().Widget->GetTypeAsString();
            Out.Details->SetStringField(TEXT("occluding_widget"), Widget);
            Out.Message = FString::Printf(TEXT("Element '%s' is covered at (%.0f, %.0f) by a %s drawn over the web browser, which would receive the input instead."),
                *Handle, ScreenPos.X, ScreenPos.Y, *Widget);
        }
        return Out;
    }

    // Real pointer input routed along one Slate path: FSlateApplication's Route* entry points, the
    // same routing ProcessMouse* performs once it has located the window, so the browser widget
    // forwards it to CEF exactly as it does a physical mouse. The platform cursor is moved along so
    // Slate's later synthesized moves agree with where the input went.
    class FWebPointer
    {
    public:
        explicit FWebPointer(const FWidgetPath& InPath)
            : SlateApp(FSlateApplication::Get())
            , Path(InPath)
            , bPrevHandleInactive(SlateApp.GetHandleDeviceInputWhenApplicationNotActive())
        {
            // Without this ProcessReply skips the mouse capture the browser takes on press.
            SlateApp.SetHandleDeviceInputWhenApplicationNotActive(true);
        }
        ~FWebPointer()
        {
            SlateApp.SetHandleDeviceInputWhenApplicationNotActive(bPrevHandleInactive);
        }

        void Move(const FVector2D& To, const TSet<FKey>& Pressed = TSet<FKey>())
        {
            if (TSharedPtr<ICursor> Cursor = SlateApp.GetPlatformCursor())
            {
                Cursor->SetPosition(FMath::RoundToInt(To.X), FMath::RoundToInt(To.Y));
            }
            SlateApp.RoutePointerMoveEvent(Path, Event(To, Pressed, EKeys::Invalid), /*bIsSynthetic=*/false);
            Last = To;
            bMoved = true;
        }
        void Press(const FKey& Button)
        {
            SlateApp.RoutePointerDownEvent(Path, Event(Last, TSet<FKey>({ Button }), Button));
        }
        void Release(const FKey& Button)
        {
            SlateApp.RoutePointerUpEvent(Path, Event(Last, TSet<FKey>(), Button));
        }
        void Wheel(float Delta)
        {
            SlateApp.RouteMouseWheelOrGestureEvent(Path, Event(Last, TSet<FKey>(), EKeys::Invalid, Delta), nullptr);
        }

    private:
        FPointerEvent Event(const FVector2D& Pos, const TSet<FKey>& Pressed, const FKey& Effecting, float WheelDelta = 0.0f) const
        {
            return FPointerEvent(static_cast<uint32>(SlateApp.GetUserIndexForMouse()), FSlateApplication::CursorPointerIndex,
                Pos, bMoved ? Last : Pos, Pressed, Effecting, WheelDelta, SlateApp.GetModifierKeys());
        }

        FSlateApplication& SlateApp;
        const FWidgetPath& Path;
        const bool bPrevHandleInactive;
        // Every caller moves before it presses, releases or wheels, so Last is the pointer position.
        FVector2D Last = FVector2D::ZeroVector;
        bool bMoved = false;
    };

    // Deliver pointer input at a located element: map its CSS center to the desktop, require that
    // Slate's hit-test there reaches the browser widget (else TARGET_OCCLUDED, nothing pressed), then
    // Fire the input along that path.
    void RouteToBrowser(UWebBrowser* Browser, const FString& Handle, const FDriveWebLocateResult& Located,
        TFunction<void(FWebPointer&, const FVector2D&)> Fire, FWebInjectDone Done)
    {
        const TSharedPtr<SWidget> BrowserWidget = Browser->GetCachedWidget();
        if (!BrowserWidget.IsValid() || !FSlateApplication::IsInitialized())
        {
            Done({ ErrorCodes::ERR_INPUT_FAILED, TEXT("The web browser has no live Slate widget to route input to."), nullptr });
            return;
        }
        const FVector2D ScreenPos = LocatedToScreen(Browser, Located);
        const FWidgetPath Path = SlatePathAt(ScreenPos);
        if (!PathReaches(Path, BrowserWidget.Get()))
        {
            Done(SlateOccluded(Handle, ScreenPos, Path,
                FSlateApplication::Get().FindWidgetWindow(BrowserWidget.ToSharedRef())));
            return;
        }
        {
            FWebPointer Pointer(Path);
            Fire(Pointer, ScreenPos);
        }
        Done(FWebInjectOutcome());
    }

    // Locate Handle in the page, then route pointer input to it (see RouteToBrowser).
    void LocateAndRoute(UWebBrowser* Browser, const FString& Handle,
        TFunction<void(FWebPointer&, const FVector2D&)> Fire, FWebInjectDone Done)
    {
        const TWeakObjectPtr<UWebBrowser> WeakBrowser(Browser);
        FDriveWebBridge::LocateElement(Browser, Handle, /*bFocus=*/false,
            [WeakBrowser, Handle, Fire = MoveTemp(Fire), Done = MoveTemp(Done)](const FDriveWebLocateResult& Located)
            {
                UWebBrowser* Live = WeakBrowser.Get();
                if (!Located.bOk || !Live)
                {
                    Done(Live ? LocateFailure(Handle, Located)
                        : FWebInjectOutcome{ ErrorCodes::ERR_WEB_BROWSER_NOT_FOUND, TEXT("The web browser went away mid-action."), nullptr });
                    return;
                }
                RouteToBrowser(Live, Handle, Located, Fire, Done);
            },
            GQueryTimeoutSeconds);
    }

    // Per-action settle state: the latest DOM sample and the shared settle driver, which is stepped
    // once per completed DOM query instead of once per frame (a DOM sample is a CEF round-trip).
    struct FWebSettle
    {
        TSharedPtr<FDriveSettleDriver> Driver;
        TArray<FDriveElement> Latest;
        bool bQueryInFlight = false;
    };

    // The game/editor settle model on the DOM: FDriveSettleDriver's change-then-stable decision with
    // stable_ticks / quiet_budget_ms / settle_budget_ms, or wait_for until timeout_ms, against the
    // pre-action Baseline. Resolves Token with the same { outcome, changed, settled, condition_met,
    // elapsed_ms, ticks, input_path, diff, observation?, journal? } shape.
    void StartWebSettle(const TSharedRef<FAsyncResponseToken>& Token, const TWeakObjectPtr<UWebBrowser>& WeakBrowser,
        const FWebActionParams& Params, const TArray<FDriveElement>& Baseline)
    {
        TSharedRef<FWebSettle> Settle = MakeShared<FWebSettle>();
        Settle->Latest = Baseline;
        // The driver is owned by Settle, so its callbacks read Settle through a raw pointer
        // (a shared capture would be a reference cycle).
        FWebSettle* Raw = &Settle.Get();

        FDriveSettleDriver::FIsWaitForMet IsWaitForMet = nullptr;
        if (Params.Config.WaitFor.IsSet())
        {
            const FDriveCondition Condition = Params.Config.WaitFor.GetValue();
            IsWaitForMet = [Raw, Condition]() { return EvaluateOverElements(Condition, Raw->Latest); };
        }

        Settle->Driver = FDriveSettleDriver::CreateForStep(Params.Config,
            [Raw]() { return Raw->Latest; },
            MoveTemp(IsWaitForMet),
            [Token, Params, Baseline](const FDriveSettleResult& Result, const TArray<FDriveElement>& Final)
            {
                TSharedPtr<FJsonObject> Resp = FDriveActionCommon::WriteSettleResult(
                    Result, FDriveChangeDetector::Diff(Baseline, Final), Params.bFullDiff);
                Resp->SetStringField(TEXT("input_path"), Params.InputPath);
                if (TSharedPtr<FJsonObject> Observation = BuildWebObservationJson(Final, Params.ObserveMode, Params.MarkCap))
                {
                    Resp->SetObjectField(TEXT("observation"), Observation);
                }
                FDriveActionCommon::MaybeAttachJournal(Resp, Params.bIncludeJournal, Params.JournalSince);
                Token->SendSuccess(Resp);
            },
            FPlatformTime::Seconds());

        FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Settle, WeakBrowser](float) -> bool
        {
            if (Settle->Driver->IsComplete())
            {
                return false;
            }
            if (Settle->bQueryInFlight)
            {
                return true;
            }
            UWebBrowser* Browser = WeakBrowser.Get();
            if (!Browser)
            {
                // Nothing left to sample; step on the last DOM so the budgets still end the settle.
                Settle->Driver->Tick(FPlatformTime::Seconds());
                return !Settle->Driver->IsComplete();
            }
            Settle->bQueryInFlight = true;
            FDriveWebBridge::QueryElements(Browser,
                [Settle](bool bOk, TArray<FDriveElement> Elements)
                {
                    Settle->bQueryInFlight = false;
                    if (bOk)
                    {
                        Settle->Latest = MoveTemp(Elements);
                    }
                    // A failed sample still steps the driver (on the last DOM) so a browser that
                    // stops answering cannot hold the request open past its budgets.
                    Settle->Driver->Tick(FPlatformTime::Seconds());
                },
                GQueryTimeoutSeconds);
            return true;
        }), 0.0f);
    }

    // Shared web action flow: pre-action DOM baseline, the widget_absent guard, Inject, then the
    // settle. An injection refusal/failure resolves Token with its own code and nothing settles.
    void RunWebAction(FHandlerContext& Ctx, UWebBrowser* Browser, const FWebActionParams& Params, FWebInject Inject)
    {
        TSharedRef<FAsyncResponseToken> Token = Ctx.MakeAsyncToken();
        const TWeakObjectPtr<UWebBrowser> WeakBrowser(Browser);
        FDriveWebBridge::QueryElements(Browser,
            [Token, WeakBrowser, Params, Inject = MoveTemp(Inject)](bool bBaselineOk, TArray<FDriveElement> Baseline)
            {
                if (!bBaselineOk)
                {
                    Baseline.Reset();
                }
                if (FDriveActionCommon::IsAbsenceUnverifiable(Params.Config.WaitFor, Baseline))
                {
                    Token->SendError(ErrorCodes::ERR_CONDITION_INVALID,
                        FString::Printf(
                            TEXT("wait_for widget_absent target '%s' matches no element before the action (%d observed), so its absence would prove nothing."),
                            *Params.Config.WaitFor->Target, Baseline.Num()));
                    return;
                }
                UWebBrowser* Live = WeakBrowser.Get();
                if (!Live)
                {
                    Token->SendError(ErrorCodes::ERR_WEB_BROWSER_NOT_FOUND, TEXT("The web browser went away mid-action."));
                    return;
                }
                Inject(Live, [Token, WeakBrowser, Params, Baseline](const FWebInjectOutcome& Outcome)
                {
                    if (!Outcome.Code.IsEmpty())
                    {
                        Token->SendError(Outcome.Code, Outcome.Message, Outcome.Details);
                        return;
                    }
                    StartWebSettle(Token, WeakBrowser, Params, Baseline);
                });
            },
            GQueryTimeoutSeconds);
    }

    // Parse a "+"-separated modifier list (shift/ctrl/alt/cmd, with control/meta/win aliases).
    EDriveModifierKeys ParseWebModifiers(const FString& Spec)
    {
        EDriveModifierKeys Result = EDriveModifierKeys::None;
        TArray<FString> Tokens;
        Spec.ParseIntoArray(Tokens, TEXT("+"), /*CullEmpty=*/true);
        for (FString Token : Tokens)
        {
            Token.TrimStartAndEndInline();
            if (Token.Equals(TEXT("shift"), ESearchCase::IgnoreCase)) { Result |= EDriveModifierKeys::Shift; }
            else if (Token.Equals(TEXT("ctrl"), ESearchCase::IgnoreCase) || Token.Equals(TEXT("control"), ESearchCase::IgnoreCase)) { Result |= EDriveModifierKeys::Ctrl; }
            else if (Token.Equals(TEXT("alt"), ESearchCase::IgnoreCase)) { Result |= EDriveModifierKeys::Alt; }
            else if (Token.Equals(TEXT("cmd"), ESearchCase::IgnoreCase) || Token.Equals(TEXT("meta"), ESearchCase::IgnoreCase) || Token.Equals(TEXT("win"), ESearchCase::IgnoreCase)) { Result |= EDriveModifierKeys::Cmd; }
        }
        return Result;
    }

    // The SWebBrowserView under a UWebBrowser's widget: the widget that forwards keys to CEF.
    // SWebBrowser itself takes keyboard focus and only hands it on to its view from UE 5.8
    // (SWebBrowser::OnFocusReceived); before that, keys focused on SWebBrowser never reach the page.
    TSharedPtr<SWidget> FindBrowserView(const TSharedPtr<SWidget>& Widget)
    {
        if (!Widget.IsValid() || Widget->GetType() == FName(TEXT("SWebBrowserView")))
        {
            return Widget;
        }
        FChildren* Children = Widget->GetChildren();
        for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
        {
            if (const TSharedPtr<SWidget> Found = FindBrowserView(Children->GetChildAt(Index)))
            {
                return Found;
            }
        }
        return nullptr;
    }

    // Give the browser view Slate keyboard focus (which also focuses its CEF host), then send the
    // key the way a keyboard does: key down, the character the platform delivers with it, key up.
    bool SendWebKey(UWebBrowser* Browser, const FKey& Key, TCHAR Char, EDriveModifierKeys Modifiers, EDriveKeyAction Action)
    {
        if (!FSlateApplication::IsInitialized() || !Browser->GetCachedWidget().IsValid())
        {
            return false;
        }
        FSlateApplication& SlateApp = FSlateApplication::Get();
        TSharedPtr<SWidget> View = FindBrowserView(Browser->GetCachedWidget());
#if UE_VERSION_OLDER_THAN(5, 4, 0)
        // SWebBrowserView::OnFocusReceived, which hands focus on to the inner viewport that forwards
        // keys to CEF, arrived in 5.4; through 5.3 focus that viewport (the view's only child) directly.
        if (View.IsValid() && View->GetChildren() && View->GetChildren()->Num() > 0)
        {
            View = View->GetChildren()->GetChildAt(0);
        }
#endif
        SlateApp.SetKeyboardFocus(View.IsValid() ? View : Browser->GetCachedWidget(), EFocusCause::SetDirectly);

        if (Action != EDriveKeyAction::Up && Key.IsValid())
        {
            FDriveInput::PressKey(Key, Modifiers, EDriveKeyAction::Down);
        }
        // A chord with Ctrl/Alt/Cmd carries no text, exactly as on a real keyboard.
        if (Action != EDriveKeyAction::Up && Char != 0
            && !EnumHasAnyFlags(Modifiers, EDriveModifierKeys::Ctrl | EDriveModifierKeys::Alt | EDriveModifierKeys::Cmd))
        {
            const bool bPrevHandleInactive = SlateApp.GetHandleDeviceInputWhenApplicationNotActive();
            SlateApp.SetHandleDeviceInputWhenApplicationNotActive(true);
            const bool bShift = EnumHasAnyFlags(Modifiers, EDriveModifierKeys::Shift);
            FCharacterEvent CharEvent(Char, FModifierKeysState(bShift, false, false, false, false, false, false, false, false),
                static_cast<uint32>(SlateApp.GetUserIndexForKeyboard()), /*bIsRepeat=*/false);
            SlateApp.ProcessKeyCharEvent(CharEvent);
            SlateApp.SetHandleDeviceInputWhenApplicationNotActive(bPrevHandleInactive);
        }
        if (Action != EDriveKeyAction::Down && Key.IsValid())
        {
            FDriveInput::PressKey(Key, Modifiers, EDriveKeyAction::Up);
        }
        return true;
    }

    // Shared per-wait state for WaitForWeb, owned via TSharedPtr by the ticker delegate and the
    // in-flight query callback. Re-selects the browser by index each poll so a browser that
    // disappears mid-wait can't dangle.
    struct FWebWaitState
    {
        FWebWaitState(const TSharedRef<FAsyncResponseToken>& InToken, int32 InBrowserIndex,
            const FDriveCondition& InCondition, EDriveObserveMode InObserveMode, int32 InMarkCap,
            double InStartSeconds, double InDeadlineSeconds)
            : Token(InToken)
            , BrowserIndex(InBrowserIndex)
            , Condition(InCondition)
            , ObserveMode(InObserveMode)
            , MarkCap(InMarkCap)
            , StartSeconds(InStartSeconds)
            , DeadlineSeconds(InDeadlineSeconds)
        {
        }

        TSharedRef<FAsyncResponseToken> Token;
        int32 BrowserIndex = 0;
        FDriveCondition Condition;
        EDriveObserveMode ObserveMode = EDriveObserveMode::ListAndScreenshot;
        int32 MarkCap = 50;
        double StartSeconds = 0.0;
        double DeadlineSeconds = 0.0;

        // Guards against overlapping polls and double-resolution.
        bool bQueryInFlight = false;
        bool bResolved = false;

        // Last successfully-queried elements, attached to the resolved observation (so even a
        // timed-out wait reports the most recent DOM it saw).
        TArray<FDriveElement> LastElements;
        bool bHaveLast = false;

        FTSTicker::FDelegateHandle TickerHandle;
    };

    // Resolve a wait with { met, elapsed_ms, matched?, observation? } and tear down the ticker.
    // Idempotent. `matched` is the single matched element (small) when the condition is met and
    // had an element target; the full observation is opt-in (ObserveMode defaults to none).
    void ResolveWebWait(const TSharedPtr<FWebWaitState>& State, bool bMet)
    {
        if (!State.IsValid() || State->bResolved)
        {
            return;
        }
        State->bResolved = true;

        if (State->TickerHandle.IsValid())
        {
            FTSTicker::GetCoreTicker().RemoveTicker(State->TickerHandle);
            State->TickerHandle.Reset();
        }

        const double ElapsedMs = (FPlatformTime::Seconds() - State->StartSeconds) * 1000.0;

        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetBoolField(TEXT("met"), bMet);
        Resp->SetNumberField(TEXT("elapsed_ms"), static_cast<double>(static_cast<int64>(ElapsedMs)));

        // When met, return the single matched element instead of the whole DOM. Journal
        // conditions have no matching element, so `matched` is simply omitted for them.
        if (bMet && State->bHaveLast && !State->Condition.Target.IsEmpty())
        {
            for (const FDriveElement& Element : State->LastElements)
            {
                if (FDriveConditionEval::ElementMatchesTarget(Element, State->Condition.Target))
                {
                    Resp->SetObjectField(TEXT("matched"), FDriveJson::WriteElement(Element));
                    break;
                }
            }
        }

        if (State->bHaveLast)
        {
            if (TSharedPtr<FJsonObject> Observation =
                    BuildWebObservationJson(State->LastElements, State->ObserveMode, State->MarkCap))
            {
                Resp->SetObjectField(TEXT("observation"), Observation);
            }
        }

        State->Token->SendSuccess(Resp);
    }

    // One poll tick: returns false to unregister the ticker once terminal. Fires at most one
    // DOM query at a time; re-selects the browser by index so a vanished browser is tolerated
    // as a transient miss (the wait still times out cleanly).
    bool WebWaitTick(const TSharedPtr<FWebWaitState>& State)
    {
        if (!State.IsValid() || State->bResolved)
        {
            return false;
        }

        if (FPlatformTime::Seconds() >= State->DeadlineSeconds)
        {
            ResolveWebWait(State, /*bMet=*/false);
            return false;
        }

        if (State->bQueryInFlight)
        {
            return true;
        }

        UWebBrowser* Browser = FDriveWebBridge::SelectBrowser(State->BrowserIndex);
        if (!Browser)
        {
            // Browser not (yet) live: keep polling until the deadline.
            return true;
        }

        State->bQueryInFlight = true;
        FDriveWebBridge::QueryElements(Browser,
            [State](bool bOk, TArray<FDriveElement> Elements)
            {
                if (!State.IsValid() || State->bResolved)
                {
                    return;
                }
                State->bQueryInFlight = false;
                if (!bOk)
                {
                    // Transient query failure; the next tick retries until the deadline.
                    return;
                }

                const bool bMet = EvaluateOverElements(State->Condition, Elements);
                State->LastElements = MoveTemp(Elements);
                State->bHaveLast = true;

                if (bMet)
                {
                    ResolveWebWait(State, /*bMet=*/true);
                }
            },
            GQueryTimeoutSeconds);

        return true;
    }
}

using namespace DriveWebHandlersLocal;

void FDriveWebHandlers::ObserveWeb(FHandlerContext& Ctx)
{
    const int32 BrowserIndex = Ctx.GetInt(TEXT("browser_index"), 0);
    UWebBrowser* Browser = FDriveWebBridge::SelectBrowser(BrowserIndex);
    if (!Browser)
    {
        SendNoBrowser(Ctx, BrowserIndex);
        return;
    }

    const bool bScreenshot = Ctx.GetBool(TEXT("screenshot"), true);
    const bool bScreenshotToFile = FDriveHandlerCommon::ParseScreenshotToFile(Ctx);
    const int32 MarkCap = Ctx.GetInt(TEXT("mark_cap"), 50);
    const bool bInteractablesOnly = Ctx.GetBool(TEXT("interactables_only"), false);
    const int32 MaxElements = Ctx.GetInt(TEXT("max_elements"), 0);
    const int32 MaxBytes = Ctx.GetInt(TEXT("max_bytes"), 0);
    const bool bIncludeJournal = Ctx.GetBool(TEXT("include_journal"), false);
    const int32 SinceRaw = Ctx.GetInt(TEXT("journal_since"), 0);
    const uint64 JournalSince = SinceRaw > 0 ? static_cast<uint64>(SinceRaw) : 0;

    TSharedRef<FAsyncResponseToken> Token = Ctx.MakeAsyncToken();

    FDriveWebBridge::QueryElements(Browser,
        [Token, bScreenshot, bScreenshotToFile, MarkCap, bInteractablesOnly, MaxElements, MaxBytes, bIncludeJournal, JournalSince]
        (bool bOk, TArray<FDriveElement> Elements)
        {
            if (!bOk)
            {
                Token->SendError(ErrorCodes::ERR_WEB_QUERY_FAILED,
                    TEXT("The web DOM query did not return (GetSource DOM-poll timed out or "
                         "the page reported a JS error)."));
                return;
            }

            FDriveObservation Observation;
            Observation.Surface = EDriveSurface::Web;
            Observation.Elements = MoveTemp(Elements);

            // Compact the list before the screenshot reads it, mirroring the game/editor
            // observe path (interactables_only filter, then the max_elements cap, then the
            // max_bytes serialized-size cap).
            FDriveHandlerCommon::FilterObservationElements(
                Observation.Elements, bInteractablesOnly, MaxElements, MaxBytes, Observation.OmittedCount);

            // Set-of-Mark screenshot is best-effort and captures the GAME viewport (the CEF HUD
            // composites into it via OSR); a capture miss just leaves the screenshot unset.
            if (bScreenshot)
            {
                FDriveScreenshot Screenshot;
                FString ScreenshotErrorCode;
                if (FDriveSetOfMarkRenderer::CaptureAnnotated(
                        Observation.Elements, MarkCap, Screenshot, ScreenshotErrorCode, EDriveSurface::Web,
                        FDriveWindowSelector(), bScreenshotToFile))
                {
                    Observation.Screenshot = MoveTemp(Screenshot);
                }
            }

            if (bIncludeJournal)
            {
                FDriveJournalDelta Delta;
                FDriveHandlerCommon::GetJournalDelta(JournalSince, Delta);
                Observation.Journal = MoveTemp(Delta);
            }

            Observation.Frame = static_cast<int64>(GFrameCounter);
            Observation.Timestamp = FDateTime::UtcNow();

            Token->SendSuccess(FDriveJson::WriteObservation(Observation));
        },
        GQueryTimeoutSeconds);
}

void FDriveWebHandlers::ExpectWeb(FHandlerContext& Ctx)
{
    // Self-contained condition parse so ExpectWeb evaluates exactly one queried DOM.
    FDriveCondition Condition;
    if (!FDriveJson::ParseCondition(Ctx.GetObject(TEXT("condition")), Condition))
    {
        Ctx.SendError(ErrorCodes::ERR_CONDITION_INVALID,
            TEXT("The 'condition' object is missing, has an unrecognized 'type', or has no 'target' (every type but journal_severity needs one)."));
        return;
    }

    const int32 BrowserIndex = Ctx.GetInt(TEXT("browser_index"), 0);
    UWebBrowser* Browser = FDriveWebBridge::SelectBrowser(BrowserIndex);
    if (!Browser)
    {
        SendNoBrowser(Ctx, BrowserIndex);
        return;
    }

    TSharedRef<FAsyncResponseToken> Token = Ctx.MakeAsyncToken();

    FDriveWebBridge::QueryElements(Browser,
        [Token, Condition](bool bOk, TArray<FDriveElement> Elements)
        {
            if (!bOk)
            {
                Token->SendError(ErrorCodes::ERR_WEB_QUERY_FAILED,
                    TEXT("The web DOM query did not return (GetSource DOM-poll timed out or "
                         "the page reported a JS error)."));
                return;
            }

            FDriveJournalDelta Journal;
            if (IsJournalCondition(Condition))
            {
                FDriveHandlerCommon::GetJournalDelta(0, Journal);
            }
            const FDriveConditionResult Result = FDriveConditionEval::Evaluate(Condition, Elements, Journal);

            TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
            Out->SetBoolField(TEXT("met"), Result.bMet);
            Out->SetStringField(TEXT("actual"), Result.Actual);
            Out->SetStringField(TEXT("expected"), Result.Expected);
            Out->SetStringField(TEXT("detail"), Result.Detail);
            Token->SendSuccess(Out);
        },
        GQueryTimeoutSeconds);
}

void FDriveWebHandlers::ClickWeb(FHandlerContext& Ctx)
{
    const FString Handle = Ctx.GetString(TEXT("handle"));
    if (Handle.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAM,
            TEXT("drive.click on surface=web requires a 'handle' from a drive.observe element."));
        return;
    }
    if (RefuseOsInput(Ctx))
    {
        return;
    }
    FWebActionParams Params;
    UWebBrowser* Browser = ReadWebAction(Ctx, TEXT("slate"), Params);
    if (!Browser)
    {
        return;
    }

    const EDriveMouseButton Button = FDriveInput::ParseMouseButton(Ctx.GetString(TEXT("button"), TEXT("left")));
    const FKey ButtonKey = Button == EDriveMouseButton::Right ? EKeys::RightMouseButton
        : Button == EDriveMouseButton::Middle ? EKeys::MiddleMouseButton : EKeys::LeftMouseButton;
    RunWebAction(Ctx, Browser, Params, [Handle, ButtonKey](UWebBrowser* InBrowser, FWebInjectDone Done)
    {
        LocateAndRoute(InBrowser, Handle,
            [Key = ButtonKey](FWebPointer& Pointer, const FVector2D& ScreenPos)
            {
                Pointer.Move(ScreenPos);
                Pointer.Press(Key);
                Pointer.Release(Key);
            },
            MoveTemp(Done));
    });
}

void FDriveWebHandlers::TypeWeb(FHandlerContext& Ctx)
{
    const FString Handle = Ctx.GetString(TEXT("handle"));
    if (Handle.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAM,
            TEXT("drive.type on surface=web requires a 'handle' from a drive.observe element."));
        return;
    }
    const FString Text = Ctx.GetString(TEXT("text"));
    FWebActionParams Params;
    UWebBrowser* Browser = ReadWebAction(Ctx, TEXT("dom"), Params);
    if (!Browser)
    {
        return;
    }

    RunWebAction(Ctx, Browser, Params, [Handle, Text](UWebBrowser* InBrowser, FWebInjectDone Done)
    {
        FDriveWebBridge::TypeIntoElement(InBrowser, Handle, Text,
            [Handle, Done = MoveTemp(Done)](bool bOk, FString Code)
            {
                if (bOk)
                {
                    Done(FWebInjectOutcome());
                    return;
                }
                const FString Failed = Code.IsEmpty() ? FString(ErrorCodes::ERR_ACTION_FAILED) : Code;
                Done({ Failed, FString::Printf(TEXT("Web action on '%s' did not apply (%s)."), *Handle, *Failed), nullptr });
            },
            GQueryTimeoutSeconds);
    });
}

void FDriveWebHandlers::ScrollWeb(FHandlerContext& Ctx)
{
    const FString Handle = Ctx.GetString(TEXT("handle"));
    if (Handle.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAM,
            TEXT("drive.scroll on surface=web requires a 'handle' from a drive.observe element."));
        return;
    }
    FWebActionParams Params;
    UWebBrowser* Browser = ReadWebAction(Ctx, TEXT("slate"), Params);
    if (!Browser)
    {
        return;
    }

    const float Delta = static_cast<float>(Ctx.GetNumber(TEXT("delta"), 1.0));
    RunWebAction(Ctx, Browser, Params, [Handle, Delta](UWebBrowser* InBrowser, FWebInjectDone Done)
    {
        LocateAndRoute(InBrowser, Handle,
            [Delta](FWebPointer& Pointer, const FVector2D& ScreenPos)
            {
                Pointer.Move(ScreenPos);
                Pointer.Wheel(Delta);
            },
            MoveTemp(Done));
    });
}

void FDriveWebHandlers::HoverWeb(FHandlerContext& Ctx)
{
    const FString Handle = Ctx.GetString(TEXT("handle"));
    if (Handle.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAM,
            TEXT("drive.hover on surface=web requires a 'handle' from a drive.observe element."));
        return;
    }
    if (RefuseOsInput(Ctx))
    {
        return;
    }
    FWebActionParams Params;
    UWebBrowser* Browser = ReadWebAction(Ctx, TEXT("slate"), Params);
    if (!Browser)
    {
        return;
    }

    RunWebAction(Ctx, Browser, Params, [Handle](UWebBrowser* InBrowser, FWebInjectDone Done)
    {
        LocateAndRoute(InBrowser, Handle,
            [](FWebPointer& Pointer, const FVector2D& ScreenPos) { Pointer.Move(ScreenPos); },
            MoveTemp(Done));
    });
}

void FDriveWebHandlers::KeyWeb(FHandlerContext& Ctx)
{
    const FString KeyName = Ctx.GetString(TEXT("key"));
    if (KeyName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAM,
            TEXT("drive.key on surface=web requires a 'key' (a DOM key name, e.g. Enter, "
                 "Escape, ArrowDown, a)."));
        return;
    }
    FKey Key;
    TCHAR Char = 0;
    bool bShift = false;
    if (!FDriveWebBridge::MapDomKey(KeyName, Key, Char, bShift))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_KEY,
            FString::Printf(TEXT("Unknown key name '%s' (expected a DOM key value such as Enter, ArrowDown or a, or an FKey name)."), *KeyName));
        return;
    }
    FWebActionParams Params;
    UWebBrowser* Browser = ReadWebAction(Ctx, TEXT("slate"), Params);
    if (!Browser)
    {
        return;
    }

    // Handle is optional: with it the element is hit-tested and DOM-focused (no click) before the
    // key; without it the key goes to the page's focused element.
    const FString Handle = Ctx.GetString(TEXT("handle"));
    EDriveModifierKeys Modifiers = ParseWebModifiers(Ctx.GetString(TEXT("modifiers")));
    if (bShift)
    {
        Modifiers |= EDriveModifierKeys::Shift;
    }
    const FString ActionToken = Ctx.GetString(TEXT("action"), TEXT("press"));
    const EDriveKeyAction Action = ActionToken.Equals(TEXT("down"), ESearchCase::IgnoreCase) ? EDriveKeyAction::Down
        : ActionToken.Equals(TEXT("up"), ESearchCase::IgnoreCase) ? EDriveKeyAction::Up
        : EDriveKeyAction::Press;

    RunWebAction(Ctx, Browser, Params, [Handle, Key, Char, Modifiers, Action](UWebBrowser* InBrowser, FWebInjectDone Done)
    {
        auto Send = [Key, Char, Modifiers, Action](UWebBrowser* Live, const FWebInjectDone& InDone)
        {
            if (SendWebKey(Live, Key, Char, Modifiers, Action))
            {
                InDone(FWebInjectOutcome());
            }
            else
            {
                InDone({ ErrorCodes::ERR_INPUT_FAILED, TEXT("Slate key injection failed (no live browser widget)."), nullptr });
            }
        };
        if (Handle.IsEmpty())
        {
            Send(InBrowser, Done);
            return;
        }
        const TWeakObjectPtr<UWebBrowser> WeakBrowser(InBrowser);
        FDriveWebBridge::LocateElement(InBrowser, Handle, /*bFocus=*/true,
            [WeakBrowser, Handle, Send, Done = MoveTemp(Done)](const FDriveWebLocateResult& Located)
            {
                UWebBrowser* Live = WeakBrowser.Get();
                if (!Located.bOk || !Live)
                {
                    Done(Live ? LocateFailure(Handle, Located)
                        : FWebInjectOutcome{ ErrorCodes::ERR_WEB_BROWSER_NOT_FOUND, TEXT("The web browser went away mid-action."), nullptr });
                    return;
                }
                Send(Live, Done);
            },
            GQueryTimeoutSeconds);
    });
}

void FDriveWebHandlers::DragWeb(FHandlerContext& Ctx)
{
    const FString FromHandle = Ctx.GetString(TEXT("handle"));
    if (FromHandle.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAM,
            TEXT("drive.drag on surface=web requires a 'handle' for the element to drag from."));
        return;
    }
    if (RefuseOsInput(Ctx))
    {
        return;
    }

    // Web drag is DOM-element-to-element, so a drop target handle is required and the coordinate
    // to_x/to_y release point used on the game/editor surfaces has no web meaning.
    const FString ToHandle = Ctx.GetString(TEXT("to_handle"));
    if (ToHandle.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("drive.drag on surface=web requires a 'to_handle' (the drop-target element): "
                 "web drag is DOM-element-to-element, not coordinate-based, so to_x/to_y are not used."));
        return;
    }
    FWebActionParams Params;
    UWebBrowser* Browser = ReadWebAction(Ctx, TEXT("slate"), Params);
    if (!Browser)
    {
        return;
    }

    const int32 DurationMs = Ctx.GetInt(TEXT("duration_ms"), 200);
    RunWebAction(Ctx, Browser, Params, [FromHandle, ToHandle, DurationMs](UWebBrowser* InBrowser, FWebInjectDone Done)
    {
        // The release target is hit-tested too, then the press point is located and routed.
        const TWeakObjectPtr<UWebBrowser> WeakBrowser(InBrowser);
        FDriveWebBridge::LocateElement(InBrowser, ToHandle, /*bFocus=*/false,
            [WeakBrowser, FromHandle, ToHandle, DurationMs, Done = MoveTemp(Done)](const FDriveWebLocateResult& To)
            {
                UWebBrowser* Live = WeakBrowser.Get();
                if (!To.bOk || !Live)
                {
                    Done(Live ? LocateFailure(ToHandle, To)
                        : FWebInjectOutcome{ ErrorCodes::ERR_WEB_BROWSER_NOT_FOUND, TEXT("The web browser went away mid-action."), nullptr });
                    return;
                }
                const FVector2D ToScreen = LocatedToScreen(Live, To);
                LocateAndRoute(Live, FromHandle,
                    [ToScreen, DurationMs](FWebPointer& Pointer, const FVector2D& FromScreen)
                    {
                        // Press, the same interpolated moves the game surface's drag sends, release.
                        // The browser captures the mouse on press, so every later event reaches it.
                        Pointer.Move(FromScreen);
                        Pointer.Press(EKeys::LeftMouseButton);
                        for (const FVector2D& Step : FDriveInput::ComputeDragStepPoints(FromScreen, ToScreen, DurationMs))
                        {
                            Pointer.Move(Step, TSet<FKey>({ EKeys::LeftMouseButton }));
                        }
                        Pointer.Release(EKeys::LeftMouseButton);
                    },
                    Done);
            },
            GQueryTimeoutSeconds);
    });
}

void FDriveWebHandlers::WaitForWeb(FHandlerContext& Ctx)
{
    FDriveCondition Condition;
    if (!FDriveJson::ParseCondition(Ctx.GetObject(TEXT("condition")), Condition))
    {
        Ctx.SendError(ErrorCodes::ERR_CONDITION_INVALID,
            TEXT("The 'condition' object is missing, has an unrecognized 'type', or has no 'target' (every type but journal_severity needs one)."));
        return;
    }

    const int32 BrowserIndex = Ctx.GetInt(TEXT("browser_index"), 0);
    // Pre-check: with no live browser, surface this as a clean coded error now rather than
    // polling for the whole timeout.
    if (!FDriveWebBridge::SelectBrowser(BrowserIndex))
    {
        SendNoBrowser(Ctx, BrowserIndex);
        return;
    }

    const EDriveObserveMode ObserveMode = FDriveActionCommon::ParseObserveMode(Ctx);
    const int32 MarkCap = Ctx.GetInt(TEXT("mark_cap"), 50);
    const int32 TimeoutMs = Ctx.GetInt(TEXT("timeout_ms"), 5000);

    const double Now = FPlatformTime::Seconds();
    const double Deadline = Now + (TimeoutMs > 0 ? TimeoutMs : 5000) / 1000.0;

    TSharedRef<FAsyncResponseToken> Token = Ctx.MakeAsyncToken();

    TSharedPtr<FWebWaitState> State = MakeShared<FWebWaitState>(
        Token, BrowserIndex, Condition, ObserveMode, MarkCap, Now, Deadline);

    // Poll the DOM on a fixed interval; the ticker holds the only owning reference to State
    // through the delegate, and ResolveWebWait removes the ticker once terminal.
    State->TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([State](float /*DeltaTime*/) -> bool
        {
            return WebWaitTick(State);
        }),
        static_cast<float>(GWaitPollIntervalSeconds));
}
