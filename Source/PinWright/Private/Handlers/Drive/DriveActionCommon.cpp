// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Drive/DriveActionCommon.h"

#include "Handlers/Drive/DriveConditionEval.h"
#include "Handlers/Drive/DriveFingerprint.h"
#include "Handlers/Drive/DriveHandlerCommon.h"
#include "Handlers/Drive/DriveInput.h"
#include "Handlers/Drive/DriveJson.h"
#include "Handlers/Drive/DriveLiveResolver.h"
#include "Handlers/Drive/DriveSettleDriver.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
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

    void SendOccluded(const FAsyncResponseToken& Token, const FString& Handle, const FVector2D& Point,
        const TSharedPtr<SWindow>& Under)
    {
        const FString Title = Under.IsValid() ? Under->GetTitle().ToString() : FString();
        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
        Details->SetStringField(TEXT("handle"), Handle);
        Details->SetNumberField(TEXT("x"), Point.X);
        Details->SetNumberField(TEXT("y"), Point.Y);
        if (Under.IsValid())
        {
            Details->SetStringField(TEXT("occluding_window"), Title);
        }
        Token.SendError(ErrorCodes::ERR_TARGET_OCCLUDED,
            Under.IsValid()
                ? FString::Printf(
                    TEXT("Element '%s' is covered at (%.0f, %.0f) by window '%s', which would receive the input instead. Close or move that window (drive.list_windows lists it), then retry."),
                    *Handle, Point.X, Point.Y, *Title)
                : FString::Printf(
                    TEXT("No window accepts pointer input at element '%s''s center (%.0f, %.0f), so the input would land nowhere."),
                    *Handle, Point.X, Point.Y),
            Details);
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
    case EDriveSettleOutcome::Continue:
    default:                                         return TEXT("continue");
    }
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
    const TCHAR* InputPath)
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
    if (!FDriveJson::ParseSettleConfig(Ctx.GetRawPayload(), Config))
    {
        Ctx.SendError(ErrorCodes::ERR_CONDITION_INVALID,
            TEXT("The 'wait_for' object has an unrecognized 'type'."));
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

    // Everything after a successful injection: run the settle loop and resolve the request
    // through Token once it reaches a terminal outcome.
    const auto StartSettle =
        [Config, Surface, Selector, WindowSelector, PreElements, ObserveMode, MarkCap, bIncludeJournal, JournalSince, bFullDiff, InputPathLabel]
        (const TSharedRef<FAsyncResponseToken>& Token)
    {
        FDriveSettleDriver::FIsWaitForMet IsWaitForMet = nullptr;
        if (Config.WaitFor.IsSet())
        {
            IsWaitForMet = MakeIsWaitForMet(Surface, Selector, Config.WaitFor.GetValue(), WindowSelector);
        }

        FDriveSettleDriver::FOnComplete OnComplete =
            [Token, Surface, Selector, WindowSelector, PreElements, ObserveMode, MarkCap, bIncludeJournal, JournalSince, bFullDiff, InputPathLabel]
            (const FDriveSettleResult& Result, const TArray<FDriveElement>& FinalElements)
            {
                TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
                Resp->SetStringField(TEXT("outcome"), SettleOutcomeToString(Result.Outcome));
                if (!InputPathLabel.IsEmpty())
                {
                    Resp->SetStringField(TEXT("input_path"), InputPathLabel);
                }
                Resp->SetBoolField(TEXT("changed"), Result.bChanged);
                Resp->SetBoolField(TEXT("settled"), Result.bSettled);
                Resp->SetBoolField(TEXT("condition_met"), Result.bConditionMet);
                Resp->SetNumberField(TEXT("elapsed_ms"), Result.ElapsedMs);
                Resp->SetNumberField(TEXT("ticks"), Result.Ticks);

                // Compact summary by default; full handle lists only when full_diff was set.
                const FDriveDiff Diff = FDriveChangeDetector::Diff(PreElements, FinalElements);
                Resp->SetObjectField(TEXT("diff"),
                    bFullDiff ? FDriveJson::WriteDiffFull(Diff) : FDriveJson::WriteDiffSummary(Diff));

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
    // the target's window. os_input is left alone: it never warps the Slate cursor, and the X
    // server routes its events itself. A retainer's virtual window never appears in that routing.
    const bool bGatePointer = TargetWindow.IsValid() && !TargetWindow->IsVirtualWindow()
        && InputPathLabel != TEXT("os_x11");
    if (bGatePointer)
    {
        FDriveInput::MoveTo(TargetCenter);
    }

    if (!bGatePointer || FDriveInput::WindowUnderPoint(TargetCenter) == TargetWindow)
    {
        // Inject the input. A failure here (Slate down mid-flight) is a clean error.
        if (!Inject(TargetCenter))
        {
            Ctx.SendError(ErrorCodes::ERR_INPUT_FAILED, InjectFailedMessage);
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
    const double Deadline = FPlatformTime::Seconds() + PointerRouteWaitSeconds;
    const TWeakPtr<SWindow> WeakTargetWindow = TargetWindow;
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
        [Token, WeakTargetWindow, TargetCenter, Deadline, Inject, StartSettle, Handle, InjectFailedMessage](float) -> bool
        {
            const TSharedPtr<SWindow> Target = WeakTargetWindow.Pin();
            if (!Target.IsValid())
            {
                Token->SendError(ErrorCodes::ERR_TARGET_CHANGED,
                    FString::Printf(TEXT("The window holding element '%s' closed before the input could be delivered."), *Handle));
                return false;
            }
            const TSharedPtr<SWindow> Under = FDriveInput::WindowUnderPoint(TargetCenter);
            if (Under == Target)
            {
                if (Inject(TargetCenter))
                {
                    StartSettle(Token);
                }
                else
                {
                    Token->SendError(ErrorCodes::ERR_INPUT_FAILED, InjectFailedMessage);
                }
                return false;
            }
            if (FPlatformTime::Seconds() < Deadline)
            {
                return true;
            }
            SendOccluded(*Token, Handle, TargetCenter, Under);
            return false;
        }));
}
