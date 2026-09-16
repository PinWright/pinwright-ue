// Copyright (c) 2026 Alexander Penkin. MIT License.

// Test-side raw TCP client + HTTP/SSE wire helpers for the FSocketHttpServer
// integration tests under Tests/Transport/. Named namespace (not anonymous) so
// Unity builds can merge the transport test TUs without ODR collisions.
//
// Every wait loop here is deadline-bounded: a stuck server yields a helper
// `false` (a test FAILURE), never a hung automation run. Wait loops pump
// FSocketHttpServer::Tick plus the game-thread task queues so both a
// single-threaded (Tick-driven) and an I/O-thread server design make progress
// while the test blocks on the game thread.
#pragma once

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "Containers/Ticker.h"
#include "Async/TaskGraphInterfaces.h"
#include "PinWrightSettings.h"
#include "Utils/GatewayAuthToken.h"
#include "Transport/SocketHttpServer.h"

namespace PinWrightSocketTest
{
    // Hard wall-clock bailout for every recv/wait loop. A server that never
    // answers turns into an assertion failure at this deadline, not a hang.
    constexpr double kDefaultTimeoutSeconds = 5.0;

    // ------------------------------------------------------------------
    // Pumping
    // ------------------------------------------------------------------

    // One pump iteration: advances the server (Tick-driven designs), the core
    // ticker, and the game-thread task queue (I/O-thread designs marshal work
    // here), then yields briefly so a real I/O thread gets CPU time.
    inline void PumpOnce(FSocketHttpServer* Server, float DeltaTime = 0.01f)
    {
        if (Server)
        {
            Server->Tick(DeltaTime);
        }
        FTSTicker::GetCoreTicker().Tick(DeltaTime);
        FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
        FPlatformProcess::Sleep(0.001f);
    }

    // Pumps until Cond() is true or the deadline passes. Returns the final
    // Cond() value so callers TestTrue() the wait itself.
    inline bool WaitForCondition(TFunctionRef<bool()> Cond, FSocketHttpServer* Server,
        double TimeoutSeconds = kDefaultTimeoutSeconds)
    {
        const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
        while (FPlatformTime::Seconds() < Deadline)
        {
            if (Cond())
            {
                return true;
            }
            PumpOnce(Server);
        }
        return Cond();
    }

    // ------------------------------------------------------------------
    // Server lifecycle
    // ------------------------------------------------------------------

    // Try-bind loop on ephemeral high ports so parallel editors / leftover
    // listeners never collide with the test server.
    inline TSharedPtr<FSocketHttpServer> StartServerOnFreePort(uint32& OutPort)
    {
        for (int32 Attempt = 0; Attempt < 16; ++Attempt)
        {
            const uint32 Port = 29000u + static_cast<uint32>(FMath::RandRange(0, 2999));
            TSharedPtr<FSocketHttpServer> Server = MakeShared<FSocketHttpServer>();
            if (Server->Start(Port))
            {
                // Transport tests run without UPinWrightSubsystem, which normally
                // publishes the live editor-readiness snapshot every tick.
                Server->SetEditorReadiness(true, false, true);
                OutPort = Port;
                return Server;
            }
            Server->Stop();
        }
        OutPort = 0;
        return nullptr;
    }

    // Scoped mutation of UPinWrightSettings::bRequireAuthToken. The socket
    // server provisions its bearer token from settings + gateway-token at
    // Start(), so construct this BEFORE StartServerOnFreePort and keep it
    // alive for the test body; the destructor restores the prior value.
    struct FScopedAuthRequirement
    {
        bool bPrior;
        explicit FScopedAuthRequirement(bool bRequire)
        {
            UPinWrightSettings* Settings = GetMutableDefault<UPinWrightSettings>();
            bPrior = Settings->bRequireAuthToken;
            Settings->bRequireAuthToken = bRequire;
        }
        ~FScopedAuthRequirement()
        {
            GetMutableDefault<UPinWrightSettings>()->bRequireAuthToken = bPrior;
        }
    };

    // ------------------------------------------------------------------
    // Raw TCP client
    // ------------------------------------------------------------------

