// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "State/JobRegistry.h"

#include "Compat/EngineVersionCompat.h"
#include "Utils/JobMonitorLog.h"
#include "Misc/Guid.h"
#include "HAL/PlatformTime.h"

namespace
{
    void EmitEvent(FJobMonitorLog* Log, const FString& TicketId, const FString& Method,
                   const FString& Event, const TSharedPtr<FJsonObject>& Extra)
    {
        if (!Log) return;
        auto Line = MakeShared<FJsonObject>();
        Line->SetStringField(TEXT("ts"), FDateTime::UtcNow().ToIso8601());
        Line->SetStringField(TEXT("ticket_id"), TicketId);
        Line->SetStringField(TEXT("method"), Method);
        Line->SetStringField(TEXT("event"), Event);
        if (Extra.IsValid())
        {
            for (const auto& Pair : Extra->Values)
            {
                Line->SetField(Pair.Key, Pair.Value);
            }
        }
        Log->AppendEvent(Line);
    }
}

FJobRegistry::FJobRegistry(int32 InTtlSeconds, int32 InProgressMinIntervalMs, FJobMonitorLog* InMonitorLog)
    : TtlSeconds(InTtlSeconds)
    , ProgressMinIntervalMs(InProgressMinIntervalMs)
    , MonitorLog(InMonitorLog)
{
}

FString FJobRegistry::AllocateId(const FString& /*Method*/)
{
    const FString Stamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%dT%H%M%S"));
    // Right, not Left: on Linux FGuid is a UUIDv7 whose leading 32 bits are the
    // millisecond timestamp >> 16, so Left(8) repeats for ~65 s and two jobs started in
    // the same second shared one ticket (the later Start silently replaced the first).
    const FString Hex = FGuid::NewGuid().ToString(EGuidFormats::DigitsLower).Right(8);
    return FString::Printf(TEXT("j_%s_%s"), *Stamp, *Hex);
}

FString FJobRegistry::Start(const FString& Method, const TSharedRef<FJsonObject>& Params)
{
    FJobTicket Ticket;
    Ticket.TicketId = AllocateId(Method);
    Ticket.Method = Method;
    Ticket.Status = TEXT("running");
    Ticket.StartedAt = FDateTime::UtcNow();

    {
        FScopeLock Lock(&Mutex);
        Tickets.Add(Ticket.TicketId, Ticket);
    }

    auto Extra = MakeShared<FJsonObject>();
    Extra->SetObjectField(TEXT("params"), Params);
    EmitEvent(MonitorLog, Ticket.TicketId, Method, TEXT("started"), Extra);
    OnJobEventDelegate.Broadcast(Ticket.TicketId, TEXT("started"), Extra, nullptr);
    return Ticket.TicketId;
}

void FJobRegistry::SetCancelCallback(const FString& TicketId, TFunction<void()> Callback)
{
    FScopeLock Lock(&Mutex);
    if (FJobTicket* T = Tickets.Find(TicketId))
    {
        T->CancelCallback = MoveTemp(Callback);
    }
}

