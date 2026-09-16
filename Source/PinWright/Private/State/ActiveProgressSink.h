// Copyright (c) 2026 Alexander Penkin. MIT License.

// ActiveProgressSink - the ambient "who is listening for progress right now" slot.
//
// Why ambient rather than a threaded-through parameter: the code that wants to report
// progress is frequently not PinWright's. python.execute hands control to a user script
// that PinWright never sees, and that script cannot be given a C++ handle. An ambient
// slot published for the duration of the handler's synchronous work lets that code
// report progress by name alone (unreal.PinWrightProgressLibrary.report_progress), with
// no token to pass and nothing to plumb.
//
// Correctness rests on one fact: every RPC handler runs on the game thread
// (FRpcDispatcher::ProcessRequest marshals with AsyncTask(ENamedThreads::GameThread)),
// and a handler's synchronous body cannot be interleaved with another handler's. Report()
// therefore refuses off-thread callers rather than racing, and the scope is a strict
// save/restore stack so nesting cannot orphan an outer sink.

#pragma once

#include "CoreMinimal.h"

namespace PinWright::Progress
{
    // Publishes TicketId as the ambient sink for the lifetime of the scope, restoring
    // whatever was published before. Constructing with an empty ticket id publishes
    // "nobody is listening", which is what makes Report() a cheap no-op on the ordinary
    // non-streaming path instead of an error.
    struct FScopedSink
    {
        explicit FScopedSink(const FString& InTicketId);
        ~FScopedSink();

        FScopedSink(const FScopedSink&) = delete;
        FScopedSink& operator=(const FScopedSink&) = delete;

    private:
        FString Previous;
    };

    // True when a ticket is published AND still running, i.e. a Report() would reach a
    // client. Callers use this to skip building an expensive progress message.
    bool IsActive();

    // Ticket id currently published, or empty.
    FString ActiveTicketId();

    // Records one progress event against the ambient ticket. Total <= 0 means "unknown",
    // and is omitted from the wire frame rather than sent as zero.
    //
    // Returns true only when the event was accepted and broadcast. False means the event
    // did NOT reach a client, and every reason is legitimate: no ambient ticket, the
    // ticket already reached a terminal state, an off-game-thread caller, or - most often -
    // the registry's rate limit, which is respected here precisely so that calling this
    // every iteration of a tight loop is safe. A caller must not treat false as a failure
    // of its own work, and must not stop reporting because of it.
    bool Report(const FString& Message, double Progress, double Total);
}
