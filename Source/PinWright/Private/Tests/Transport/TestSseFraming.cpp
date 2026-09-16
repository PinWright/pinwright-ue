// Copyright (c) 2026 Alexander Penkin. MIT License.

// SSE wire-format tests for FSocketHttpServer: byte-exact frame framing
// ("event: message\r\ndata: <condensed json>\r\n\r\n"), one-frame integrity
// for payloads with embedded newlines, UTF-8 round-tripping, and the
// ": ping\n\n" heartbeat comment emitted by Tick past the heartbeat interval.
#include "Misc/AutomationTest.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/Transport/SocketTestClient.h"

namespace PinWrightSseFramingTest
{
    using namespace PinWrightSocketTest;

    // Raw SSE byte region of the stream (everything after the response head),
    // decoded as UTF-8 for byte-pattern assertions.
    inline FString StreamText(const FSocketTestClient& Client, const FParsedHttpResponse& Head)
    {
        return Utf8Range(Client.Received, Head.BodyStart, Client.Received.Num() - Head.BodyStart);
    }
}

// ============================================================================
// Frame bytes: exact "event: message\r\ndata: " prefix and "\r\n\r\n"
// terminator; the JSON is condensed onto ONE data line even when a payload
// string embeds newlines; UTF-8 multibyte content round-trips intact.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSseFramingFrameBytesTest,
    "PinWright.transport.sse.Framing.FrameBytesCondensedUtf8",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSseFramingFrameBytesTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightSocketTest;
    using namespace PinWrightSseFramingTest;

    FScopedAuthRequirement Auth(false);
    uint32 Port = 0;
    TSharedPtr<FSocketHttpServer> Server = StartServerOnFreePort(Port);
    if (!Server.IsValid())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("test-port-unbindable"),
            TEXT("Could not bind a test port for FSocketHttpServer; skipping."));
        return true;
    }
    ON_SCOPE_EXIT { Server->Stop(); };
    TSharedRef<FDispatchCapture> Capture = MakeShared<FDispatchCapture>();
    BindCaptureDispatcher(*Server, Capture, EDispatchMode::Hold);

    FSocketTestClient Client;
    if (!TestTrue(TEXT("client connected"), Client.Connect(Port)))
    {
        return true;
    }
    const FEstablishedStream Stream = EstablishSseStream(*this, Client, Server.Get(), Capture, 1);
    if (!Stream.bOk)
    {
        return true;
    }

    // Payload with an embedded newline AND multibyte UTF-8 (Cyrillic + emoji).
    const FString TrickyMessage = TEXT("line one\nline two Дрон \U0001F681");
    TSharedPtr<FJsonObject> Extra = MakeShared<FJsonObject>();
    Extra->SetStringField(TEXT("message"), TrickyMessage);
    TestTrue(TEXT("WriteStreamFrame accepted"), Server->WriteStreamFrame(Stream.RequestId,
        BuildProgressNotification(Stream.ProgressToken, 1.0, Extra)));

    TestTrue(TEXT("frame arrived"), Client.RecvUntilContains(
        TEXT("notifications/progress"), Server.Get()));

    const FString Raw = StreamText(Client, Stream.Head);

    // Byte-exact frame shape.
    const FString FramePrefix = TEXT("event: message\r\ndata: ");
    const int32 PrefixIdx = Raw.Find(FramePrefix, ESearchCase::CaseSensitive);
    TestTrue(TEXT("frame starts with 'event: message\\r\\ndata: '"), PrefixIdx != INDEX_NONE);
    if (PrefixIdx == INDEX_NONE)
    {
        return true;
    }
    const int32 DataStart = PrefixIdx + FramePrefix.Len();
    const int32 TermIdx = Raw.Find(TEXT("\r\n\r\n"), ESearchCase::CaseSensitive,
        ESearchDir::FromStart, DataStart);
    TestTrue(TEXT("frame ends with '\\r\\n\\r\\n'"), TermIdx != INDEX_NONE);
    if (TermIdx == INDEX_NONE)
    {
        return true;
    }

    // The data payload must be ONE line: condensed JSON, no raw newline inside
    // the frame body (the embedded newline must ride as the \n JSON escape).
    const FString DataLine = Raw.Mid(DataStart, TermIdx - DataStart);
    TestFalse(TEXT("data payload contains no raw LF (condensed, single line)"),
        DataLine.Contains(TEXT("\n"), ESearchCase::CaseSensitive));
    TestFalse(TEXT("data payload contains no raw CR"),
        DataLine.Contains(TEXT("\r"), ESearchCase::CaseSensitive));

    // UTF-8 + newline round-trip through serialize -> wire -> parse.
    TSharedPtr<FJsonObject> Parsed = ParseJsonObject(DataLine);
    if (TestTrue(TEXT("data payload parses as JSON"), Parsed.IsValid()))
    {
        TestEqual(TEXT("frame method is notifications/progress"),
            Parsed->GetStringField(TEXT("method")), FString(TEXT("notifications/progress")));
        const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
        if (TestTrue(TEXT("frame params present"),
                Parsed->TryGetObjectField(TEXT("params"), ParamsPtr) && ParamsPtr))
        {
            TestEqual(TEXT("UTF-8 + embedded-newline message round-trips exactly"),
                (*ParamsPtr)->GetStringField(TEXT("message")), TrickyMessage);
            TestEqual(TEXT("progressToken round-trips"),
                (*ParamsPtr)->GetStringField(TEXT("progressToken")), Stream.ProgressToken);
        }
    }

    // Terminate the stream so the server doesn't carry a dangling completion.
    Server->ResolveCompletion(Stream.RequestId, true, TEXT("OK"),
        MakeShared<FJsonObject>(), FString());
    Client.RecvUntilClosed(Server.Get());
    return true;
}

