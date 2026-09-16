// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Transport/SocketHttpServer.h"
#include "Transport/McpRequestCore.h"
#include "Transport/ModalStateProbe.h"
#include "State/ClientActivity.h"
#include "JsonRpc.h"
#include "Utils/HttpResponseSpill.h"
#include "PinWrightSettings.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY(LogSocketHttp);

namespace
{
    constexpr int32 kMaxConnections = 64;
    constexpr int32 kMaxHeaderBytes = 16 * 1024;
    constexpr int32 kListenBacklog = 16;
    constexpr int32 kMaxReadBytesPerPump = 64 * 1024;
    constexpr float kIdleSleepSeconds = 0.002f;
    // Grace window for the lingering close: after the final response is flushed and
    // the write side FIN'd, inbound bytes are drained this long (or until the peer
    // closes) before the socket is destroyed.
    constexpr double kCloseLingerSeconds = 2.0;

    // Absolute cap on a lingering close. The idle grace above is extended while the
    // peer keeps streaming inbound bytes (e.g. an oversized body still in flight
    // behind an early 413 — closing mid-upload would RST and discard the response
    // on the peer's side), so a hostile peer must not be able to hold a doomed
    // connection open forever by trickling bytes.
    constexpr double kMaxLingerSeconds = 30.0;
    constexpr double kShutdownWriteDrainSeconds = kMaxLingerSeconds + 1.0;

    const TCHAR* StatusReason(int32 Code)
    {
        switch (Code)
        {
        case 100: return TEXT("Continue");
        case 200: return TEXT("OK");
        case 202: return TEXT("Accepted");
        case 400: return TEXT("Bad Request");
        case 401: return TEXT("Unauthorized");
        case 403: return TEXT("Forbidden");
        case 404: return TEXT("Not Found");
        case 405: return TEXT("Method Not Allowed");
        case 413: return TEXT("Payload Too Large");
        case 500: return TEXT("Internal Server Error");
        case 503: return TEXT("Service Unavailable");
        default:  return TEXT("OK");
        }
    }

    // Condensed (single-line) serialization: SSE `data:` lines must not contain raw
    // newlines, and buffered bodies don't need pretty-printing either.
    FString SerializeCondensed(const TSharedPtr<FJsonObject>& Obj)
    {
        if (!Obj.IsValid())
        {
            return FString();
        }
        FString Out;
        const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
        FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
        Writer->Close();
        return Out;
    }

    TArray<uint8> ToUtf8Bytes(const FString& Text)
    {
        FTCHARToUTF8 Converter(*Text, Text.Len());
        TArray<uint8> Out;
        Out.Append(reinterpret_cast<const uint8*>(Converter.Get()), Converter.Length());
        return Out;
    }

    FString Utf8BytesToString(const uint8* Bytes, int32 Num)
    {
        FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Bytes), Num);
        return FString(Converter.Length(), Converter.Get());
    }

    // Full response with accurate Content-Length; every status code, including interim
    // and error responses, carries an explicit Content-Type — a body is never delimited
    // by socket close. ExtraHeaders must be empty or "Name: value\r\n"-terminated lines.
    TArray<uint8> BuildHttpResponseBytes(int32 Code, const FString& ContentType,
                                         const FString& Body, bool bKeepAlive,
                                         const FString& ExtraHeaders = FString())
    {
        const TArray<uint8> BodyBytes = ToUtf8Bytes(Body);
        const FString Head = FString::Printf(
            TEXT("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: %s\r\n%s\r\n"),
            Code, StatusReason(Code), *ContentType, BodyBytes.Num(),
            bKeepAlive ? TEXT("keep-alive") : TEXT("close"), *ExtraHeaders);
        TArray<uint8> Out = ToUtf8Bytes(Head);
        Out.Append(BodyBytes);
        return Out;
    }

    // SSE response head: no Content-Length — the stream is delimited by connection close
    // after the final frame.
    TArray<uint8> BuildSseResponseHeadBytes()
    {
        return ToUtf8Bytes(TEXT("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: keep-alive\r\n\r\n"));
    }

    TArray<uint8> BuildSseMessageFrame(const FString& CondensedJson)
    {
        return ToUtf8Bytes(FString::Printf(TEXT("event: message\r\ndata: %s\r\n\r\n"), *CondensedJson));
    }

    // Comment frame: keeps idle streams alive through proxies without emitting an event.
    TArray<uint8> BuildSseHeartbeatFrame()
    {
        return ToUtf8Bytes(TEXT(": ping\n\n"));
    }

    bool IsAllowedOrigin(const FString& Origin)
    {
        FString Scheme;
        FString Authority;
        if (!Origin.Split(TEXT("://"), &Scheme, &Authority)
            || !Scheme.Equals(TEXT("http"), ESearchCase::IgnoreCase)
            || Authority.IsEmpty()
            || Authority.Contains(TEXT("@"))
            || Authority.Contains(TEXT("/"))
            || Authority.Contains(TEXT("?"))
            || Authority.Contains(TEXT("#")))
        {
            return false;
        }

        FString Host = Authority;
        FString Port;
        if (Authority.Split(TEXT(":"), &Host, &Port))
        {
            if (Port.IsEmpty())
            {
                return false;
            }
            for (const TCHAR Character : Port)
            {
                if (!FChar::IsDigit(Character))
                {
                    return false;
                }
            }
            const int32 PortNumber = FCString::Atoi(*Port);
            if (PortNumber <= 0 || PortNumber > 65535)
            {
                return false;
            }
        }

        return Host.Equals(TEXT("localhost"), ESearchCase::IgnoreCase)
            || Host.Equals(TEXT("127.0.0.1"), ESearchCase::IgnoreCase);
    }

    int32 FindHeaderTerminator(const TArray<uint8>& Buffer)
    {
        const int32 Num = Buffer.Num();
        for (int32 Index = 0; Index + 3 < Num; ++Index)
        {
            if (Buffer[Index] == '\r' && Buffer[Index + 1] == '\n'
                && Buffer[Index + 2] == '\r' && Buffer[Index + 3] == '\n')
            {
                return Index;
            }
        }
        return INDEX_NONE;
    }

    bool IsAllDigits(const FString& Text)
    {
        if (Text.IsEmpty())
        {
            return false;
        }
        for (const TCHAR Ch : Text)
        {
            if (!FChar::IsDigit(Ch))
            {
                return false;
            }
        }
        return true;
    }
}