    enum class ERecvStep : uint8
    {
        Data,     // appended fresh bytes
        NoData,   // nothing pending (EWOULDBLOCK)
        Closed,   // peer FIN or hard socket failure — no more bytes will arrive
        Error     // no socket
    };

    // Minimal loopback stream-socket client. Blocking connect (loopback is
    // instant), then non-blocking recv so every read loop can honor a
    // wall-clock deadline. Accumulates everything received into `Received`.
    class FSocketTestClient
    {
    public:
        FSocket* Sock = nullptr;
        TArray<uint8> Received;

        ~FSocketTestClient() { Close(); }

        bool Connect(uint32 Port)
        {
            ISocketSubsystem* SS = ISocketSubsystem::Get();
            if (!SS)
            {
                return false;
            }
            Sock = SS->CreateSocket(NAME_Stream, TEXT("PinWrightSocketTestClient"), false);
            if (!Sock)
            {
                return false;
            }
            TSharedRef<FInternetAddr> Addr = SS->CreateInternetAddr();
            bool bValidIp = false;
            Addr->SetIp(TEXT("127.0.0.1"), bValidIp);
            Addr->SetPort(static_cast<int32>(Port));
            if (!bValidIp)
            {
                return false;
            }
            const bool bConnected = Sock->Connect(*Addr);
            Sock->SetNonBlocking(true);
            return bConnected;
        }

        void Close()
        {
            if (Sock)
            {
                Sock->Close();
                if (ISocketSubsystem* SS = ISocketSubsystem::Get())
                {
                    SS->DestroySocket(Sock);
                }
                Sock = nullptr;
            }
        }

        // Sends the full byte buffer, pumping the server between partial sends.
        bool SendBytes(const TArray<uint8>& Bytes, FSocketHttpServer* Server,
            double TimeoutSeconds = kDefaultTimeoutSeconds)
        {
            if (!Sock)
            {
                return false;
            }
            int32 Offset = 0;
            const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
            while (Offset < Bytes.Num())
            {
                if (FPlatformTime::Seconds() > Deadline)
                {
                    return false;
                }
                int32 Sent = 0;
                const bool bOk = Sock->Send(Bytes.GetData() + Offset, Bytes.Num() - Offset, Sent);
                if (bOk && Sent > 0)
                {
                    Offset += Sent;
                }
                else
                {
                    PumpOnce(Server);
                }
            }
            return true;
        }

        // UTF-8-encodes and sends a string (HTTP request text).
        bool SendString(const FString& Text, FSocketHttpServer* Server,
            double TimeoutSeconds = kDefaultTimeoutSeconds)
        {
            FTCHARToUTF8 Utf8(*Text);
            TArray<uint8> Bytes(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
            return SendBytes(Bytes, Server, TimeoutSeconds);
        }

        // Single non-blocking recv step; appends any fresh bytes to Received.
        // FSocketBSD::Recv on a STREAM socket returns true with zero bytes for
        // EWOULDBLOCK (idle) and false for a graceful FIN or a hard error.
        ERecvStep RecvStep()
        {
            if (!Sock)
            {
                return ERecvStep::Error;
            }
            uint8 Buf[8192];
            int32 BytesRead = 0;
            const bool bOk = Sock->Recv(Buf, sizeof(Buf), BytesRead);
            if (bOk && BytesRead > 0)
            {
                Received.Append(Buf, BytesRead);
                return ERecvStep::Data;
            }
            if (bOk)
            {
                return ERecvStep::NoData;  // EWOULDBLOCK: no data yet, socket healthy
            }
            return ERecvStep::Closed;      // graceful FIN or hard failure — no more bytes ever
        }

        // Everything received so far, decoded as UTF-8 text.
        FString ReceivedString() const
        {
            if (Received.Num() == 0)
            {
                return FString();
            }
            FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Received.GetData()), Received.Num());
            return FString(Conv.Length(), Conv.Get());
        }