// ============================================================================
// Heartbeat: with a stream open and no frames flowing, advancing the server's
// Tick clock past the heartbeat interval must emit the byte-exact SSE comment
// ": ping\n\n" on the wire (keeps proxies/timeouts from reaping idle streams).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSseFramingHeartbeatTest,
    "PinWright.transport.sse.Framing.HeartbeatComment",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSseFramingHeartbeatTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightSocketTest;
    using namespace PinWrightSseFramingTest;

    FScopedAuthRequirement Auth(false);
    uint32 Port = 0;
    TSharedPtr<FSocketHttpServer> Server = StartServerOnFreePort(Port);
    if (!Server.IsValid())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("test-port-unbindable"),
            TEXT("Could not bind a test port for FSocketHttpServer; skipping."));
        return true;
    }
    ON_SCOPE_EXIT { Server->Stop(); };
    TSharedRef<FDispatchCapture> Capture = MakeShared<FDispatchCapture>();
    BindCaptureDispatcher(*Server, Capture, EDispatchMode::Hold);

    FSocketTestClient Client;
    if (!TestTrue(TEXT("client connected"), Client.Connect(Port)))
    {
        return true;
    }
    const FEstablishedStream Stream = EstablishSseStream(*this, Client, Server.Get(), Capture, 2);
    if (!Stream.bOk)
    {
        return true;
    }

    // Advance the server's tick clock well past any sane heartbeat interval
    // (<=15s per the proxy contract) in 1s steps, receiving as we go. The
    // recv deadline is wall-clock-bounded so a heartbeat-less server fails
    // the assert instead of hanging.
    const double Deadline = FPlatformTime::Seconds() + 10.0;
    bool bSawHeartbeat = false;
    float TickedSeconds = 0.0f;
    while (FPlatformTime::Seconds() < Deadline && TickedSeconds < 60.0f)
    {
        Server->Tick(1.0f);
        TickedSeconds += 1.0f;
        for (int32 i = 0; i < 5; ++i)
        {
            if (Client.RecvStep() != ERecvStep::Data)
            {
                FTSTicker::GetCoreTicker().Tick(0.01f);
                FPlatformProcess::Sleep(0.001f);
            }
        }
        if (StreamText(Client, Stream.Head).Contains(TEXT(": ping\n\n"), ESearchCase::CaseSensitive))
        {
            bSawHeartbeat = true;
            break;
        }
    }
    TestTrue(TEXT("byte-exact ': ping\\n\\n' heartbeat appeared after Tick passed the interval"),
        bSawHeartbeat);

    // The heartbeat must parse as an SSE comment, not as a data event.
    const TArray<FSseEvent> Events = ParseSseEvents(Client.Received, Stream.Head.BodyStart);
    bool bCommentSeen = false;
    for (const FSseEvent& E : Events)
    {
        if (E.bComment)
        {
            bCommentSeen = true;
        }
        else
        {
            AddError(FString::Printf(
                TEXT("unexpected data event during idle heartbeat phase: %s"), *E.Data));
        }
    }
    TestTrue(TEXT("heartbeat parses as an SSE comment"), bCommentSeen);

    Server->ResolveCompletion(Stream.RequestId, true, TEXT("OK"),
        MakeShared<FJsonObject>(), FString());
    Client.RecvUntilClosed(Server.Get());
    return true;
}
