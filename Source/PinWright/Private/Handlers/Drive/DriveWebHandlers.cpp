// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Drive/DriveWebHandlers.h"

#include "Handlers/Drive/DriveActionCommon.h"     // EDriveObserveMode, ParseObserveMode
#include "Handlers/Drive/DriveConditionEval.h"
#include "Handlers/Drive/DriveFingerprint.h"        // FDriveDiff, FDriveChangeDetector
#include "Handlers/Drive/DriveHandlerCommon.h"      // GetJournalDelta
#include "Handlers/Drive/DriveJson.h"
#include "Handlers/Drive/DriveSetOfMarkRenderer.h"
#include "Handlers/Drive/DriveTypes.h"
#include "Handlers/Drive/DriveWebBridge.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"

class UWebBrowser;

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

    // Finish a web action (click/type): resolve { ok, code, diff, observation? }. The diff and
    // observation are attached only when the post-action re-observe succeeded. The diff defaults
    // to the compact summary (counts + 15-handle samples); bFullDiff swaps in the full lists.
    // The observation is opt-in (ObserveMode defaults to none).
    void FinishWebAction(
        const TSharedRef<FAsyncResponseToken>& Token,
        const FString& Code,
        bool bReObserveOk,
        const TArray<FDriveElement>& Baseline,
        const TArray<FDriveElement>& Final,
        EDriveObserveMode ObserveMode,
        int32 MarkCap,
        bool bFullDiff)
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetBoolField(TEXT("ok"), true);
        Resp->SetStringField(TEXT("code"), Code.IsEmpty() ? FString(TEXT("OK")) : Code);

        if (bReObserveOk)
        {
            const FDriveDiff Diff = FDriveChangeDetector::Diff(Baseline, Final);
            Resp->SetObjectField(TEXT("diff"),
                bFullDiff ? FDriveJson::WriteDiffFull(Diff) : FDriveJson::WriteDiffSummary(Diff));

            if (TSharedPtr<FJsonObject> Observation = BuildWebObservationJson(Final, ObserveMode, MarkCap))
            {
                Resp->SetObjectField(TEXT("observation"), Observation);
            }
        }

        Token->SendSuccess(Resp);
    }

    // The injection step of a web action: a click or a type, both expressed as one async
    // bridge call resolving (bOk, code). Click and type differ only in this lambda.
    using FWebInject = TFunction<void(UWebBrowser* Browser, TFunction<void(bool /*bOk*/, FString /*Code*/)> OnInjected)>;

    // Shared web action driver implementing the WEB SETTLE MODEL: query a pre-action baseline,
    // inject (click/type), then re-query the DOM exactly once and resolve { ok, code, diff,
    // observation }. A bridge injection failure resolves the token with the bridge's own code
    // (TARGET_NOT_FOUND / TARGET_CHANGED / ACTION_FAILED / TIMEOUT / MALFORMED_JSON). Browser is
    // captured for the bounded multi-hop round-trip.
    void RunWebActionWithSettle(
        const TSharedRef<FAsyncResponseToken>& Token,
        UWebBrowser* Browser,
        EDriveObserveMode ObserveMode,
        int32 MarkCap,
        bool bFullDiff,
        FWebInject Inject)
    {
        FDriveWebBridge::QueryElements(Browser,
            [Token, Browser, ObserveMode, MarkCap, bFullDiff, Inject = MoveTemp(Inject)]
            (bool bBaselineOk, TArray<FDriveElement> Baseline)
            {
                TArray<FDriveElement> BaselineElements = bBaselineOk ? MoveTemp(Baseline) : TArray<FDriveElement>();

                Inject(Browser,
                    [Token, Browser, ObserveMode, MarkCap, bFullDiff, BaselineElements](bool bInjectOk, FString Code)
                    {
                        if (!bInjectOk)
                        {
                            // Forward the bridge's specific code unchanged; ACTION_FAILED is a
                            // defensive fallback if the bridge somehow returned an empty code.
                            Token->SendError(
                                Code.IsEmpty() ? FString(ErrorCodes::ERR_ACTION_FAILED) : Code,
                                FString::Printf(TEXT("Web action did not apply (%s)."),
                                    Code.IsEmpty() ? TEXT("ACTION_FAILED") : *Code));
                            return;
                        }

                        // Single post-action re-observe (the web settle: inject then async
                        // re-query, NOT a per-tick fingerprint loop).
                        FDriveWebBridge::QueryElements(Browser,
                            [Token, Code, BaselineElements, ObserveMode, MarkCap, bFullDiff]
                            (bool bReObserveOk, TArray<FDriveElement> Final)
                            {
                                FinishWebAction(Token, Code, bReObserveOk, BaselineElements,
                                    Final, ObserveMode, MarkCap, bFullDiff);
                            },
                            GQueryTimeoutSeconds);
                    });
            },
            GQueryTimeoutSeconds);
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
            TEXT("The 'condition' object is missing or has an unrecognized 'type'."));
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

    const int32 BrowserIndex = Ctx.GetInt(TEXT("browser_index"), 0);
    UWebBrowser* Browser = FDriveWebBridge::SelectBrowser(BrowserIndex);
    if (!Browser)
    {
        SendNoBrowser(Ctx, BrowserIndex);
        return;
    }

    const EDriveObserveMode ObserveMode = FDriveActionCommon::ParseObserveMode(Ctx);
    const int32 MarkCap = Ctx.GetInt(TEXT("mark_cap"), 50);
    const bool bFullDiff = Ctx.GetBool(TEXT("full_diff"), false);

    TSharedRef<FAsyncResponseToken> Token = Ctx.MakeAsyncToken();

    RunWebActionWithSettle(Token, Browser, ObserveMode, MarkCap, bFullDiff,
        [Handle](UWebBrowser* InBrowser, TFunction<void(bool, FString)> OnInjected)
        {
            FDriveWebBridge::ClickElement(InBrowser, Handle, MoveTemp(OnInjected), GQueryTimeoutSeconds);
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

    const int32 BrowserIndex = Ctx.GetInt(TEXT("browser_index"), 0);
    UWebBrowser* Browser = FDriveWebBridge::SelectBrowser(BrowserIndex);
    if (!Browser)
    {
        SendNoBrowser(Ctx, BrowserIndex);
        return;
    }

    const EDriveObserveMode ObserveMode = FDriveActionCommon::ParseObserveMode(Ctx);
    const int32 MarkCap = Ctx.GetInt(TEXT("mark_cap"), 50);
    const bool bFullDiff = Ctx.GetBool(TEXT("full_diff"), false);

    TSharedRef<FAsyncResponseToken> Token = Ctx.MakeAsyncToken();

    RunWebActionWithSettle(Token, Browser, ObserveMode, MarkCap, bFullDiff,
        [Handle, Text](UWebBrowser* InBrowser, TFunction<void(bool, FString)> OnInjected)
        {
            FDriveWebBridge::TypeIntoElement(InBrowser, Handle, Text, MoveTemp(OnInjected), GQueryTimeoutSeconds);
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

    const int32 BrowserIndex = Ctx.GetInt(TEXT("browser_index"), 0);
    UWebBrowser* Browser = FDriveWebBridge::SelectBrowser(BrowserIndex);
    if (!Browser)
    {
        SendNoBrowser(Ctx, BrowserIndex);
        return;
    }

    const double Delta = Ctx.GetNumber(TEXT("delta"), 1.0);
    const EDriveObserveMode ObserveMode = FDriveActionCommon::ParseObserveMode(Ctx);
    const int32 MarkCap = Ctx.GetInt(TEXT("mark_cap"), 50);
    const bool bFullDiff = Ctx.GetBool(TEXT("full_diff"), false);

    TSharedRef<FAsyncResponseToken> Token = Ctx.MakeAsyncToken();

    RunWebActionWithSettle(Token, Browser, ObserveMode, MarkCap, bFullDiff,
        [Handle, Delta](UWebBrowser* InBrowser, TFunction<void(bool, FString)> OnInjected)
        {
            // The new bridge action methods take a void(bool, const FString&) callback. A
            // void(bool, FString) TFunction can't construct that directly (TFunction's templated
            // ctor is SFINAE-disabled for other TFunctions), so forward through a thin lambda.
            FDriveWebBridge::ScrollElement(InBrowser, Handle, Delta,
                [OnInjected = MoveTemp(OnInjected)](bool bOk, const FString& Code) { OnInjected(bOk, Code); },
                GQueryTimeoutSeconds);
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

    const int32 BrowserIndex = Ctx.GetInt(TEXT("browser_index"), 0);
    UWebBrowser* Browser = FDriveWebBridge::SelectBrowser(BrowserIndex);
    if (!Browser)
    {
        SendNoBrowser(Ctx, BrowserIndex);
        return;
    }

    const EDriveObserveMode ObserveMode = FDriveActionCommon::ParseObserveMode(Ctx);
    const int32 MarkCap = Ctx.GetInt(TEXT("mark_cap"), 50);
    const bool bFullDiff = Ctx.GetBool(TEXT("full_diff"), false);

    TSharedRef<FAsyncResponseToken> Token = Ctx.MakeAsyncToken();

    RunWebActionWithSettle(Token, Browser, ObserveMode, MarkCap, bFullDiff,
        [Handle](UWebBrowser* InBrowser, TFunction<void(bool, FString)> OnInjected)
        {
            // Forward the void(bool, FString) settle callback into the bridge's
            // void(bool, const FString&) callback through a thin lambda (see ScrollWeb).
            FDriveWebBridge::HoverElement(InBrowser, Handle,
                [OnInjected = MoveTemp(OnInjected)](bool bOk, const FString& Code) { OnInjected(bOk, Code); },
                GQueryTimeoutSeconds);
        });
}

void FDriveWebHandlers::KeyWeb(FHandlerContext& Ctx)
{
    const FString Key = Ctx.GetString(TEXT("key"));
    if (Key.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAM,
            TEXT("drive.key on surface=web requires a 'key' (a DOM key name, e.g. Enter, "
                 "Escape, ArrowDown, a)."));
        return;
    }

    const int32 BrowserIndex = Ctx.GetInt(TEXT("browser_index"), 0);
    UWebBrowser* Browser = FDriveWebBridge::SelectBrowser(BrowserIndex);
    if (!Browser)
    {
        SendNoBrowser(Ctx, BrowserIndex);
        return;
    }

    // Handle is optional on the web key verb: with it the bridge focuses that element by its
    // data-pw-id before the key, without it the key goes to the page's active element.
    // Modifiers/action ride as raw strings; the bridge maps them DOM-side (no native FKey).
    const FString Handle = Ctx.GetString(TEXT("handle"));
    const FString Modifiers = Ctx.GetString(TEXT("modifiers"));
    const FString Action = Ctx.GetString(TEXT("action"), TEXT("press"));

    const EDriveObserveMode ObserveMode = FDriveActionCommon::ParseObserveMode(Ctx);
    const int32 MarkCap = Ctx.GetInt(TEXT("mark_cap"), 50);
    const bool bFullDiff = Ctx.GetBool(TEXT("full_diff"), false);

    TSharedRef<FAsyncResponseToken> Token = Ctx.MakeAsyncToken();

    RunWebActionWithSettle(Token, Browser, ObserveMode, MarkCap, bFullDiff,
        [Handle, Key, Modifiers, Action](UWebBrowser* InBrowser, TFunction<void(bool, FString)> OnInjected)
        {
            // Forward the void(bool, FString) settle callback into the bridge's
            // void(bool, const FString&) callback through a thin lambda (see ScrollWeb).
            FDriveWebBridge::KeyElement(InBrowser, Handle, Key, Modifiers, Action,
                [OnInjected = MoveTemp(OnInjected)](bool bOk, const FString& Code) { OnInjected(bOk, Code); },
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

    // Web drag is DOM-element-to-element (the bridge dispatches the HTML5 drag sequence between
    // two data-pw-id handles), so a drop target handle is required and the coordinate to_x/to_y
    // release point used on the game/editor surfaces has no web meaning.
    const FString ToHandle = Ctx.GetString(TEXT("to_handle"));
    if (ToHandle.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("drive.drag on surface=web requires a 'to_handle' (the drop-target element): "
                 "web drag is DOM-element-to-element, not coordinate-based, so to_x/to_y are not used."));
        return;
    }

    const int32 BrowserIndex = Ctx.GetInt(TEXT("browser_index"), 0);
    UWebBrowser* Browser = FDriveWebBridge::SelectBrowser(BrowserIndex);
    if (!Browser)
    {
        SendNoBrowser(Ctx, BrowserIndex);
        return;
    }

    const EDriveObserveMode ObserveMode = FDriveActionCommon::ParseObserveMode(Ctx);
    const int32 MarkCap = Ctx.GetInt(TEXT("mark_cap"), 50);
    const bool bFullDiff = Ctx.GetBool(TEXT("full_diff"), false);

    TSharedRef<FAsyncResponseToken> Token = Ctx.MakeAsyncToken();

    RunWebActionWithSettle(Token, Browser, ObserveMode, MarkCap, bFullDiff,
        [FromHandle, ToHandle](UWebBrowser* InBrowser, TFunction<void(bool, FString)> OnInjected)
        {
            // Forward the void(bool, FString) settle callback into the bridge's
            // void(bool, const FString&) callback through a thin lambda (see ScrollWeb).
            FDriveWebBridge::DragElement(InBrowser, FromHandle, ToHandle,
                [OnInjected = MoveTemp(OnInjected)](bool bOk, const FString& Code) { OnInjected(bOk, Code); },
                GQueryTimeoutSeconds);
        });
}

void FDriveWebHandlers::WaitForWeb(FHandlerContext& Ctx)
{
    FDriveCondition Condition;
    if (!FDriveJson::ParseCondition(Ctx.GetObject(TEXT("condition")), Condition))
    {
        Ctx.SendError(ErrorCodes::ERR_CONDITION_INVALID,
            TEXT("The 'condition' object is missing or has an unrecognized 'type'."));
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
