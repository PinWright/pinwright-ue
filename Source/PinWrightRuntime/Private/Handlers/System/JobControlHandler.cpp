// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "State/PluginState.h"
#include "State/JobRegistry.h"

REGISTER_RPC_HANDLER("system.job_status", "system",
    "Return the status of a previously started long-running job",
    RPC_PARAMS(
        RPC_PARAM_REQ("ticket_id", "string", "Ticket id returned from the kickoff RPC")))
{
    FString Id;
    if (!Ctx.RequireString(TEXT("ticket_id"), Id)) return true;

    FJobTicket T;
    if (!FPluginState::Get().GetJobRegistry().Get(Id, T))
    {
        Ctx.SendError(ErrorCodes::ERR_TICKET_NOT_FOUND, FString::Printf(
            TEXT("No ticket with id '%s' (may have been evicted)"), *Id));
        return true;
    }
    Ctx.SendSuccess(FJobRegistry::ToJson(T));
    return true;
}

REGISTER_RPC_HANDLER("system.job_list", "system",
    "List active and recently-completed long-running jobs",
    RPC_NO_PARAMS)
{
    auto Out = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Items;
    for (const FJobTicket& T : FPluginState::Get().GetJobRegistry().List())
    {
        auto Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("ticket_id"), T.TicketId);
        Item->SetStringField(TEXT("method"), T.Method);
        Item->SetStringField(TEXT("status"), T.Status);
        Item->SetStringField(TEXT("started_at"), T.StartedAt.ToIso8601());
        Items.Add(MakeShared<FJsonValueObject>(Item));
    }
    Out->SetArrayField(TEXT("tickets"), Items);
    Ctx.SendSuccess(Out);
    return true;
}

REGISTER_RPC_HANDLER("system.job_cancel", "system",
    "Stop a running long-running job. Errors with JOB_CANCEL_UNSUPPORTED when the job's verb "
    "cannot be stopped, rather than reporting a cancellation that did not happen",
    RPC_PARAMS(
        RPC_PARAM_REQ("ticket_id", "string", "Ticket id to cancel")))
{
    FString Id;
    if (!Ctx.RequireString(TEXT("ticket_id"), Id)) return true;

    // Only the Requested branch is a success. The three others used to collapse into
    // {cancelled:<bool>} on a 200, which meant the one call an agent makes IN RESPONSE TO A HANG
    // answered "done" for a job that was still running — see ErrorCodes::ERR_JOB_CANCEL_UNSUPPORTED.
    switch (FPluginState::Get().GetJobRegistry().Cancel(Id))
    {
    case EJobCancelResult::Requested:
    {
        auto Out = MakeShared<FJsonObject>();
        Out->SetBoolField(TEXT("cancelled"), true);
        // Says what was proven, not what is hoped for. The verb's cancel hook has been invoked;
        // how promptly the work unwinds is up to that verb, so the ticket remains the source of
        // truth for "has it actually stopped".
        Out->SetStringField(TEXT("cancellation"), TEXT("requested"));
        Out->SetStringField(TEXT("ticket_id"), Id);
        Ctx.SendSuccess(Out);
        return true;
    }
    case EJobCancelResult::Unsupported:
        Ctx.SendError(ErrorCodes::ERR_JOB_CANCEL_UNSUPPORTED, FString::Printf(
            TEXT("Job '%s' is still running and CANNOT be cancelled: its verb registers no ")
            TEXT("cancellation hook, so nothing in the editor can stop the work. The ticket has ")
            TEXT("been left in 'running' precisely so it does not claim otherwise. The job will ")
            TEXT("run to completion and keep writing its output; poll system.job_status for the ")
            TEXT("real outcome, and do not treat the editor as idle until it reports a terminal ")
            TEXT("status."), *Id));
        return true;
    case EJobCancelResult::NotRunning:
        Ctx.SendError(ErrorCodes::ERR_JOB_NOT_RUNNING, FString::Printf(
            TEXT("Job '%s' already reached a terminal state, so there is nothing to cancel. ")
            TEXT("Call system.job_status for its result."), *Id));
        return true;
    case EJobCancelResult::NotFound:
    default:
        Ctx.SendError(ErrorCodes::ERR_TICKET_NOT_FOUND, FString::Printf(
            TEXT("No ticket with id '%s' (may have been evicted)"), *Id));
        return true;
    }
}
