// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "Dom/JsonObject.h"
#include "HAL/CriticalSection.h"
#include "Transport/McpTransportTypes.h"
#include <atomic>

class FSocket;
class FRunnableThread;
class ISocketSubsystem;

namespace McpRequestCore { struct FRequestDecision; }

DECLARE_LOG_CATEGORY_EXTERN(LogSocketHttp, Log, All);

// Custom HTTP/1.1 server over raw sockets that owns POST /mcp. It can hold a
// response open, which enables the opt-in SSE streaming path (progress frames
// for long-running tools/call requests). Buffered JSON remains the default
// response mode.
//
// Threading model:
//  - Start/Stop/QuiesceRequestIntake/DrainPendingWrites/Tick/ResolveCompletion/
//    WriteStreamFrame/FailAllCompletions run on the game thread (Tick from the
//    subsystem ticker).
//  - ONE non-blocking I/O thread polls the listener plus all live connections; it owns
//    every socket read/write and the per-connection parse state.
//  - The game thread reaches a connection only through a thread-safe outbound byte
//    queue (plus a few atomics) held by a TSharedPtr pinned under the completion mutex.
class PINWRIGHT_API FSocketHttpServer : public TSharedFromThis<FSocketHttpServer>
{
public:
    // Constructor/destructor are out-of-line: members hold TUniquePtr to types
    // that are incomplete in this header (FIoRunnable), so the compiler-generated
    // special members must be instantiated where the types are complete.
    FSocketHttpServer();
    ~FSocketHttpServer();

    // Why Start failed. A bare bool could not tell a caller whether waiting would help,
    // so a port collision - the one failure that clears itself when the previous editor
    // process finishes exiting - was indistinguishable from a missing socket subsystem.
    enum class EStartResult : uint8
    {
        // Bound, listening, I/O thread up. Also reported for an already-active server.
        Ok,
        // 127.0.0.1:Port is held by another process, or lies in an OS-reserved range.
        // RETRYABLE: the holder may exit.
        PortInUse,
        // No socket subsystem, socket creation failed, listen() failed, or the I/O thread
        // did not start. Waiting clears none of these.
        Unavailable
    };

    // Binds 127.0.0.1:Port (the direct bind doubles as the port-conflict probe),
    // starts listening, and spins up the I/O thread. Returns false on any failure;
    // OutResult, when supplied, says whether a retry could ever succeed.
    bool Start(uint32 Port, EStartResult* OutResult = nullptr);
    void Stop();

    // Prevent any later I/O-thread handoff and wait for a handoff already in
    // progress to leave the request delegate. The I/O thread remains alive so
    // responses resolved during owner teardown can still be written.
    void QuiesceRequestIntake();

    // After intake is quiesced and completions are resolved, wait until queued
    // responses have reached the socket and each connection has completed its
    // bounded FIN/linger phase, or until the caller's finite timeout expires.
    bool DrainPendingWrites(double TimeoutSeconds);

    // Fired on the I/O thread when a valid execute request arrives; the dispatcher
    // marshals to the game thread itself.
    FOnRpcRequest OnRequestReceived;

    // Completes a pending request: buffered entries get their HTTP 200 JSON response,
    // streaming entries get the final JSON-RPC response as a last SSE frame and close.
    bool ResolveCompletion(const FString& RequestId, bool bSuccess, const FString& Message,
                           const TSharedPtr<FJsonObject>& Result, const FString& ErrorCode);

    // Enqueues one SSE `event: message` frame carrying the notification (condensed
    // JSON). Refreshes the entry's timeout deadline so a live stream is never reaped.
    // Returns false if the request is unknown, not streaming, or its client is gone.
    bool WriteStreamFrame(const FString& RequestId, const TSharedRef<FJsonObject>& Notification);

    // Game-thread ticker entry: sweeps completion timeouts and enqueues SSE heartbeat
    // comments for idle streaming connections.
    void Tick(float DeltaTime);