bool FJobRegistry::RecordProgress(const FString& TicketId, const FString& Message,
                                  const TSharedPtr<FJsonObject>& Payload, bool bBypassRateLimit)
{
    FString Method;
    double WireProgress = 0.0;
    double WireTotal = 0.0;
    bool bHasTotal = false;
    bool bClamped = false;
    double RequestedProgress = 0.0;
    bool bHasRequestedProgress = false;
    {
        FScopeLock Lock(&Mutex);
        FJobTicket* T = Tickets.Find(TicketId);
        if (!T || T->Status != TEXT("running")) return false;

        const double Now = FPlatformTime::Seconds();
        if (!bBypassRateLimit && ProgressMinIntervalMs > 0 &&
            (Now - T->LastProgressSeconds) * 1000.0 < ProgressMinIntervalMs)
        {
            return false;
        }
        T->LastProgressSeconds = Now;
        FJobProgressEvent Ev{ FDateTime::UtcNow(), Message, Payload };
        T->Progress.Add(MoveTemp(Ev));
        constexpr int32 MaxProgressEvents = 50;
        if (T->Progress.Num() > MaxProgressEvents)
        {
            T->Progress.RemoveAt(0, T->Progress.Num() - MaxProgressEvents,
                EAllowShrinking::No);
        }
        ++T->ProgressSeq;

        // Caller units win when supplied; otherwise the never-trimmed event counter.
        if (Payload.IsValid() &&
            Payload->TryGetNumberField(TEXT("progress"), RequestedProgress))
        {
            bHasRequestedProgress = true;
            WireProgress = RequestedProgress;
        }
        else
        {
            WireProgress = static_cast<double>(T->ProgressSeq);
        }
        if (Payload.IsValid() && Payload->TryGetNumberField(TEXT("total"), WireTotal))
        {
            bHasTotal = true;
        }

        // The published value must never go backwards: a decreasing numerator reorders the
        // job in the client's eyes and is what the MCP "progress MUST increase" rule is
        // guarding against. A reporter that goes backwards is HELD at its previous value,
        // never advanced past it.
        //
        // Holding rather than incrementing is deliberate and is the whole judgement here.
        // A long job legitimately stalls — asset.dump_folder sits in "waiting for async
        // compilation" with its completed count flat — and bumping the number to satisfy
        // the letter of the rule would draw a bar that advances while nothing happens.
        // That is exactly the class of defect this plugin exists to stop reporting: a
        // number named for an outcome that was not observed. A repeated value is the
        // truth ("no further units completed"), and the message says why.
        if (T->ProgressSeq > 1 && WireProgress < T->LastProgressValue)
        {
            WireProgress = T->LastProgressValue;
            bClamped = true;
        }
        T->LastProgressValue = WireProgress;
        Method = T->Method;
    }

    auto Extra = MakeShared<FJsonObject>();
    Extra->SetStringField(TEXT("message"), Message);
    Extra->SetNumberField(TEXT("progress"), WireProgress);
    if (bHasTotal) Extra->SetNumberField(TEXT("total"), WireTotal);
    // The audit trail keeps the reporter's raw number whenever the hold changed it, so
    // the JSONL still shows what the verb actually reported. The flag is set on every
    // hold, including the mixed case where a numbered report is followed by an unnumbered
    // one and there is no raw value to preserve — a silent hold there would be the same
    // omission one field over. A held value is a reporter bug worth finding, not a normal
    // condition: nothing in the plugin should be counting backwards.
    if (bClamped)
    {
        Extra->SetBoolField(TEXT("progressHeldAtPrevious"), true);
        if (bHasRequestedProgress)
        {
            Extra->SetNumberField(TEXT("reportedProgress"), RequestedProgress);
        }
    }
    if (Payload.IsValid()) Extra->SetObjectField(TEXT("payload"), Payload);
    EmitEvent(MonitorLog, TicketId, Method, TEXT("progress"), Extra);
    // Only accepted (non-throttled) progress reaches this point, so the
    // observer fires iff a JSONL progress line was emitted.
    OnJobEventDelegate.Broadcast(TicketId, TEXT("progress"), Extra, nullptr);
    return true;
}

void FJobRegistry::Complete(const FString& TicketId, bool bSuccess,
                            const TSharedPtr<FJsonObject>& Result, const FString& Error)
{
    FString Method;
    {
        FScopeLock Lock(&Mutex);
        FJobTicket* T = Tickets.Find(TicketId);
        if (!T || T->Status != TEXT("running")) return;
        T->Status = bSuccess ? TEXT("completed") : TEXT("failed");
        T->CompletedAt = FDateTime::UtcNow();
        T->Result = Result;
        T->Error = Error;
        T->CancelCallback = nullptr;
        Method = T->Method;
    }

    auto Extra = MakeShared<FJsonObject>();
    if (Result.IsValid()) Extra->SetObjectField(TEXT("result"), Result);
    if (!Error.IsEmpty()) Extra->SetStringField(TEXT("error"), Error);
    EmitEvent(MonitorLog, TicketId, Method,
              bSuccess ? TEXT("completed") : TEXT("failed"), Extra);
    // Terminal result travels in the fourth arg on success only; on failure the
    // error string is already in the payload (Extra's "error" field).
    OnJobEventDelegate.Broadcast(TicketId,
        bSuccess ? TEXT("completed") : TEXT("failed"),
        Extra, bSuccess ? Result : TSharedPtr<FJsonObject>());
}