// Forwards Run() onto the owning server; FRunnable's Stop() maps to the shared stop flag.
class FSocketHttpServer::FIoRunnable : public FRunnable
{
public:
    explicit FIoRunnable(FSocketHttpServer& InOwner) : Owner(InOwner) {}

    virtual uint32 Run() override
    {
        Owner.RunIoThread();
        return 0;
    }

    virtual void Stop() override
    {
        Owner.bStopRequested.store(true);
    }

private:
    FSocketHttpServer& Owner;
};

FSocketHttpServer::FSocketHttpServer() = default;

FSocketHttpServer::~FSocketHttpServer()
{
    Stop();
}

bool FSocketHttpServer::Start(uint32 Port, EStartResult* OutResult)
{
    auto Finish = [OutResult](EStartResult Result) -> bool
    {
        if (OutResult)
        {
            *OutResult = Result;
        }
        return Result == EStartResult::Ok;
    };

    if (bActive)
    {
        Finish(EStartResult::Ok);
        return true;
    }

    bStopRequested.store(false);

    const UPinWrightSettings* Settings = GetDefault<UPinWrightSettings>();
    MaxBodyBytes = FMath::Max(1024, Settings->HttpMaxRequestBodyBytes);
    DefaultTimeoutMs = FMath::Max(1000, Settings->HttpDefaultTimeoutMs);
    MaxTimeoutMs = FMath::Max(DefaultTimeoutMs, Settings->HttpMaxTimeoutMs);
    SpillThresholdCharacters =
        HttpResponseSpill::ClampThresholdCharacters(Settings->HttpResponseSpillThresholdCharacters);

    ISocketSubsystem* Sub = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!Sub)
    {
        UE_LOG(LogSocketHttp, Error, TEXT("Socket subsystem unavailable; transport not started."));
        return Finish(EStartResult::Unavailable);
    }

    // Direct bind doubles as the pre-flight port probe: a raw socket surfaces the
    // bind result, so a port collision with e.g. a second editor is detected right
    // here. No reuse-addr on purpose.
    FSocket* Socket = Sub->CreateSocket(NAME_Stream, TEXT("PinWrightSocketHttpListener"), false);
    if (!Socket)
    {
        UE_LOG(LogSocketHttp, Error, TEXT("Failed to create listener socket for port %u."), Port);
        return Finish(EStartResult::Unavailable);
    }

    const TSharedRef<FInternetAddr> BindAddr = Sub->CreateInternetAddr();
    BindAddr->SetLoopbackAddress();
    BindAddr->SetPort(static_cast<int32>(Port));

    if (!Socket->Bind(*BindAddr))
    {
        // Full remediation text on the FIRST failure only. The subsystem retries this bind on
        // a bounded backoff (Transport/BindRetryPolicy.h), and repeating eight lines of
        // guidance on every step would bury its per-attempt Warning - which is the line that
        // actually says whether the port is still contested.
        ++BindFailureCount;
        if (BindFailureCount == 1)
        {
            UE_LOG(LogSocketHttp, Error,
                   TEXT("Port %u could not be bound - it is either already in use (e.g. another project's ")
                   TEXT("editor on the same default port, or a previous editor of THIS project that has ")
                   TEXT("not finished exiting), or reserved by the OS (e.g. a Windows excluded / ")
                   TEXT("Hyper-V port range). This editor's MCP server is NOT serving; the bind will be ")
                   TEXT("retried on a bounded backoff. If the retries are exhausted, fix it by enabling ")
                   TEXT("\"Auto-derive Port From Project Path\" in Project Settings (PinWright) to give ")
                   TEXT("each project a unique port, or set a different fixed HttpPort, then restart and re-run ")
                   TEXT("onboarding to update the agent configs."),
                   Port);
        }
        else
        {
            UE_LOG(LogSocketHttp, Verbose,
                   TEXT("Port %u still could not be bound (failure %d)."), Port, BindFailureCount);
        }
        Sub->DestroySocket(Socket);
        return Finish(EStartResult::PortInUse);
    }

    if (!Socket->Listen(kListenBacklog))
    {
        UE_LOG(LogSocketHttp, Error, TEXT("Listen failed on port %u."), Port);
        Sub->DestroySocket(Socket);
        return Finish(EStartResult::Unavailable);
    }
    Socket->SetNonBlocking(true);

    ListenSocket = Socket;
    BoundPort = Port;
    bCloseConnectionsAfterDrain.store(false, std::memory_order_release);
    bAcceptingRequests.store(true, std::memory_order_release);

    IoRunnable = MakeUnique<FIoRunnable>(*this);
    IoThread = FRunnableThread::Create(IoRunnable.Get(), TEXT("PinWrightSocketHttp"), 0, TPri_Normal);
    if (!IoThread)
    {
        UE_LOG(LogSocketHttp, Error, TEXT("Failed to start I/O thread."));
        Sub->DestroySocket(ListenSocket);
        ListenSocket = nullptr;
        IoRunnable.Reset();
        bAcceptingRequests.store(false, std::memory_order_release);
        return Finish(EStartResult::Unavailable);
    }

    bActive = true;
    UE_LOG(LogSocketHttp, Log,
           TEXT("Socket HTTP transport active on 127.0.0.1:%u/mcp (SSE streaming supported)."), Port);
    return Finish(EStartResult::Ok);
}