    // Publish the game-thread editor-startup snapshot consumed by the I/O-thread
    // request gate and bootstrap ping. The transport stores only atomics so the
    // socket thread never dereferences editor-owned objects.
    void SetEditorReadiness(bool bEditorReady, bool bEditorLoadingPackage,
                            bool bEditorSelectionSetAvailable);

    void FailAllCompletions(const FString& Message, const FString& ErrorCode);

    bool IsStreamingRequest(const FString& RequestId) const;

    // The progress token the client attached to this request, for echoing back in
    // notifications/progress. False when the request is unknown or carried no token.
    bool GetProgressToken(const FString& RequestId, TSharedPtr<FJsonValue>& OutToken) const;

private:
    // Per-connection HTTP state machine plus the outbound byte queue. Created and torn
    // down on the I/O thread; the game thread only touches OutQueue and the atomics.
    struct FConnection
    {
        FSocket* Socket = nullptr;

        // ---- I/O-thread-only inbound state ----
        TArray<uint8> InBuffer;                 // accumulated unparsed bytes
        bool bHaveHeaders = false;              // request line + headers parsed
        bool bInputBroken = false;              // malformed input; stop parsing, close after flush
        FString Method;
        FString Path;
        // Keys are lowercased. Duplicate Authorization values are newline-joined
        // for independent bearer parsing; other duplicates are comma-joined.
        TMap<FString, FString> Headers;
        int32 ContentLength = 0;
        bool bKeepAlive = true;

        // Buffered responses must go out in request order, so parsing stalls while a
        // dispatched RPC is outstanding on this connection (set on dispatch, cleared by
        // the completion callback).
        std::atomic<bool> bAwaitingResponse { false };

        // Once an SSE response starts the connection stops parsing further input.
        std::atomic<bool> bStreaming { false };

        // ---- outbound (producers: game thread + I/O thread; consumer: I/O thread) ----
        TQueue<TArray<uint8>, EQueueMode::Mpsc> OutQueue;
        TArray<uint8> PendingOut;               // I/O-thread-only partial-send remainder
        std::atomic<bool> bCloseAfterFlush { false };
        std::atomic<bool> bDead { false };      // set by the I/O thread on teardown

        // ---- I/O-thread-only lingering-close state ----
        // After the final response is flushed the write side is shut down (FIN) and
        // inbound bytes are drained until the peer closes or the grace expires; an
        // immediate close() with unread inbound data would RST and destroy the
        // just-sent response on the peer's side. The idle deadline is pushed back
        // while the peer is still streaming (it clearly hasn't processed the FIN
        // yet); LingerAbortSeconds caps the total linger regardless.
        bool bLingering = false;
        double LingerDeadlineSeconds = 0.0;
        double LingerAbortSeconds = 0.0;
    };

    struct FPendingCompletion
    {
        FTransportCompletionCallback Callback;
        double DeadlineSeconds = 0.0;
        float TimeoutSeconds = 0.0f;
        FString Method;
        bool bStreaming = false;
        // Last frame or heartbeat write, in TICK-CLOCK seconds (accumulated Tick
        // deltas, not wall clock); drives the heartbeat cadence (game thread, guarded
        // by CompletionMutex). Heartbeats do NOT refresh DeadlineSeconds — only real
        // WriteStreamFrame calls do, so an idle stream still times out.
        double LastStreamActivitySeconds = 0.0;
        TWeakPtr<FConnection> Connection;
        // The client's own params._meta.progressToken, retained verbatim (string or
        // number — MCP permits both). Every notifications/progress frame for this
        // request must echo THIS value: the spec admits only tokens "provided in an
        // active request", so a server-invented token is one a conforming client
        // cannot correlate and may drop.
        TSharedPtr<FJsonValue> ProgressToken;
    };

    class FIoRunnable;