EJobCancelResult FJobRegistry::Cancel(const FString& TicketId)
{
    TFunction<void()> Cb;
    FString Method;
    {
        FScopeLock Lock(&Mutex);
        FJobTicket* T = Tickets.Find(TicketId);
        if (!T) return EJobCancelResult::NotFound;
        if (T->Status != TEXT("running")) return EJobCancelResult::NotRunning;
        if (!T->CancelCallback)
        {
            // Nothing to stop the work with. Return WITHOUT mutating the ticket: marking it
            // "cancelled" here is what made system.job_cancel report success on a job that then
            // wrote ~19,700 more files, and it also silently voids the real outcome, because
            // Complete() refuses to overwrite a terminal ticket. Leaving the status at "running"
            // keeps both the ticket and the eventual completion honest.
            return EJobCancelResult::Unsupported;
        }
        Cb = MoveTemp(T->CancelCallback);
        T->Status = TEXT("cancelled");
        T->CompletedAt = FDateTime::UtcNow();
        Method = T->Method;
    }
    Cb();
    EmitEvent(MonitorLog, TicketId, Method, TEXT("cancelled"), nullptr);
    OnJobEventDelegate.Broadcast(TicketId, TEXT("cancelled"), nullptr, nullptr);
    return EJobCancelResult::Requested;
}

bool FJobRegistry::Get(const FString& TicketId, FJobTicket& OutTicket) const
{
    FScopeLock Lock(&Mutex);
    if (const FJobTicket* T = Tickets.Find(TicketId))
    {
        OutTicket = *T;
        return true;
    }
    return false;
}

TArray<FJobTicket> FJobRegistry::List() const
{
    FScopeLock Lock(&Mutex);
    TArray<FJobTicket> Out;
    Out.Reserve(Tickets.Num());
    for (const auto& Pair : Tickets) Out.Add(Pair.Value);
    return Out;
}

void FJobRegistry::EvictExpired(const FDateTime& Now)
{
    const FTimespan Ttl = FTimespan::FromSeconds(TtlSeconds);
    FScopeLock Lock(&Mutex);
    for (auto It = Tickets.CreateIterator(); It; ++It)
    {
        const FJobTicket& T = It.Value();
        if (T.Status != TEXT("running") && (Now - T.CompletedAt) > Ttl)
        {
            It.RemoveCurrent();
        }
    }
}

TSharedPtr<FJsonObject> FJobRegistry::ToJson(const FJobTicket& T)
{
    auto Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("ticket_id"), T.TicketId);
    Out->SetStringField(TEXT("method"), T.Method);
    Out->SetStringField(TEXT("status"), T.Status);
    Out->SetStringField(TEXT("started_at"), T.StartedAt.ToIso8601());
    if (T.Status != TEXT("running"))
        Out->SetStringField(TEXT("completed_at"), T.CompletedAt.ToIso8601());
    if (T.Result.IsValid()) Out->SetObjectField(TEXT("result"), T.Result);
    if (!T.Error.IsEmpty()) Out->SetStringField(TEXT("error"), T.Error);

    TArray<TSharedPtr<FJsonValue>> ProgressJson;
    for (const FJobProgressEvent& Ev : T.Progress)
    {
        auto E = MakeShared<FJsonObject>();
        E->SetStringField(TEXT("ts"), Ev.Timestamp.ToIso8601());
        E->SetStringField(TEXT("message"), Ev.Message);
        if (Ev.Payload.IsValid()) E->SetObjectField(TEXT("payload"), Ev.Payload);
        ProgressJson.Add(MakeShared<FJsonValueObject>(E));
    }
    Out->SetArrayField(TEXT("progress"), ProgressJson);
    return Out;
}
