// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"

#include "Handlers/Drive/DriveActionCommon.h"
#include "Handlers/Drive/DriveConditionEval.h"
#include "Handlers/Drive/DriveHandlerCommon.h"
#include "Handlers/Drive/DriveJson.h"
#include "Handlers/Drive/DriveSettleDriver.h"
#include "Handlers/Drive/DriveWebHandlers.h"

#include "Dom/JsonObject.h"

// drive.wait_for — poll a surface across engine frames until a condition holds or
// the timeout elapses. No input injection: it runs the settle driver in
// wait-for-only mode and resolves the request late via an async token. Mirrors the
// "no blind sleeps" loop used by the action verbs.
REGISTER_RPC_HANDLER("drive.wait_for", "drive",
    "Wait for a condition to hold on a live surface, polling each frame until met or timed out.",
    RPC_PARAMS(
        RPC_PARAM_DEF("surface", "string", "Surface to poll: game | editor_chrome | web | auto (default auto -> game).", "auto"),
        RPC_PARAM_OPT("instance_name", "string", "Live UMG root selector: substring-matched against the backing widget name."),
        RPC_PARAM_OPT("root_index", "integer", "Live UMG root selector: the Nth root (0-based). Takes precedence over instance_name."),
        RPC_PARAM_REQ("condition", "object", "Condition to wait for (type + target/expected_text/expected_count/count_op/expected_bounds/severity)."),
        RPC_PARAM_DEF("timeout_ms", "number", "How long to poll before timing out, in ms (default 5000).", "5000"),
        RPC_PARAM_DEF("stable_ticks", "number", "Consecutive stable ticks required before settled (carried for parity; default 2).", "2"),
        RPC_PARAM_DEF("quiet_budget_ms", "number", "No-change budget, in ms (carried for parity; default 500).", "500"),
        RPC_PARAM_DEF("settle_budget_ms", "number", "Overall settle budget, in ms (carried for parity; default 1500).", "1500"),
        RPC_PARAM_DEF("observe", "string", "Post-wait observation detail: none | list | list+screenshot (default none -> only the small `matched` element is returned).", "none"),
        RPC_PARAM_DEF("include_journal", "boolean", "Attach a top-level journal delta since journal_since (default false).", "false"),
        RPC_PARAM_DEF("mark_cap", "number", "Max interactables given a visible mark in the observation screenshot (default 50).", "50"),
        RPC_PARAM_DEF("journal_since", "number", "Cursor to read journal from when include_journal is set (default 0).", "0")
    ))
{
    FDriveCondition Condition;
    if (!FDriveJson::ParseCondition(Ctx.GetObject(TEXT("condition")), Condition))
    {
        Ctx.SendError(ErrorCodes::ERR_CONDITION_INVALID,
            TEXT("The 'condition' object is missing or has an unrecognized 'type'."));
        return true;
    }

    const EDriveSurface Surface = FDriveHandlerCommon::ResolveSurface(Ctx);

    // Web is fully asynchronous (the condition is polled against CEF DOM queries), so it
    // branches to the dedicated async path before the synchronous game/editor settle loop.
    // WaitForWeb re-parses the condition itself; the parse above still guards a malformed
    // condition for every surface.
    if (Surface == EDriveSurface::Web)
    {
        FDriveWebHandlers::WaitForWeb(Ctx);
        return true;
    }

    const FDriveRootSelector Selector = FDriveHandlerCommon::ParseRootSelector(Ctx);

    // Live-UI pre-check: with no PIE/viewport, sampling fails. Surface this as the
    // resolver's clean coded error now rather than polling for the whole timeout.
    {
        TArray<FDriveElement> Probe;
        FString RootName;
        FString ErrorCode;
        FString ErrorMessage;
        if (!FDriveHandlerCommon::GetElementsForSurface(Surface, Selector, Probe, RootName, ErrorCode, ErrorMessage))
        {
            Ctx.SendError(ErrorCode, ErrorMessage);
            return true;
        }
    }

    // Settle config: the budgets are read for parity, but in wait-for-only mode the
    // terminal states are the condition itself and its dedicated timeout.
    FDriveSettleConfig Config;
    FDriveJson::ParseSettleConfig(Ctx.GetRawPayload(), Config);
    Config.WaitFor = Condition;
    Config.WaitForTimeoutMs = Ctx.GetInt(TEXT("timeout_ms"), Config.WaitForTimeoutMs);

    const EDriveObserveMode ObserveMode = FDriveActionCommon::ParseObserveMode(Ctx);
    const int32 MarkCap = Ctx.GetInt(TEXT("mark_cap"), 50);
    const bool bIncludeJournal = Ctx.GetBool(TEXT("include_journal"), false);
    const int32 SinceRaw = Ctx.GetInt(TEXT("journal_since"), 0);
    const uint64 JournalSince = SinceRaw > 0 ? static_cast<uint64>(SinceRaw) : 0;

    TSharedRef<FAsyncResponseToken> Token = Ctx.MakeAsyncToken();

    FDriveSettleDriver::FOnComplete OnComplete =
        [Token, Surface, Selector, Condition, ObserveMode, MarkCap, bIncludeJournal, JournalSince]
        (const FDriveSettleResult& Result, const TArray<FDriveElement>& FinalElements)
        {
            TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
            Resp->SetBoolField(TEXT("met"), Result.bConditionMet);
            Resp->SetStringField(TEXT("outcome"), FDriveActionCommon::SettleOutcomeToString(Result.Outcome));
            Resp->SetNumberField(TEXT("elapsed_ms"), Result.ElapsedMs);
            Resp->SetNumberField(TEXT("ticks"), Result.Ticks);

            // When met, return the single matched element (small) instead of the whole list,
            // so the common case stays compact. Journal conditions have no matching element.
            if (Result.bConditionMet && !Condition.Target.IsEmpty())
            {
                for (const FDriveElement& Element : FinalElements)
                {
                    if (FDriveConditionEval::ElementMatchesTarget(Element, Condition.Target))
                    {
                        Resp->SetObjectField(TEXT("matched"), FDriveJson::WriteElement(Element));
                        break;
                    }
                }
            }

            // Full post-wait observation only when the caller opted in (observe != none).
            if (TSharedPtr<FJsonObject> Observation =
                    FDriveActionCommon::BuildObservationField(Surface, Selector, ObserveMode, MarkCap))
            {
                Resp->SetObjectField(TEXT("observation"), Observation);
            }

            FDriveActionCommon::MaybeAttachJournal(Resp, bIncludeJournal, JournalSince);

            Token->SendSuccess(Resp);
        };

    // The ticker started by Start() owns the driver until it terminates; drop the ref.
    FDriveSettleDriver::Start(
        Config,
        FDriveActionCommon::MakeGetElements(Surface, Selector),
        FDriveActionCommon::MakeIsWaitForMet(Surface, Selector, Condition),
        MoveTemp(OnComplete));
    return true;
}