    // ---- I/O thread ----
    void RunIoThread();
    void AcceptPendingConnections(ISocketSubsystem& Sub, bool& bDidWork);
    bool PumpConnectionRead(FConnection& Conn, ISocketSubsystem& Sub, bool& bDidWork);
    void PumpParse(const TSharedPtr<FConnection>& Conn);
    bool FlushConnectionWrites(FConnection& Conn, ISocketSubsystem& Sub, bool& bDidWork);
    void TeardownConnection(const TSharedPtr<FConnection>& Conn, ISocketSubsystem& Sub);
    void HandleCompleteRequest(const TSharedPtr<FConnection>& Conn, const FString& BodyString);
    bool ShouldStreamRequest(
        const FConnection& Conn,
        const McpRequestCore::FRequestDecision& Decision) const;

    // ---- completion registry ----
    bool RegisterCompletion(const FString& RequestId, FTransportCompletionCallback Callback,
                            double TimeoutSeconds, const FString& Method, bool bStreaming,
                            const TWeakPtr<FConnection>& Connection,
                            const TSharedPtr<FJsonValue>& ProgressToken);
    void ProcessCompletionTimeouts();
    void SendHeartbeats();
    void DropCompletionsForConnection(const FConnection* Conn);

    static bool ParseRequestHead(FConnection& Conn, const FString& HeadText, FString& OutError);
    static void RejectAndClose(FConnection& Conn, int32 Code, const FString& Message);
    static void EnqueueBytes(FConnection& Conn, TArray<uint8>&& Bytes);

    FSocket* ListenSocket = nullptr;
    TUniquePtr<FIoRunnable> IoRunnable;
    FRunnableThread* IoThread = nullptr;
    std::atomic<bool> bStopRequested { false };
    std::atomic<bool> bAcceptingRequests { false };
    std::atomic<bool> bCloseConnectionsAfterDrain { false };
    std::atomic<uint64> LastRequestedDrainId { 0 };
    std::atomic<uint64> LastCompletedDrainId { 0 };
    bool bActive = false;
    uint32 BoundPort = 0;

    // Bind failures seen by this server. The first one prints the full remediation text at
    // Error; the retries the subsystem drives print at Verbose, because the same wall of
    // guidance repeated on every backoff step buries the one line that matters. The
    // retry-cadence Warning belongs to the subsystem, which owns the schedule.
    int32 BindFailureCount = 0;

    // Accumulated Tick DeltaTime. The heartbeat cadence runs on this clock (not wall
    // time) so callers that drive Tick with synthetic deltas — the automation tests —
    // can advance it; under the real 0.1s subsystem ticker it tracks wall time anyway.
    // Atomic: written on the game thread (Tick), read on the I/O thread when a new
    // completion snapshots its initial LastStreamActivitySeconds.
    std::atomic<double> TickClockSeconds { 0.0 };

    // Readiness snapshot published by UPinWrightSubsystem on the game thread;
    // fail closed until the first safe snapshot arrives.
    std::atomic<bool> bEditorReady { false };
    std::atomic<bool> bEditorLoadingPackage { true };
    std::atomic<bool> bEditorSelectionSetAvailable { false };

    // Settings snapshot, published on Start (game thread) before the I/O thread exists.
    int32 MaxBodyBytes = 1024 * 1024;
    int32 DefaultTimeoutMs = 120000;
    int32 MaxTimeoutMs = 300000;
    int32 SpillThresholdCharacters = 0;

    TArray<TSharedPtr<FConnection>> Connections;    // I/O thread only

    // Serializes the final request-delegate handoff with QuiesceRequestIntake.
    // The delegate itself may schedule game-thread work, so dispatcher-side
    // lifetime validation remains required for that queued work.
    FCriticalSection RequestIntakeMutex;

    TMap<FString, FPendingCompletion> PendingCompletions;
    mutable FCriticalSection CompletionMutex;
};
