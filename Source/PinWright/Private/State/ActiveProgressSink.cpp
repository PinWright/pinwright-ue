// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "State/ActiveProgressSink.h"

#include "State/JobRegistry.h"
#include "State/PluginState.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"

namespace PinWright::Progress
{
    namespace
    {
        // Game-thread only; see the header for why that is sufficient.
        FString GActiveTicketId;
    }

    FScopedSink::FScopedSink(const FString& InTicketId)
        : Previous(GActiveTicketId)
    {
        GActiveTicketId = InTicketId;
    }

    FScopedSink::~FScopedSink()
    {
        GActiveTicketId = Previous;
    }

    FString ActiveTicketId()
    {
        return IsInGameThread() ? GActiveTicketId : FString();
    }

    bool IsActive()
    {
        const FString TicketId = ActiveTicketId();
        if (TicketId.IsEmpty())
        {
            return false;
        }
        FJobTicket Ticket;
        if (!FPluginState::Get().GetJobRegistry().Get(TicketId, Ticket))
        {
            return false;
        }
        return Ticket.Status == TEXT("running");
    }

    bool Report(const FString& Message, double Progress, double Total)
    {
        const FString TicketId = ActiveTicketId();
        if (TicketId.IsEmpty())
        {
            return false;
        }

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetNumberField(TEXT("progress"), Progress);
        if (Total > 0.0)
        {
            Payload->SetNumberField(TEXT("total"), Total);
        }

        // The throttle is DELIBERATELY respected here, unlike the plugin's own streamed
        // jobs which bypass it. Those emit on a timer they control; this one is called by
        // code we did not write, and the obvious way to write it - report every iteration -
        // is exactly what a caller will do. Measured on this project's map: a loop over
        // 3283 actors completes in 0.1 s, so per-item reporting would put ~3283 frames on
        // the socket inside a tenth of a second and hand the client 3283 notifications to
        // parse. MCP asks both parties to rate-limit progress for this reason.
        //
        // Respecting it makes the naive loop correct by construction (rpc-design.md §2:
        // a rule the caller would have to remember becomes a property of the mechanism),
        // and ProgressEventMinIntervalMs - now 1 s, not the old 60 s - is the right
        // resolution for something a human or an agent is watching.
        return FPluginState::Get().GetJobRegistry().RecordProgress(
            TicketId, Message, Payload, /*bBypassRateLimit=*/false);
    }
}