void FSocketHttpServer::Stop()
{
    if (!bActive && !IoThread && !ListenSocket)
    {
        return;
    }

    QuiesceRequestIntake();
    FailAllCompletions(TEXT("MCP transport stopped."), TEXT("TRANSPORT_STOPPED"));
    if (!DrainPendingWrites(kShutdownWriteDrainSeconds))
    {
        UE_LOG(LogSocketHttp, Warning,
               TEXT("Timed out draining MCP responses before transport stop."));
    }

    // FRunnable's stop path publishes the flag observed by the outer loop and
    // by the bounded accept/read loops. All sockets are non-blocking, so after
    // this point the join is limited to the current capped pass.
    if (IoRunnable)
    {
        IoRunnable->Stop();
    }
    else
    {
        bStopRequested.store(true, std::memory_order_release);
    }
    if (IoThread)
    {
        IoThread->WaitForCompletion();
        delete IoThread;
        IoThread = nullptr;
    }
    IoRunnable.Reset();

    if (ListenSocket)
    {
        ListenSocket->Close();
        if (ISocketSubsystem* Sub = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
        {
            Sub->DestroySocket(ListenSocket);
        }
        ListenSocket = nullptr;
    }

    bActive = false;
    UE_LOG(LogSocketHttp, Log, TEXT("Socket HTTP transport stopped."));
}

void FSocketHttpServer::QuiesceRequestIntake()
{
    bAcceptingRequests.store(false, std::memory_order_release);

    // A request that already crossed the atomic gate may be inside the delegate.
    // Taking the same lock waits for that handoff to finish before its owner dies.
    FScopeLock Lock(&RequestIntakeMutex);
    OnRequestReceived.Unbind();
}

bool FSocketHttpServer::DrainPendingWrites(double TimeoutSeconds)
{
    // All shutdown responses are enqueued before this method is called. Only now
    // may the I/O thread FIN otherwise-idle keep-alive sockets; doing so during
    // QuiesceRequestIntake could close the write side before abandonment replies
    // have been produced.
    if (bAcceptingRequests.load(std::memory_order_acquire))
    {
        return false;
    }

    bCloseConnectionsAfterDrain.store(true, std::memory_order_release);
    const uint64 DrainId =
        LastRequestedDrainId.fetch_add(1, std::memory_order_acq_rel) + 1;
    const double DeadlineSeconds =
        FPlatformTime::Seconds() + FMath::Max(0.0, TimeoutSeconds);
    do
    {
        if (LastCompletedDrainId.load(std::memory_order_acquire) >= DrainId)
        {
            return true;
        }
        FPlatformProcess::SleepNoStats(kIdleSleepSeconds);
    }
    while (FPlatformTime::Seconds() < DeadlineSeconds);

    return LastCompletedDrainId.load(std::memory_order_acquire) >= DrainId;
}

void FSocketHttpServer::Tick(float DeltaTime)
{
    TickClockSeconds.store(
        TickClockSeconds.load(std::memory_order_relaxed) + static_cast<double>(DeltaTime),
        std::memory_order_relaxed);
    ProcessCompletionTimeouts();
    SendHeartbeats();
}

void FSocketHttpServer::SetEditorReadiness(bool bInEditorReady,
                                            bool bInEditorLoadingPackage,
                                            bool bInEditorSelectionSetAvailable)
{
    bEditorReady.store(bInEditorReady, std::memory_order_release);
    bEditorLoadingPackage.store(bInEditorLoadingPackage, std::memory_order_release);
    bEditorSelectionSetAvailable.store(
        bInEditorSelectionSetAvailable, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// I/O thread
// ---------------------------------------------------------------------------

void FSocketHttpServer::RunIoThread()
{
    ISocketSubsystem* Sub = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!Sub || !ListenSocket)
    {
        return;
    }

    while (!bStopRequested.load())
    {
        bool bDidWork = false;
        const uint64 DrainRequestId =
            LastRequestedDrainId.load(std::memory_order_acquire);
        const bool bAcceptingRequestsNow =
            bAcceptingRequests.load(std::memory_order_acquire);

        if (bAcceptingRequestsNow)
        {
            AcceptPendingConnections(*Sub, bDidWork);
        }

        for (int32 Index = Connections.Num() - 1; Index >= 0; --Index)
        {
            const TSharedPtr<FConnection> Conn = Connections[Index];
            bool bAlive = PumpConnectionRead(*Conn, *Sub, bDidWork);
            if (bAlive && Conn->bLingering)
            {
                // Half-closed: the response and FIN are out; inbound bytes are read
                // and discarded until the peer closes or the grace window expires.
                Conn->InBuffer.Reset();
                if (FPlatformTime::Seconds() >= Conn->LingerDeadlineSeconds)
                {
                    bAlive = false;
                }
            }
            else if (bAlive)
            {
                if (bAcceptingRequestsNow)
                {
                    PumpParse(Conn);
                }
                else if (bCloseConnectionsAfterDrain.load(std::memory_order_acquire))
                {
                    Conn->bCloseAfterFlush.store(true);
                }
                bAlive = FlushConnectionWrites(*Conn, *Sub, bDidWork);

                const bool bFlushedAndClosing = Conn->bCloseAfterFlush.load()
                    && Conn->OutQueue.IsEmpty() && Conn->PendingOut.Num() == 0;
                if (bAlive && bFlushedAndClosing)
                {
                    // Close via FIN-then-drain instead of an immediate close():
                    // destroying a socket that still has unread inbound bytes (e.g.
                    // a request body in flight behind an early 413) sends RST, which
                    // discards the just-sent response on the peer's side.
                    Conn->Socket->Shutdown(ESocketShutdownMode::Write);
                    Conn->bLingering = true;
                    Conn->LingerDeadlineSeconds = FPlatformTime::Seconds() + kCloseLingerSeconds;
                    Conn->LingerAbortSeconds = FPlatformTime::Seconds() + kMaxLingerSeconds;
                    bDidWork = true;
                }
            }

            if (!bAlive)
            {
                TeardownConnection(Conn, *Sub);
                Connections.RemoveAt(Index);
                bDidWork = true;
            }
        }

        // A shutdown drain is not complete at queue-empty: the FIN/linger phase
        // must finish so unread inbound bytes cannot turn close() into an RST that
        // discards the response already handed to the socket.
        if (bCloseConnectionsAfterDrain.load(std::memory_order_acquire)
            && Connections.Num() == 0)
        {
            LastCompletedDrainId.store(DrainRequestId, std::memory_order_release);
        }

        if (!bDidWork)
        {
            FPlatformProcess::SleepNoStats(kIdleSleepSeconds);
        }
    }

    // Tear the remaining connections down here so every socket op stays on this thread.
    for (const TSharedPtr<FConnection>& Conn : Connections)
    {
        TeardownConnection(Conn, *Sub);
    }
    Connections.Empty();
}

void FSocketHttpServer::AcceptPendingConnections(ISocketSubsystem& Sub, bool& bDidWork)
{
    bool bHasPending = false;
    while (!bStopRequested.load(std::memory_order_acquire)
           && bAcceptingRequests.load(std::memory_order_acquire)
           && ListenSocket->HasPendingConnection(bHasPending) && bHasPending)
    {
        FSocket* Incoming = ListenSocket->Accept(TEXT("PinWrightSocketHttpConnection"));
        if (!Incoming)
        {
            break;
        }
        bDidWork = true;
        Incoming->SetNonBlocking(true);
        Incoming->SetNoDelay(true);

        if (Connections.Num() >= kMaxConnections)
        {
            // Runaway backstop: refuse beyond the cap with an immediate 503. The
            // response is tiny, so the fresh socket's send buffer takes it whole.
            const TArray<uint8> Reject = BuildHttpResponseBytes(
                503, TEXT("text/plain"), TEXT("Too many concurrent connections."), false);
            int32 BytesSent = 0;
            Incoming->Send(Reject.GetData(), Reject.Num(), BytesSent);
            Incoming->Close();
            Sub.DestroySocket(Incoming);
            UE_LOG(LogSocketHttp, Warning,
                   TEXT("Rejected connection: %d concurrent connection cap reached."), kMaxConnections);
            continue;
        }

        TSharedPtr<FConnection> Conn = MakeShared<FConnection>();
        Conn->Socket = Incoming;
        Connections.Add(Conn);
        UE_LOG(LogSocketHttp, Verbose, TEXT("Accepted connection (%d live)."), Connections.Num());
    }
}

bool FSocketHttpServer::PumpConnectionRead(FConnection& Conn, ISocketSubsystem& Sub, bool& bDidWork)
{
    if (!Conn.Socket)
    {
        return false;
    }

    int32 ReadBudgetBytes = kMaxReadBytesPerPump;
    uint32 PendingSize = 0;
    while (!bStopRequested.load(std::memory_order_acquire)
           && ReadBudgetBytes > 0
           && Conn.Socket->HasPendingData(PendingSize) && PendingSize > 0)
    {
        const int32 ReadSize = static_cast<int32>(FMath::Min<uint32>(
            PendingSize, static_cast<uint32>(ReadBudgetBytes)));
        const int32 Offset = Conn.InBuffer.Num();
        Conn.InBuffer.AddUninitialized(ReadSize);
        int32 BytesRead = 0;
        const bool bOk = Conn.Socket->Recv(Conn.InBuffer.GetData() + Offset,
                                           ReadSize, BytesRead);
        Conn.InBuffer.SetNum(Offset + FMath::Max(BytesRead, 0));
        if (!bOk)
        {
            return false; // graceful FIN or hard error — either way the stream is done
        }
        if (BytesRead == 0)
        {
            break; // would-block race after HasPendingData; nothing consumed this pass
        }
        ReadBudgetBytes -= BytesRead;
        bDidWork = true;
    }

    // A doomed connection (reject sent / close pending / lingering) discards its
    // inbound bytes: they will never be parsed, and letting them accumulate would
    // trip the flood backstop below and hard-close the socket (RST) while the
    // just-sent response (e.g. an early 413 with the request body still in
    // flight) may not have been read on the peer's side yet.
    if (!bAcceptingRequests.load(std::memory_order_acquire)
        || Conn.bInputBroken || Conn.bLingering || Conn.bCloseAfterFlush.load())
    {
        // A lingering peer that is still streaming has not processed our FIN (its
        // request body is still in flight) — closing now would RST and discard the
        // just-sent response before the peer reads it. Push the idle grace window
        // back while bytes keep arriving, bounded by the absolute linger cap.
        if (Conn.bLingering && Conn.InBuffer.Num() > 0)
        {
            Conn.LingerDeadlineSeconds = FMath::Min(
                FPlatformTime::Seconds() + kCloseLingerSeconds, Conn.LingerAbortSeconds);
        }
        Conn.InBuffer.Reset();
    }
    // Flood backstop: a client must never park more than one max-size request plus
    // headers while a response is outstanding.
    else if (Conn.InBuffer.Num() > MaxBodyBytes + kMaxHeaderBytes)
    {
        return false;
    }

    // No readable bytes: peek one byte to distinguish idle from remote close.
    // FSocketBSD::Recv on a STREAM socket returns true with zero bytes for
    // EWOULDBLOCK (idle, socket healthy) and false for a graceful FIN or a hard
    // error — so a false return means no byte will ever arrive again.
    uint8 Probe = 0;
    int32 ProbeRead = 0;
    return Conn.Socket->Recv(&Probe, 1, ProbeRead, ESocketReceiveFlags::Peek);
}

void FSocketHttpServer::PumpParse(const TSharedPtr<FConnection>& Conn)
{
    // Loop handles pipelined keep-alive requests; it stalls while a dispatched RPC is
    // outstanding (responses must go out in request order) and stops for good once the
    // connection is streaming or its input is broken.
    while (bAcceptingRequests.load(std::memory_order_acquire)
           && !Conn->bInputBroken
           && !Conn->bStreaming.load()
           && !Conn->bAwaitingResponse.load()
           && !Conn->bCloseAfterFlush.load())
    {
        if (!Conn->bHaveHeaders)
        {
            const int32 HeaderEnd = FindHeaderTerminator(Conn->InBuffer);
            if (HeaderEnd == INDEX_NONE)
            {
                if (Conn->InBuffer.Num() > kMaxHeaderBytes)
                {
                    RejectAndClose(*Conn, 400, TEXT("Header block too large."));
                }
                return;
            }

            const FString HeadText = Utf8BytesToString(Conn->InBuffer.GetData(), HeaderEnd);
            Conn->InBuffer.RemoveAt(0, HeaderEnd + 4);

            FString ParseError;
            if (!ParseRequestHead(*Conn, HeadText, ParseError))
            {
                RejectAndClose(*Conn, 400, ParseError);
                return;
            }

            if (Conn->ContentLength > MaxBodyBytes)
            {
                RejectAndClose(*Conn, 413, TEXT("Request body is too large."));
                return;
            }

            // Interim 100 before the client commits to sending the body.
            if (const FString* Expect = Conn->Headers.Find(TEXT("expect")))
            {
                if (Expect->Contains(TEXT("100-continue")))
                {
                    EnqueueBytes(*Conn, BuildHttpResponseBytes(100, TEXT("text/plain"), FString(), true));
                }
            }

            Conn->bHaveHeaders = true;
        }

        if (Conn->InBuffer.Num() < Conn->ContentLength)
        {
            return; // body still arriving (possibly split across many recv chunks)
        }

        FString BodyString;
        if (Conn->ContentLength > 0)
        {
            BodyString = Utf8BytesToString(Conn->InBuffer.GetData(), Conn->ContentLength);
            Conn->InBuffer.RemoveAt(0, Conn->ContentLength);
        }

        HandleCompleteRequest(Conn, BodyString);

        // Reset the state machine for the next request on this keep-alive socket.
        Conn->bHaveHeaders = false;
        Conn->Headers.Reset();
        Conn->Method.Reset();
        Conn->Path.Reset();
        Conn->ContentLength = 0;
    }
}

bool FSocketHttpServer::FlushConnectionWrites(FConnection& Conn, ISocketSubsystem& Sub, bool& bDidWork)
{
    TArray<uint8> Chunk;
    while (Conn.OutQueue.Dequeue(Chunk))
    {
        Conn.PendingOut.Append(Chunk);
    }

    if (Conn.PendingOut.Num() == 0)
    {
        return true;
    }
    if (!Conn.Socket)
    {
        return false;
    }

    int32 BytesSent = 0;
    const bool bOk = Conn.Socket->Send(Conn.PendingOut.GetData(), Conn.PendingOut.Num(), BytesSent);
    if (!bOk)
    {
        const ESocketErrors Err = Sub.GetLastErrorCode();
        if (Err != SE_EWOULDBLOCK && Err != SE_TRY_AGAIN)
        {
            return false; // dead socket — caller tears the connection down
        }
        BytesSent = 0;
    }
    if (BytesSent > 0)
    {
        Conn.PendingOut.RemoveAt(0, BytesSent);
        bDidWork = true;
    }
    return true;
}

void FSocketHttpServer::TeardownConnection(const TSharedPtr<FConnection>& Conn, ISocketSubsystem& Sub)
{
    Conn->bDead.store(true);
    if (Conn->Socket)
    {
        Conn->Socket->Close();
        Sub.DestroySocket(Conn->Socket);
        Conn->Socket = nullptr;
    }
    DropCompletionsForConnection(Conn.Get());
}

// ---------------------------------------------------------------------------
// Request handling (I/O thread)
// ---------------------------------------------------------------------------

bool FSocketHttpServer::ParseRequestHead(FConnection& Conn, const FString& HeadText, FString& OutError)
{
    TArray<FString> Lines;
    HeadText.ParseIntoArray(Lines, TEXT("\r\n"), true);
    if (Lines.Num() == 0)
    {
        OutError = TEXT("Missing request line.");
        return false;
    }

    TArray<FString> Tokens;
    Lines[0].ParseIntoArray(Tokens, TEXT(" "), true);
    if (Tokens.Num() < 3 || !Tokens[2].StartsWith(TEXT("HTTP/")))
    {
        OutError = TEXT("Malformed request line.");
        return false;
    }
    Conn.Method = Tokens[0];
    Conn.Path = Tokens[1];
    int32 QueryIndex = INDEX_NONE;
    if (Conn.Path.FindChar(TEXT('?'), QueryIndex))
    {
        Conn.Path.LeftInline(QueryIndex);
    }
    const bool bHttp10 = Tokens[2] == TEXT("HTTP/1.0");

    Conn.Headers.Reset();
    for (int32 Index = 1; Index < Lines.Num(); ++Index)
    {
        FString Key;
        FString Value;
        if (!Lines[Index].Split(TEXT(":"), &Key, &Value))
        {
            OutError = TEXT("Malformed header line.");
            return false;
        }
        Key = Key.TrimStartAndEnd().ToLower();
        Value = Value.TrimStartAndEnd();
        if (Key.IsEmpty())
        {
            OutError = TEXT("Malformed header line.");
            return false;
        }
        if (FString* Existing = Conn.Headers.Find(Key))
        {
            // Keep duplicate Authorization values independently parseable by the
            // constant-time bearer-token loop. Other duplicate fields retain normal
            // comma-list semantics.
            *Existing += Key == TEXT("authorization") ? TEXT("\n") : TEXT(", ");
            *Existing += Value;
        }
        else
        {
            Conn.Headers.Add(MoveTemp(Key), MoveTemp(Value));
        }
    }

    if (Conn.Headers.Contains(TEXT("transfer-encoding")))
    {
        OutError = TEXT("Transfer-Encoding is not supported; send Content-Length.");
        return false;
    }

    Conn.ContentLength = 0;
    if (const FString* ContentLengthValue = Conn.Headers.Find(TEXT("content-length")))
    {
        if (!IsAllDigits(*ContentLengthValue))
        {
            OutError = TEXT("Invalid Content-Length.");
            return false;
        }
        const int64 Parsed = FCString::Atoi64(**ContentLengthValue);
        if (Parsed < 0 || Parsed > static_cast<int64>(TNumericLimits<int32>::Max()))
        {
            OutError = TEXT("Invalid Content-Length.");
            return false;
        }
        Conn.ContentLength = static_cast<int32>(Parsed);
    }

    // Persistence: HTTP/1.1 defaults to keep-alive, HTTP/1.0 to close.
    FString ConnectionHeader;
    if (const FString* Header = Conn.Headers.Find(TEXT("connection")))
    {
        ConnectionHeader = Header->ToLower();
    }
    Conn.bKeepAlive = bHttp10
        ? ConnectionHeader.Contains(TEXT("keep-alive"))
        : !ConnectionHeader.Contains(TEXT("close"));

    return true;
}

void FSocketHttpServer::HandleCompleteRequest(const TSharedPtr<FConnection>& Conn, const FString& BodyString)
{
    const bool bKeepAlive = Conn->bKeepAlive;

    // Enqueues a synchronous response and, on a non-persistent request, arranges the
    // close AFTER the bytes are queued — setting the flag with an empty outbound queue
    // would let the I/O loop tear the connection down before the response exists.
    auto RespondNow = [&Conn, bKeepAlive](TArray<uint8>&& Bytes)
    {
        EnqueueBytes(*Conn, MoveTemp(Bytes));
        if (!bKeepAlive)
        {
            Conn->bCloseAfterFlush.store(true);
        }
    };

    UE_LOG(LogSocketHttp, Verbose, TEXT("%s %s (%d body bytes)"),
           *Conn->Method, *Conn->Path, BodyString.Len());

    // Cross-origin gate: browser-originated requests must come from loopback pages.
    if (const FString* Origin = Conn->Headers.Find(TEXT("origin")))
    {
        if (!Origin->IsEmpty() && !IsAllowedOrigin(*Origin))
        {
            RespondNow(BuildHttpResponseBytes(
                403, TEXT("text/plain"), TEXT("Forbidden: non-loopback Origin."), bKeepAlive));
            return;
        }
    }

    // Prompt 404 for anything that isn't /mcp — this covers Codex's pre-initialize
    // OAuth `.well-known` GET probes, which must fail fast instead of hanging.
    if (Conn->Path != TEXT("/mcp"))
    {
        RespondNow(BuildHttpResponseBytes(
            404, TEXT("text/plain"), TEXT("Not found. The MCP endpoint is POST /mcp."), bKeepAlive));
        return;
    }
    if (Conn->Method != TEXT("POST"))
    {
        RespondNow(BuildHttpResponseBytes(
            405, TEXT("text/plain"), TEXT("Method not allowed. Use POST /mcp."), bKeepAlive,
            TEXT("Allow: POST\r\n")));
        return;
    }

    FString AuthHeader;
    if (const FString* Auth = Conn->Headers.Find(TEXT("authorization")))
    {
        AuthHeader = *Auth;
    }

    // Shared validation + protocol routing (auth, size, envelope parse, the five
    // protocol methods, tools/call shape routing) lives in McpRequestCore. The
    // editor-readiness fields are an atomic game-thread snapshot: no editor API is
    // touched from this I/O thread.
    McpRequestCore::FRequestDecision Decision;
    McpRequestCore::FRequestConfig RequestConfig =
        McpRequestCore::FRequestConfig::FromSettings();
    RequestConfig.bEditorReady = bEditorReady.load(std::memory_order_acquire);
    RequestConfig.bEditorLoadingPackage =
        bEditorLoadingPackage.load(std::memory_order_acquire);
    RequestConfig.bEditorSelectionSetAvailable =
        bEditorSelectionSetAvailable.load(std::memory_order_acquire);

    // The readiness snapshot above is written by the game thread, so it freezes -
    // stale-but-positive - the moment anything owns that thread. ModalStateProbe's
    // two probes are the signals that keep moving: the modal latch, written from
    // inside the nested Slate loop, and the heartbeat, whose AGE is measurable
    // precisely because nothing is writing it. Reading them here lets this I/O
    // thread answer truthfully while nothing game-thread-driven can. Below the
    // report thresholds the fields stay false/0/empty and every response is
    // byte-identical to the healthy case.
    {
        const UPinWrightSettings* Settings = GetDefault<UPinWrightSettings>();
        const double ReportThresholdSeconds = Settings
            ? static_cast<double>(Settings->ModalBlockedReportSeconds)
            : 2.0;
        if (ModalStateProbe::ShouldReportBlocked(ReportThresholdSeconds))
        {
            RequestConfig.bBlockedOnModal = true;
            RequestConfig.BlockedOnModalSeconds = ModalStateProbe::BlockedSeconds();
            RequestConfig.ModalTitle = ModalStateProbe::GetModalTitle();
        }

        // The modal latch above covers only the cause that broadcasts a Slate event.
        // A handler that simply never returns broadcasts nothing, so the heartbeat's
        // AGE is the only evidence - and reading it here, on the I/O thread, is what
        // stops `ping` answering editorReady:true through the wedge. Both probes are
        // read unconditionally; BuildPingResult ranks them (modal wins, being the
        // more specific and the non-retryable one). Below the threshold the fields
        // stay false/0/empty and every response is byte-identical.
        const double StallThresholdSeconds = Settings
            ? static_cast<double>(Settings->GameThreadStallReportSeconds)
            : 90.0;
        if (ModalStateProbe::ShouldReportGameThreadStalled(StallThresholdSeconds))
        {
            RequestConfig.bGameThreadStalled = true;
            RequestConfig.GameThreadStalledSeconds = ModalStateProbe::GameThreadStalledSeconds();
            ModalStateProbe::GetInFlightRpc(RequestConfig.InFlightMethod,
                                            RequestConfig.InFlightRequestId,
                                            RequestConfig.InFlightSeconds);
        }
    }

    const bool bAccepted = McpRequestCore::ProcessRequestBody(
        BodyString, AuthHeader, Decision, RequestConfig);

    if (!bAccepted
        || Decision.Kind == McpRequestCore::FRequestDecision::EKind::ImmediateResponse)
    {
        const FString Json = SerializeCondensed(Decision.ImmediateBody);
        const FString Extra = Decision.HttpCode == 401
            ? FString(TEXT("WWW-Authenticate: Bearer\r\n"))
            : FString();
        RespondNow(BuildHttpResponseBytes(
            Decision.HttpCode, TEXT("application/json"), Json, bKeepAlive, Extra));
        return;
    }

    // Serialize the final dispatch handoff with shutdown. Quiescing first flips
    // the atomic gate, then takes this lock, so no owner callback can begin after
    // QuiesceRequestIntake returns.
    FScopeLock IntakeLock(&RequestIntakeMutex);
    if (!bAcceptingRequests.load(std::memory_order_acquire))
    {
        const TSharedPtr<FJsonObject> Err = JsonRpc::BuildErrorResponse(
            Decision.EnvelopeId, JsonRpc::kInternalError,
            TEXT("Automation bridge shutting down."));
        RespondNow(BuildHttpResponseBytes(
            200, TEXT("application/json"), SerializeCondensed(Err), bKeepAlive));
        return;
    }

    // DispatchRpc: register a completion and hand the request to the dispatcher
    // (which marshals to the game thread itself).
    if (!OnRequestReceived.IsBound())
    {
        const TSharedPtr<FJsonObject> Err = JsonRpc::BuildErrorResponse(
            Decision.EnvelopeId, JsonRpc::kInternalError, TEXT("No dispatcher bound."));
        RespondNow(BuildHttpResponseBytes(
            200, TEXT("application/json"), SerializeCondensed(Err), bKeepAlive));
        return;
    }

    const bool bStream = ShouldStreamRequest(*Conn, Decision);
    const FString RequestId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
    const double TimeoutSeconds =
        static_cast<double>(bStream ? MaxTimeoutMs : DefaultTimeoutMs) / 1000.0;

    const TWeakPtr<FConnection> WeakConn = Conn;
    const TSharedPtr<FJsonValue> EnvelopeId = Decision.EnvelopeId;
    const FString DispatchMethod = Decision.Method;
    const int32 Spill = SpillThresholdCharacters;

    FTransportCompletionCallback Callback =
        [WeakConn, EnvelopeId, DispatchMethod, bStream, bKeepAlive, Spill](
            const FString&, bool bSuccess, const FString& Message,
            const TSharedPtr<FJsonObject>& Result, const FString& ErrorCode)
        {
            TSharedPtr<FConnection> Pinned = WeakConn.Pin();
            if (!Pinned.IsValid() || Pinned->bDead.load())
            {
                return; // client went away; the result is dropped silently
            }
            const TSharedPtr<FJsonObject> Response = McpRequestCore::WrapToolResult(
                bSuccess, Message, Result, ErrorCode, EnvelopeId, Spill, DispatchMethod);
            const FString Json = SerializeCondensed(Response);
            if (bStream)
            {
                // Final JSON-RPC response rides as the last SSE frame, then the
                // stream ends by connection close.
                EnqueueBytes(*Pinned, BuildSseMessageFrame(Json));
                Pinned->bCloseAfterFlush.store(true);
            }
            else
            {
                EnqueueBytes(*Pinned, BuildHttpResponseBytes(
                    200, TEXT("application/json"), Json, bKeepAlive));
                if (!bKeepAlive)
                {
                    Pinned->bCloseAfterFlush.store(true);
                }
            }
            Pinned->bAwaitingResponse.store(false);
        };

    Conn->bAwaitingResponse.store(true);
    if (!RegisterCompletion(RequestId, MoveTemp(Callback), TimeoutSeconds,
                            Decision.Method, bStream, WeakConn, Decision.ProgressToken))
    {
        Conn->bAwaitingResponse.store(false);
        const TSharedPtr<FJsonObject> Err = JsonRpc::BuildErrorResponse(
            Decision.EnvelopeId, JsonRpc::kInternalError,
            TEXT("Unable to register transport completion."));
        RespondNow(BuildHttpResponseBytes(
            200, TEXT("application/json"), SerializeCondensed(Err), bKeepAlive));
        return;
    }

    if (bStream)
    {
        // Commit to SSE now: headers go out immediately so the client sees the stream
        // open before the first progress frame.
        Conn->bStreaming.store(true);
        EnqueueBytes(*Conn, BuildSseResponseHeadBytes());
        UE_LOG(LogSocketHttp, Verbose, TEXT("Streaming request %s (method=%s)."),
               *RequestId, *Decision.Method);
    }

    // Park the caller id against this request id before the handoff. The delegate
    // below carries only (RequestId, Method, Args), so this is the last point at
    // which the request headers are still in scope, and the id is what lets
    // editor.quit tell its own caller's traffic from another agent's. Absent
    // header -> empty id -> the anonymous bucket. See State/ClientActivity.h.
    ClientActivity::NoteRequestClient(
        RequestId, Conn->Headers.FindRef(FString(ClientActivity::ClientIdHeader)));

    const TSharedPtr<FJsonObject> Args =
        Decision.Args.IsValid() ? Decision.Args : MakeShared<FJsonObject>();
    OnRequestReceived.Execute(RequestId, Decision.Method, Args);
}

bool FSocketHttpServer::ShouldStreamRequest(
    const FConnection& Conn,
    const McpRequestCore::FRequestDecision& Decision) const
{
    // Gate: Accept opts into SSE, the client attached a progress token to
    // receive notifications, and the caller did not decline blocking with an
    // explicit args.wait == false — block-and-stream is the default.
    const FString* Accept = Conn.Headers.Find(TEXT("accept"));
    if (!Accept || !Accept->Contains(TEXT("text/event-stream")))
    {
        return false;
    }

    if (!Decision.bWaitAccepted)
    {
        return false;
    }

    return Decision.bHasProgressToken;
}

void FSocketHttpServer::RejectAndClose(FConnection& Conn, int32 Code, const FString& Message)
{
    EnqueueBytes(Conn, BuildHttpResponseBytes(Code, TEXT("text/plain"), Message, false));
    Conn.bInputBroken = true; // cannot resync a broken byte stream — close after flush
    Conn.bCloseAfterFlush.store(true);
}

void FSocketHttpServer::EnqueueBytes(FConnection& Conn, TArray<uint8>&& Bytes)
{
    if (!Conn.bDead.load())
    {
        Conn.OutQueue.Enqueue(MoveTemp(Bytes));
    }
}

// ---------------------------------------------------------------------------
// Completion registry (game thread + I/O thread teardown, mutex-guarded)
// ---------------------------------------------------------------------------

bool FSocketHttpServer::RegisterCompletion(const FString& RequestId,
                                           FTransportCompletionCallback Callback,
                                           double TimeoutSeconds, const FString& Method,
                                           bool bStreaming,
                                           const TWeakPtr<FConnection>& Connection,
                                           const TSharedPtr<FJsonValue>& ProgressToken)
{
    if (RequestId.IsEmpty() || !Callback)
    {
        return false;
    }

    FPendingCompletion Pending;
    Pending.Callback = MoveTemp(Callback);
    Pending.Method = Method;
    Pending.bStreaming = bStreaming;
    Pending.Connection = Connection;
    Pending.ProgressToken = ProgressToken;
    Pending.TimeoutSeconds = static_cast<float>(TimeoutSeconds);
    Pending.LastStreamActivitySeconds = TickClockSeconds.load(std::memory_order_relaxed);
    if (TimeoutSeconds <= 0.0)
    {
        Pending.DeadlineSeconds = TNumericLimits<double>::Max();
    }
    else
    {
        Pending.DeadlineSeconds = FPlatformTime::Seconds() + FMath::Max(0.1, TimeoutSeconds);
    }

    FScopeLock Lock(&CompletionMutex);
    PendingCompletions.Add(RequestId, MoveTemp(Pending));
    return true;
}

bool FSocketHttpServer::ResolveCompletion(const FString& RequestId, bool bSuccess,
                                          const FString& Message,
                                          const TSharedPtr<FJsonObject>& Result,
                                          const FString& ErrorCode)
{
    FPendingCompletion Pending;
    {
        FScopeLock Lock(&CompletionMutex);
        if (!PendingCompletions.RemoveAndCopyValue(RequestId, Pending))
        {
            return false;
        }
    }

    if (Pending.Callback)
    {
        Pending.Callback(RequestId, bSuccess, Message, Result, ErrorCode);
    }
    return true;
}

bool FSocketHttpServer::WriteStreamFrame(const FString& RequestId,
                                         const TSharedRef<FJsonObject>& Notification)
{
    FScopeLock Lock(&CompletionMutex);
    FPendingCompletion* Entry = PendingCompletions.Find(RequestId);
    if (!Entry || !Entry->bStreaming)
    {
        return false;
    }
    TSharedPtr<FConnection> Pinned = Entry->Connection.Pin();
    if (!Pinned.IsValid() || Pinned->bDead.load())
    {
        return false;
    }

    TSharedPtr<FJsonObject> NotificationObj = Notification;
    EnqueueBytes(*Pinned, BuildSseMessageFrame(SerializeCondensed(NotificationObj)));

    // A live (emitting) stream refreshes its own deadline so the sweep only reaps
    // streams that have gone idle past the max timeout. The timeout deadline runs on
    // the wall clock; the heartbeat cadence runs on the tick clock.
    Entry->LastStreamActivitySeconds = TickClockSeconds.load(std::memory_order_relaxed);
    if (Entry->TimeoutSeconds > 0.0f)
    {
        Entry->DeadlineSeconds = FPlatformTime::Seconds() + Entry->TimeoutSeconds;
    }
    return true;
}

bool FSocketHttpServer::IsStreamingRequest(const FString& RequestId) const
{
    FScopeLock Lock(&CompletionMutex);
    const FPendingCompletion* Entry = PendingCompletions.Find(RequestId);
    return Entry && Entry->bStreaming;
}

bool FSocketHttpServer::GetProgressToken(const FString& RequestId,
                                         TSharedPtr<FJsonValue>& OutToken) const
{
    FScopeLock Lock(&CompletionMutex);
    const FPendingCompletion* Entry = PendingCompletions.Find(RequestId);
    if (!Entry || !Entry->ProgressToken.IsValid())
    {
        return false;
    }
    OutToken = Entry->ProgressToken;
    return true;
}

void FSocketHttpServer::ProcessCompletionTimeouts()
{
    const double NowSeconds = FPlatformTime::Seconds();
    TArray<FPendingCompletion> TimedOutCallbacks;
    TArray<FString> TimedOutRequestIds;

    {
        FScopeLock Lock(&CompletionMutex);
        for (auto It = PendingCompletions.CreateIterator(); It; ++It)
        {
            if (It.Value().DeadlineSeconds <= NowSeconds)
            {
                TimedOutRequestIds.Add(It.Key());
                TimedOutCallbacks.Add(MoveTemp(It.Value()));
                It.RemoveCurrent();
            }
        }
    }

    // For a streaming entry the callback writes the timeout as a terminal error frame
    // and closes the stream; buffered entries get the usual JSON error response.
    for (int32 Index = 0; Index < TimedOutCallbacks.Num(); ++Index)
    {
        if (!TimedOutCallbacks[Index].Callback)
        {
            continue;
        }
        const FString& Method = TimedOutCallbacks[Index].Method;
        const float TimeoutSecs = TimedOutCallbacks[Index].TimeoutSeconds;
        TimedOutCallbacks[Index].Callback(
            TimedOutRequestIds[Index], false,
            FString::Printf(TEXT("Request timed out after %.0fs (method=%s)."), TimeoutSecs, *Method),
            nullptr, TEXT("TIMEOUT"));
    }
}

void FSocketHttpServer::SendHeartbeats()
{
    const UPinWrightSettings* Settings = GetDefault<UPinWrightSettings>();
    const int32 HeartbeatSeconds = Settings ? Settings->SseHeartbeatSeconds : 15;
    if (HeartbeatSeconds <= 0)
    {
        return;
    }

    // Tick clock, not wall clock: heartbeat idleness is measured in accumulated Tick
    // deltas so tests can advance it synthetically (real ticker ~= wall time anyway).
    const double Now = TickClockSeconds.load(std::memory_order_relaxed);
    FScopeLock Lock(&CompletionMutex);
    for (TPair<FString, FPendingCompletion>& Pair : PendingCompletions)
    {
        FPendingCompletion& Entry = Pair.Value;
        if (!Entry.bStreaming
            || Now - Entry.LastStreamActivitySeconds < static_cast<double>(HeartbeatSeconds))
        {
            continue;
        }
        TSharedPtr<FConnection> Pinned = Entry.Connection.Pin();
        if (Pinned.IsValid() && !Pinned->bDead.load())
        {
            // Comment frame only — it resets the heartbeat clock but deliberately not
            // the timeout deadline, so an idle stream still times out eventually.
            EnqueueBytes(*Pinned, BuildSseHeartbeatFrame());
            Entry.LastStreamActivitySeconds = Now;
        }
    }
}

void FSocketHttpServer::FailAllCompletions(const FString& Message, const FString& ErrorCode)
{
    TArray<FPendingCompletion> PendingCallbacks;
    TArray<FString> RequestIds;
    {
        FScopeLock Lock(&CompletionMutex);
        for (TPair<FString, FPendingCompletion>& Pair : PendingCompletions)
        {
            RequestIds.Add(Pair.Key);
            PendingCallbacks.Add(MoveTemp(Pair.Value));
        }
        PendingCompletions.Empty();
    }

    for (int32 Index = 0; Index < PendingCallbacks.Num(); ++Index)
    {
        if (!PendingCallbacks[Index].Callback)
        {
            continue;
        }
        PendingCallbacks[Index].Callback(RequestIds[Index], false, Message, nullptr, ErrorCode);
    }
}

void FSocketHttpServer::DropCompletionsForConnection(const FConnection* Conn)
{
    FScopeLock Lock(&CompletionMutex);
    for (auto It = PendingCompletions.CreateIterator(); It; ++It)
    {
        const TSharedPtr<FConnection> Pinned = It.Value().Connection.Pin();
        if (!Pinned.IsValid() || Pinned.Get() == Conn)
        {
            // Client is gone: the job keeps running server-side, its result just has
            // no reader anymore. Drop silently — no callback invocation.
            UE_LOG(LogSocketHttp, Verbose,
                   TEXT("Dropping completion for dead connection (request %s)."), *It.Key());
            It.RemoveCurrent();
        }
    }
}
