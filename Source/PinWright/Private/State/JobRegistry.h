// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HAL/CriticalSection.h"
#include "Misc/DateTime.h"

class FJobMonitorLog;

struct FJobProgressEvent
{
    FDateTime Timestamp;
    FString   Message;
    TSharedPtr<FJsonObject> Payload;
};

// Outcome of a cancellation request. A bare bool cannot tell "the work is stopping" apart from
// "the ticket was marked cancelled while the work ran on", and that distinction is the entire
// reason an agent asks: cancelling is what it does in RESPONSE to a hang, so a false success
// leaves it believing the editor is idle while the job keeps writing.
enum class EJobCancelResult : uint8
{
    // A cancel callback was registered and has now been invoked. The verb opted in to
    // cancellation, so the underlying work is genuinely being stopped. Ticket -> "cancelled".
    Requested,
    // The ticket is running, but its verb registered no cancel callback: nothing in this process
    // can stop the work. The ticket is left RUNNING and otherwise untouched — flipping it to
    // "cancelled" would be the same lie one layer down, and would additionally make Complete()
    // discard the job's real outcome when it eventually lands (see the Status guard in
    // Complete). Making these verbs genuinely cancellable is 200-500 lines each (chunking a loop
    // that has no engine completion hook); reporting the truth is the honest interim answer.
    Unsupported,
    // No ticket with that id: never existed, or was evicted after CompletedTicketTtlSeconds.
    NotFound,
    // The ticket exists but already reached a terminal state, so there is nothing to cancel.
    NotRunning,
};

struct FJobTicket
{
    FString TicketId;
    FString Method;
    FString Status;            // "running" | "completed" | "failed" | "cancelled"
    FDateTime StartedAt;
    FDateTime CompletedAt;
    TSharedPtr<FJsonObject> Result;
    FString Error;
    TArray<FJobProgressEvent> Progress;
    TFunction<void()> CancelCallback;
    double LastProgressSeconds = 0.0;
    // Count of ACCEPTED progress events over the ticket's whole life. Progress[] is
    // trimmed to the newest MaxProgressEvents, so its Num() saturates and cannot serve
    // as the wire progress value: MCP requires that value to increase with every
    // notification (spec 2025-06-18, Progress, "Behavior Requirements"). This counter
    // is never trimmed, so it is the one that can.
    int64 ProgressSeq = 0;
    // Last value published as the MCP `progress` number, kept so a reporter whose value
    // goes backwards is held here rather than reordering the job in the client's eyes.
    // Held, never advanced: inventing movement for a stalled job is the defect this
    // plugin exists to stop reporting. Only RecordProgress writes it.
    double LastProgressValue = 0.0;
};

// Observer hook fired on every job state transition, mirroring the JSONL
// monitor-log vocabulary exactly: started | progress | completed | failed |
// cancelled. Broadcast on the game thread (job mutations happen on the
// game-thread ticker), always OUTSIDE the registry mutex so subscribers may
// safely call back into the registry.
DECLARE_MULTICAST_DELEGATE_FourParams(FOnJobEvent,
    const FString& /*TicketId*/,
    const FString& /*Event: started|progress|completed|failed|cancelled*/,
    const TSharedPtr<FJsonObject>& /*Payload (event-specific extras; may be null)*/,
    const TSharedPtr<FJsonObject>& /*ResultOrNull (terminal result on completed, else null)*/);

class PINWRIGHT_API FJobRegistry
{
public:
    FJobRegistry(int32 InTtlSeconds, int32 InProgressMinIntervalMs, FJobMonitorLog* InMonitorLog);

    FString Start(const FString& Method, const TSharedRef<FJsonObject>& Params);
    // Registering a callback is what makes a verb cancellable, and it is also the ONLY signal
    // Cancel() has for telling a cancellable verb from an uncancellable one. That is deliberate:
    // the capability is read from the mechanism itself, per ticket, so it cannot drift the way a
    // hand-maintained list of "verbs that support cancel" would. Register it in the same
    // synchronous block as Start() — a callback attached later leaves a window in which
    // system.job_cancel correctly, but unhelpfully, reports the verb as uncancellable.
    void   SetCancelCallback(const FString& TicketId, TFunction<void()> Callback);
    // bBypassRateLimit skips the ProgressMinIntervalMs throttle (streamed jobs
    // want every progress event); the accepted event still refreshes the
    // rate-limit clock for subsequent throttled callers.
    //
    // Payload may carry a "progress" number and a "total" number. When it does, those
    // are the caller's own units (item 12 of 1660) and are published as-is except for
    // the monotonic clamp below. When it does not, the wire value is the ticket's
    // ProgressSeq, which counts accepted events. Either way this function is the ONLY
    // writer of the published `progress` number, so the JSONL monitor line and the MCP
    // notifications/progress frame cannot disagree about it.
    bool   RecordProgress(const FString& TicketId, const FString& Message,
                          const TSharedPtr<FJsonObject>& Payload = nullptr,
                          bool bBypassRateLimit = false);
    void   Complete(const FString& TicketId, bool bSuccess,
                    const TSharedPtr<FJsonObject>& Result, const FString& Error);
    // Only EJobCancelResult::Requested moves the ticket to "cancelled" and fires the observer /
    // monitor-log "cancelled" event. Every other outcome is inspection-only: nothing is mutated
    // and nothing is broadcast, so an uncancellable job's ticket keeps reporting the truth
    // ("running") and its eventual real result still reaches Complete().
    EJobCancelResult Cancel(const FString& TicketId);

    // Observer for job state transitions; fires at the same points (and with the
    // same event strings) as the JSONL monitor log, only for accepted
    // (non-throttled) progress events.
    FOnJobEvent& OnJobEvent() { return OnJobEventDelegate; }

    bool   Get(const FString& TicketId, FJobTicket& OutTicket) const;
    TArray<FJobTicket> List() const;
    void   EvictExpired(const FDateTime& Now);

    static TSharedPtr<FJsonObject> ToJson(const FJobTicket& Ticket);

private:
    static FString AllocateId(const FString& Method);

    int32 TtlSeconds;
    int32 ProgressMinIntervalMs;
    FJobMonitorLog* MonitorLog;            // not owned
    TMap<FString, FJobTicket> Tickets;
    mutable FCriticalSection Mutex;
    FOnJobEvent OnJobEventDelegate;        // broadcast outside Mutex only
};