        // Recv until the accumulated text contains Needle (case-sensitive).
        bool RecvUntilContains(const FString& Needle, FSocketHttpServer* Server,
            double TimeoutSeconds = kDefaultTimeoutSeconds)
        {
            const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
            while (FPlatformTime::Seconds() < Deadline)
            {
                if (ReceivedString().Contains(Needle, ESearchCase::CaseSensitive))
                {
                    return true;
                }
                const ERecvStep Step = RecvStep();
                if (Step == ERecvStep::Closed)
                {
                    break;  // no more bytes will ever arrive
                }
                if (Step != ERecvStep::Data)
                {
                    PumpOnce(Server);
                }
            }
            return ReceivedString().Contains(Needle, ESearchCase::CaseSensitive);
        }

        // Recv (draining any trailing bytes) until the peer closes, or deadline.
        bool RecvUntilClosed(FSocketHttpServer* Server,
            double TimeoutSeconds = kDefaultTimeoutSeconds)
        {
            const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
            while (FPlatformTime::Seconds() < Deadline)
            {
                const ERecvStep Step = RecvStep();
                if (Step == ERecvStep::Closed)
                {
                    return true;
                }
                if (Step != ERecvStep::Data)
                {
                    PumpOnce(Server);
                }
            }
            return false;
        }
    };

    // ------------------------------------------------------------------
    // HTTP response parsing (byte-level, keep-alive aware)
    // ------------------------------------------------------------------

    struct FParsedHttpResponse
    {
        bool bComplete = false;
        int32 Code = 0;
        FString StatusLine;
        TMap<FString, FString> Headers;  // keys lowercased; duplicates joined ", "
        FString Body;                    // UTF-8 decoded
        int32 BodyStart = 0;             // byte offset of the body in the recv buffer
        int32 BodyByteLen = 0;           // body length actually consumed, in bytes
    };

    // Byte offset of the first CRLFCRLF at/after Start, or INDEX_NONE.
    inline int32 FindHeaderEnd(const TArray<uint8>& Bytes, int32 Start = 0)
    {
        for (int32 i = FMath::Max(Start, 0); i + 3 < Bytes.Num(); ++i)
        {
            if (Bytes[i] == '\r' && Bytes[i + 1] == '\n' &&
                Bytes[i + 2] == '\r' && Bytes[i + 3] == '\n')
            {
                return i;
            }
        }
        return INDEX_NONE;
    }

    inline FString Utf8Range(const TArray<uint8>& Bytes, int32 Start, int32 Count)
    {
        if (Count <= 0 || Start < 0 || Start + Count > Bytes.Num())
        {
            return FString();
        }
        FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()) + Start, Count);
        return FString(Conv.Length(), Conv.Get());
    }

    // Case-insensitive header lookup on a parsed response (keys stored lowercase).
    inline FString GetHeader(const FParsedHttpResponse& R, const FString& Key)
    {
        const FString* Found = R.Headers.Find(Key.ToLower());
        return Found ? *Found : FString();
    }

    // Parses ONE HTTP response from Bytes starting at Offset. Returns false
    // while head or (Content-Length-delimited) body is still incomplete. On
    // success, OutNextOffset points past the consumed response so keep-alive
    // tests can parse the next response from the same buffer. 1xx interim
    // responses are treated as headerless-body (consume head only). Responses
    // with no Content-Length take all currently-buffered remaining bytes.
    inline bool ParseOneHttpResponse(const TArray<uint8>& Bytes, int32 Offset,
        FParsedHttpResponse& Out, int32& OutNextOffset)
    {
        Out = FParsedHttpResponse();
        const int32 HeaderEnd = FindHeaderEnd(Bytes, Offset);
        if (HeaderEnd == INDEX_NONE)
        {
            return false;
        }
        const FString Head = Utf8Range(Bytes, Offset, HeaderEnd - Offset);
        TArray<FString> Lines;
        Head.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);
        if (Lines.Num() == 0)
        {
            return false;
        }
        Out.StatusLine = Lines[0].TrimStartAndEnd();
        {
            TArray<FString> Parts;
            Out.StatusLine.ParseIntoArray(Parts, TEXT(" "), /*bCullEmpty=*/true);
            Out.Code = (Parts.Num() >= 2) ? FCString::Atoi(*Parts[1]) : 0;
        }
        for (int32 i = 1; i < Lines.Num(); ++i)
        {
            int32 ColonIdx = INDEX_NONE;
            if (!Lines[i].FindChar(TEXT(':'), ColonIdx))
            {
                continue;
            }
            const FString Key = Lines[i].Left(ColonIdx).TrimStartAndEnd().ToLower();
            const FString Value = Lines[i].Mid(ColonIdx + 1).TrimStartAndEnd();
            if (FString* Existing = Out.Headers.Find(Key))
            {
                *Existing += TEXT(", ") + Value;  // HTTP duplicate-header list semantics
            }
            else
            {
                Out.Headers.Add(Key, Value);
            }
        }

        Out.BodyStart = HeaderEnd + 4;
        if (Out.Code >= 100 && Out.Code < 200)
        {
            // Interim response (e.g. 100 Continue): head only, never a body.
            Out.BodyByteLen = 0;
            OutNextOffset = Out.BodyStart;
            Out.bComplete = true;
            return true;
        }

        const FString ContentLengthStr = GetHeader(Out, TEXT("content-length"));
        if (!ContentLengthStr.IsEmpty())
        {
            const int32 ContentLength = FCString::Atoi(*ContentLengthStr);
            if (Bytes.Num() - Out.BodyStart < ContentLength)
            {
                return false;  // body not fully buffered yet
            }
            Out.BodyByteLen = ContentLength;
        }
        else
        {
            Out.BodyByteLen = Bytes.Num() - Out.BodyStart;  // close/stream-delimited
        }
        Out.Body = Utf8Range(Bytes, Out.BodyStart, Out.BodyByteLen);
        OutNextOffset = Out.BodyStart + Out.BodyByteLen;
        Out.bComplete = true;
        return true;
    }

    // Recv-pumps until one full HTTP response parses from Offset, or deadline.
    inline bool RecvHttpResponse(FSocketTestClient& Client, FSocketHttpServer* Server,
        FParsedHttpResponse& Out, int32& OutNextOffset,
        double TimeoutSeconds = kDefaultTimeoutSeconds, int32 Offset = 0)
    {
        const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
        while (FPlatformTime::Seconds() < Deadline)
        {
            if (ParseOneHttpResponse(Client.Received, Offset, Out, OutNextOffset))
            {
                return true;
            }
            const ERecvStep Step = Client.RecvStep();
            if (Step == ERecvStep::Closed)
            {
                // Final attempt: a close-delimited body is complete now.
                return ParseOneHttpResponse(Client.Received, Offset, Out, OutNextOffset);
            }
            if (Step != ERecvStep::Data)
            {
                PumpOnce(Server);
            }
        }
        return ParseOneHttpResponse(Client.Received, Offset, Out, OutNextOffset);
    }

    // ------------------------------------------------------------------
    // HTTP request + JSON-RPC body builders
    // ------------------------------------------------------------------

    // Assembles a raw HTTP/1.1 request. Content-Length is computed from the
    // UTF-8 byte length of Body (matching what SendString puts on the wire).
    inline FString BuildHttpRequest(const FString& Verb, const FString& Path,
        const TArray<TPair<FString, FString>>& Headers, const FString& Body,
        bool bIncludeContentLength = true)
    {
        FTCHARToUTF8 BodyUtf8(*Body);
        FString Req = FString::Printf(TEXT("%s %s HTTP/1.1\r\n"), *Verb, *Path);
        Req += TEXT("Host: 127.0.0.1\r\n");
        for (const TPair<FString, FString>& Header : Headers)
        {
            Req += FString::Printf(TEXT("%s: %s\r\n"), *Header.Key, *Header.Value);
        }
        if (bIncludeContentLength)
        {
            Req += FString::Printf(TEXT("Content-Length: %d\r\n"), BodyUtf8.Length());
        }
        Req += TEXT("\r\n");
        Req += Body;
        return Req;
    }

    inline FString SerializeJsonCondensed(const TSharedRef<FJsonObject>& Obj)
    {
        FString Out;
        const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
        FJsonSerializer::Serialize(Obj, Writer);
        Writer->Close();
        return Out;
    }

    inline TSharedPtr<FJsonObject> ParseJsonObject(const FString& Text)
    {
        TSharedPtr<FJsonObject> Obj;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
        FJsonSerializer::Deserialize(Reader, Obj);
        return Obj;
    }

    // {"jsonrpc":"2.0","id":Id,"method":"ping"}
    inline FString BuildPingBody(int32 Id)
    {
        TSharedRef<FJsonObject> Env = MakeShared<FJsonObject>();
        Env->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
        Env->SetNumberField(TEXT("id"), Id);
        Env->SetStringField(TEXT("method"), TEXT("ping"));
        return SerializeJsonCondensed(Env);
    }

    // Shape of the tools/call args.wait field. The stream gate is opt-OUT:
    // given progressToken + Accept, Absent and True both stream; only an
    // explicit False forces the buffered job-ticket path.
    enum class EWaitArg : uint8
    {
        Absent,   // no args.wait field
        True,     // args.wait = true
        False     // args.wait = false (explicit opt-out of streaming)
    };

    // tools/call envelope for the 'call' tool. bProgressToken adds
    // params._meta.progressToken ("tok-<Id>"); Wait controls the args.wait
    // field shape (absent / true / false); ExtraArgs fields are merged into
    // args.
    inline FString BuildToolsCallBody(int32 Id, const FString& Method,
        bool bProgressToken, EWaitArg Wait,
        const TSharedPtr<FJsonObject>& ExtraArgs = nullptr)
    {
        TSharedRef<FJsonObject> ArgsObj = MakeShared<FJsonObject>();
        if (Wait != EWaitArg::Absent)
        {
            ArgsObj->SetBoolField(TEXT("wait"), Wait == EWaitArg::True);
        }
        if (ExtraArgs.IsValid())
        {
            for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : ExtraArgs->Values)
            {
                ArgsObj->SetField(Pair.Key, Pair.Value);
            }
        }

        TSharedRef<FJsonObject> Arguments = MakeShared<FJsonObject>();
        Arguments->SetStringField(TEXT("method"), Method);
        Arguments->SetObjectField(TEXT("args"), ArgsObj);

        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), TEXT("call"));
        Params->SetObjectField(TEXT("arguments"), Arguments);
        if (bProgressToken)
        {
            TSharedRef<FJsonObject> Meta = MakeShared<FJsonObject>();
            Meta->SetStringField(TEXT("progressToken"), FString::Printf(TEXT("tok-%d"), Id));
            Params->SetObjectField(TEXT("_meta"), Meta);
        }

        TSharedRef<FJsonObject> Env = MakeShared<FJsonObject>();
        Env->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
        Env->SetNumberField(TEXT("id"), Id);
        Env->SetStringField(TEXT("method"), TEXT("tools/call"));
        Env->SetObjectField(TEXT("params"), Params);
        return SerializeJsonCondensed(Env);
    }

    // Standard notifications/progress notification for WriteStreamFrame.
    inline TSharedRef<FJsonObject> BuildProgressNotification(const FString& ProgressToken,
        double Progress, const TSharedPtr<FJsonObject>& ExtraParams = nullptr)
    {
        TSharedRef<FJsonObject> ParamsObj = MakeShared<FJsonObject>();
        ParamsObj->SetStringField(TEXT("progressToken"), ProgressToken);
        ParamsObj->SetNumberField(TEXT("progress"), Progress);
        if (ExtraParams.IsValid())
        {
            for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : ExtraParams->Values)
            {
                ParamsObj->SetField(Pair.Key, Pair.Value);
            }
        }
        TSharedRef<FJsonObject> Notification = MakeShared<FJsonObject>();
        Notification->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
        Notification->SetStringField(TEXT("method"), TEXT("notifications/progress"));
        Notification->SetObjectField(TEXT("params"), ParamsObj);
        return Notification;
    }

    // "Authorization: Bearer <real token>" using the same provisioning path the
    // server reads (Saved/PinWright/gateway-token, create-if-missing).
    inline TPair<FString, FString> MakeAuthHeader()
    {
        return TPair<FString, FString>(TEXT("Authorization"),
            FString(TEXT("Bearer ")) + GatewayAuthToken::GetOrCreateToken());
    }

    // ------------------------------------------------------------------
    // SSE stream parsing
    // ------------------------------------------------------------------

    struct FSseEvent
    {
        bool bComment = false;   // ":"-prefixed heartbeat line
        FString CommentText;     // text after ':' (untrimmed remainder)
        FString EventName;       // last `event:` field before dispatch
        FString Data;            // joined `data:` payload
    };

    // Parses SSE frames from the recv buffer starting at ByteStart (typically
    // FParsedHttpResponse::BodyStart of the streaming response). Tolerates
    // both CRLF and LF line endings. Comments are returned as bComment events
    // (in order) so heartbeat placement is assertable; data-less frames are
    // dropped per the SSE spec.
    inline TArray<FSseEvent> ParseSseEvents(const TArray<uint8>& Bytes, int32 ByteStart)
    {
        TArray<FSseEvent> Events;
        const FString Text = Utf8Range(Bytes, ByteStart, Bytes.Num() - ByteStart);

        TArray<FString> Lines;
        Text.ParseIntoArray(Lines, TEXT("\n"), /*bCullEmpty=*/false);

        FSseEvent Pending;
        bool bHasData = false;
        for (FString Line : Lines)
        {
            Line.RemoveFromEnd(TEXT("\r"));
            if (Line.IsEmpty())
            {
                if (bHasData)
                {
                    Events.Add(Pending);
                }
                Pending = FSseEvent();
                bHasData = false;
                continue;
            }
            if (Line.StartsWith(TEXT(":")))
            {
                FSseEvent Comment;
                Comment.bComment = true;
                Comment.CommentText = Line.Mid(1);
                Events.Add(Comment);
                continue;
            }
            if (Line.StartsWith(TEXT("event:")))
            {
                Pending.EventName = Line.Mid(6).TrimStartAndEnd();
                continue;
            }
            if (Line.StartsWith(TEXT("data:")))
            {
                FString Chunk = Line.Mid(5);
                if (Chunk.StartsWith(TEXT(" ")))
                {
                    Chunk.MidInline(1);
                }
                if (bHasData)
                {
                    Pending.Data += TEXT("\n");
                }
                Pending.Data += Chunk;
                bHasData = true;
                continue;
            }
            // Other field lines carry no payload for these tests.
        }
        return Events;
    }

    // Data events only (comments filtered), preserving order.
    inline TArray<FSseEvent> DataEventsOnly(const TArray<FSseEvent>& Events)
    {
        TArray<FSseEvent> Out;
        for (const FSseEvent& E : Events)
        {
            if (!E.bComment)
            {
                Out.Add(E);
            }
        }
        return Out;
    }

    // ------------------------------------------------------------------
    // Fake dispatcher
    // ------------------------------------------------------------------

    enum class EDispatchMode : uint8
    {
        Hold,             // record only; the test resolves/streams manually
        ResolveAll,       // record, then immediately resolve success {ok:true}
        ResolveBuffered   // resolve non-streaming requests, hold streaming ones
    };

    // Thread-safe capture of dispatched requests. The fake dispatcher may run
    // on the server's I/O thread; tests read entries from the game thread.
    struct FDispatchCapture
    {
        struct FEntry
        {
            FString RequestId;
            FString Method;
            TSharedPtr<FJsonObject> Params;
            bool bStreamingAtDispatch = false;  // IsStreamingRequest() sampled in the callback
        };

        mutable FCriticalSection Mutex;
        TArray<FEntry> Entries;

        int32 Num() const
        {
            FScopeLock Lock(&Mutex);
            return Entries.Num();
        }

        FEntry Get(int32 Index) const
        {
            FScopeLock Lock(&Mutex);
            return Entries.IsValidIndex(Index) ? Entries[Index] : FEntry();
        }

        TArray<FEntry> Snapshot() const
        {
            FScopeLock Lock(&Mutex);
            return Entries;
        }
    };

    // Result of establishing one SSE stream against a Hold-mode dispatcher.
    struct FEstablishedStream
    {
        bool bOk = false;
        FString RequestId;        // server-side id for WriteStreamFrame/ResolveCompletion
        FString ProgressToken;    // token sent in params._meta.progressToken
        FParsedHttpResponse Head; // the SSE response head (Code, headers, BodyStart)
    };

    // Sends a full streaming-eligible request (progressToken + Accept:
    // text/event-stream + an explicit args.wait=true — the gate is opt-out,
    // so wait-absent would stream too) on Client, waits for the dispatcher
    // capture to grow past PriorCaptureCount and for the SSE response head to
    // arrive, and asserts the upgrade. ExtraArgs is merged into args (e.g. a
    // per-stream tag). Test assertions fire through Test so a failure is
    // attributed, and bOk gates the caller's follow-on steps.
    inline FEstablishedStream EstablishSseStream(FAutomationTestBase& Test,
        FSocketTestClient& Client, FSocketHttpServer* Server,
        const TSharedRef<FDispatchCapture>& Capture, int32 EnvelopeId,
        const TSharedPtr<FJsonObject>& ExtraArgs = nullptr,
        int32 PriorCaptureCount = -1)
    {
        FEstablishedStream Out;
        Out.ProgressToken = FString::Printf(TEXT("tok-%d"), EnvelopeId);
        if (PriorCaptureCount < 0)
        {
            PriorCaptureCount = Capture->Num();
        }

        const FString Body = BuildToolsCallBody(EnvelopeId, TEXT("test.stream"),
            /*bProgressToken=*/true, EWaitArg::True, ExtraArgs);
        if (!Test.TestTrue(TEXT("streaming request sent"), Client.SendString(
                BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
                    {{TEXT("Content-Type"), TEXT("application/json")},
                     {TEXT("Accept"), TEXT("text/event-stream")}}, Body),
                Server)))
        {
            return Out;
        }

        if (!Test.TestTrue(TEXT("streaming request reached the dispatcher"),
                WaitForCondition([&Capture, PriorCaptureCount]()
                    { return Capture->Num() > PriorCaptureCount; }, Server)))
        {
            return Out;
        }
        Out.RequestId = Capture->Get(Capture->Num() - 1).RequestId;

        // The SSE head is written on upgrade, before any frame.
        if (!Test.TestTrue(TEXT("SSE response head received"),
                Client.RecvUntilContains(TEXT("\r\n\r\n"), Server)))
        {
            return Out;
        }
        int32 Next = 0;
        if (!Test.TestTrue(TEXT("SSE response head parses"),
                ParseOneHttpResponse(Client.Received, 0, Out.Head, Next)))
        {
            return Out;
        }
        Test.TestEqual(TEXT("SSE response status is 200"), Out.Head.Code, 200);
        Test.TestTrue(TEXT("SSE response Content-Type is text/event-stream"),
            GetHeader(Out.Head, TEXT("content-type")).Contains(TEXT("text/event-stream")));
        Out.bOk = true;
        return Out;
    }

    // Binds OnRequestReceived to a capture-and-optionally-resolve stub. The
    // lambda holds Server by raw reference — the delegate lives on the server
    // itself, so the reference can never outlive its target.
    inline void BindCaptureDispatcher(FSocketHttpServer& Server,
        const TSharedRef<FDispatchCapture>& Capture, EDispatchMode Mode)
    {
        FSocketHttpServer* ServerPtr = &Server;
        Server.OnRequestReceived.BindLambda(
            [ServerPtr, Capture, Mode](const FString& RequestId, const FString& Method,
                const TSharedPtr<FJsonObject>& Params)
            {
                const bool bStreaming = ServerPtr->IsStreamingRequest(RequestId);
                {
                    FScopeLock Lock(&Capture->Mutex);
                    FDispatchCapture::FEntry Entry;
                    Entry.RequestId = RequestId;
                    Entry.Method = Method;
                    Entry.Params = Params;
                    Entry.bStreamingAtDispatch = bStreaming;
                    Capture->Entries.Add(MoveTemp(Entry));
                }
                const bool bResolveNow = (Mode == EDispatchMode::ResolveAll) ||
                    (Mode == EDispatchMode::ResolveBuffered && !bStreaming);
                if (bResolveNow)
                {
                    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
                    Result->SetBoolField(TEXT("ok"), true);
                    ServerPtr->ResolveCompletion(RequestId, /*bSuccess=*/true,
                        TEXT("OK"), Result, FString());
                }
            });
    }
}
