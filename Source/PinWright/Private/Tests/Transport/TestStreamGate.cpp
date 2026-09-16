// Copyright (c) 2026 Alexander Penkin. MIT License.

// Opt-out stream-gate selection tests: a request streams (SSE) iff
// params._meta.progressToken is present, "Accept: text/event-stream" is
// present, and args.wait is NOT explicitly false (absent and true both
// stream). Every other combination — missing token, missing Accept, or an
// explicit args.wait=false opt-out — must take the buffered JSON path.
#include "Misc/AutomationTest.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/Transport/SocketTestClient.h"

namespace PinWrightStreamGateTest
{
    using namespace PinWrightSocketTest;

    // One truth-table row: request-shape switches + the expected route.
    struct FGateCombo
    {
        bool bToken;
        bool bAccept;
        EWaitArg Wait;
    };

    inline const TCHAR* WaitArgLabel(EWaitArg Wait)
    {
        switch (Wait)
        {
            case EWaitArg::True:  return TEXT("true");
            case EWaitArg::False: return TEXT("false");
            default:              return TEXT("absent");
        }
    }

    // Drives one combo on a fresh connection against a ResolveAll dispatcher
    // and asserts the response route (SSE vs buffered JSON) plus the
    // IsStreamingRequest() flag sampled inside the dispatcher callback.
    inline void RunCombo(FAutomationTestBase& Test, uint32 Port, FSocketHttpServer* Server,
        const TSharedRef<FDispatchCapture>& Capture, int32 EnvelopeId, const FGateCombo& Combo)
    {
        // Opt-out gate: token + Accept stream unless wait is explicitly false.
        const bool bExpectStream = Combo.bToken && Combo.bAccept &&
            Combo.Wait != EWaitArg::False;
        const FString Label = FString::Printf(TEXT("combo token=%d accept=%d wait=%s"),
            Combo.bToken ? 1 : 0, Combo.bAccept ? 1 : 0, WaitArgLabel(Combo.Wait));

        FSocketTestClient Client;
        if (!Test.TestTrue(Label + TEXT(": connected"), Client.Connect(Port)))
        {
            return;
        }

        TArray<TPair<FString, FString>> Headers;
        Headers.Add(TPair<FString, FString>(TEXT("Content-Type"), TEXT("application/json")));
        Headers.Add(TPair<FString, FString>(TEXT("Accept"),
            Combo.bAccept ? TEXT("text/event-stream") : TEXT("application/json")));

        const int32 Prior = Capture->Num();
        if (!Test.TestTrue(Label + TEXT(": request sent"), Client.SendString(
                BuildHttpRequest(TEXT("POST"), TEXT("/mcp"), Headers,
                    BuildToolsCallBody(EnvelopeId, TEXT("test.gate"), Combo.bToken, Combo.Wait)),
                Server)))
        {
            return;
        }

        if (!Test.TestTrue(Label + TEXT(": reached the dispatcher"),
                WaitForCondition([&Capture, Prior]() { return Capture->Num() > Prior; }, Server)))
        {
            return;
        }
        const FDispatchCapture::FEntry Entry = Capture->Get(Capture->Num() - 1);
        Test.TestEqual(Label + TEXT(": IsStreamingRequest at dispatch matches the gate"),
            Entry.bStreamingAtDispatch, bExpectStream);

        FParsedHttpResponse R;
        int32 Next = 0;
        if (!Test.TestTrue(Label + TEXT(": response received"),
                RecvHttpResponse(Client, Server, R, Next)))
        {
            return;
        }
        Test.TestEqual(Label + TEXT(": status is 200"), R.Code, 200);

        const FString ContentType = GetHeader(R, TEXT("content-type"));
        if (bExpectStream)
        {
            Test.TestTrue(Label + TEXT(": Content-Type is text/event-stream"),
                ContentType.Contains(TEXT("text/event-stream")));
            // The dispatcher resolved immediately, so the stream carries the
            // terminal frame and then closes.
            Test.TestTrue(Label + TEXT(": terminal frame arrived on the stream"),
                Client.RecvUntilContains(TEXT("\"result\""), Server));
            Test.TestTrue(Label + TEXT(": stream closes after the terminal frame"),
                Client.RecvUntilClosed(Server));
        }
        else
        {
            Test.TestTrue(Label + TEXT(": Content-Type is application/json (buffered)"),
                ContentType.Contains(TEXT("application/json")));
            Test.TestFalse(Label + TEXT(": buffered response is not an SSE stream"),
                ContentType.Contains(TEXT("text/event-stream")));
            Test.TestTrue(Label + TEXT(": buffered body carries the JSON-RPC result"),
                R.Body.Contains(TEXT("\"result\""), ESearchCase::CaseSensitive));
        }
    }
}

// ============================================================================
// All 12 combinations of (progressToken, Accept: text/event-stream) x
// args.wait in {absent, true, false}: only token+Accept with wait absent or
// wait=true streams (2 rows); the other 10 rows are buffered JSON.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStreamGateTruthTableTest,
    "PinWright.transport.stream_gate.OptOutGateTruthTable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStreamGateTruthTableTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightSocketTest;
    using namespace PinWrightStreamGateTest;

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
    BindCaptureDispatcher(*Server, Capture, EDispatchMode::ResolveAll);

    const EWaitArg WaitShapes[] = { EWaitArg::Absent, EWaitArg::True, EWaitArg::False };
    int32 EnvelopeId = 100;
    for (int32 Mask = 0; Mask < 4; ++Mask)
    {
        for (EWaitArg Wait : WaitShapes)
        {
            FGateCombo Combo;
            Combo.bToken  = (Mask & 2) != 0;
            Combo.bAccept = (Mask & 1) != 0;
            Combo.Wait    = Wait;
            RunCombo(*this, Port, Server.Get(), Capture, ++EnvelopeId, Combo);
        }
    }
    return true;
}

// ============================================================================
// The explicit Codex case: progressToken + Accept: text/event-stream and NO
// args.wait — clients like Codex always send a token and the Accept header
// without any wait flag, and under the opt-out gate that MUST stream. (This
// inverts the old opt-in behavior that kept Codex-shaped requests buffered;
// the opt-out is now an explicit args.wait=false.)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStreamGateCodexNoWaitTest,
    "PinWright.transport.stream_gate.CodexTokenAcceptNoWaitStreams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStreamGateCodexNoWaitTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightSocketTest;
    using namespace PinWrightStreamGateTest;

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
    BindCaptureDispatcher(*Server, Capture, EDispatchMode::ResolveAll);

    FSocketTestClient Client;
    if (!TestTrue(TEXT("client connected"), Client.Connect(Port)))
    {
        return true;
    }
    TestTrue(TEXT("Codex-shaped request sent"), Client.SendString(
        BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
            {{TEXT("Content-Type"), TEXT("application/json")},
             {TEXT("Accept"), TEXT("application/json, text/event-stream")}},
            BuildToolsCallBody(500, TEXT("test.codex"),
                /*bProgressToken=*/true, EWaitArg::Absent)),
        Server.Get()));

    TestTrue(TEXT("request reached the dispatcher"),
        WaitForCondition([&Capture]() { return Capture->Num() > 0; }, Server.Get()));
    if (Capture->Num() > 0)
    {
        TestTrue(TEXT("token+Accept without wait IS a streaming request (opt-out gate)"),
            Capture->Get(0).bStreamingAtDispatch);
    }

    FParsedHttpResponse R;
    int32 Next = 0;
    TestTrue(TEXT("response received"), RecvHttpResponse(Client, Server.Get(), R, Next));
    TestEqual(TEXT("status is 200"), R.Code, 200);
    TestTrue(TEXT("Content-Type is text/event-stream"),
        GetHeader(R, TEXT("content-type")).Contains(TEXT("text/event-stream")));
    // The ResolveAll dispatcher resolved immediately: terminal frame + close.
    TestTrue(TEXT("terminal frame arrived on the stream"),
        Client.RecvUntilContains(TEXT("\"result\""), Server.Get()));
    TestTrue(TEXT("stream closes after the terminal frame"),
        Client.RecvUntilClosed(Server.Get()));
    return true;
}

// ============================================================================
// The explicit opt-out: progressToken + Accept: text/event-stream but
// args.wait=false — the one wait shape that suppresses the upgrade. The
// request must stay a plain buffered JSON response (job-ticket path).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStreamGateWaitFalseOptOutTest,
    "PinWright.transport.stream_gate.WaitFalseOptOutBuffered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStreamGateWaitFalseOptOutTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightSocketTest;
    using namespace PinWrightStreamGateTest;

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
    BindCaptureDispatcher(*Server, Capture, EDispatchMode::ResolveAll);

    FSocketTestClient Client;
    if (!TestTrue(TEXT("client connected"), Client.Connect(Port)))
    {
        return true;
    }
    TestTrue(TEXT("opt-out request sent"), Client.SendString(
        BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
            {{TEXT("Content-Type"), TEXT("application/json")},
             {TEXT("Accept"), TEXT("text/event-stream")}},
            BuildToolsCallBody(600, TEXT("test.optout"),
                /*bProgressToken=*/true, EWaitArg::False)),
        Server.Get()));

    TestTrue(TEXT("request reached the dispatcher"),
        WaitForCondition([&Capture]() { return Capture->Num() > 0; }, Server.Get()));
    if (Capture->Num() > 0)
    {
        TestFalse(TEXT("token+Accept with wait=false is NOT a streaming request"),
            Capture->Get(0).bStreamingAtDispatch);
    }

    FParsedHttpResponse R;
    int32 Next = 0;
    TestTrue(TEXT("response received"), RecvHttpResponse(Client, Server.Get(), R, Next));
    TestEqual(TEXT("status is 200"), R.Code, 200);
    TestTrue(TEXT("Content-Type is application/json (buffered)"),
        GetHeader(R, TEXT("content-type")).Contains(TEXT("application/json")));
    TestFalse(TEXT("response is not an SSE stream"),
        GetHeader(R, TEXT("content-type")).Contains(TEXT("text/event-stream")));
    TestTrue(TEXT("buffered body carries the JSON-RPC result"),
        R.Body.Contains(TEXT("\"result\""), ESearchCase::CaseSensitive));
    return true;
}

namespace PinWrightStreamGateTest
{
    using namespace PinWrightSocketTest;

    // Simulates the two handler contracts that the stream gate must preserve:
    // buffered execution returns a running ticket immediately, while streaming
    // execution publishes that ticket as progress and finishes with the full
    // terminal result.
    inline void BindRunningTicketContractDispatcher(
        FSocketHttpServer& Server,
        const TSharedRef<FDispatchCapture>& Capture,
        const FString& TicketId,
        const FString& TerminalMarker)
    {
        FSocketHttpServer* ServerPtr = &Server;
        Server.OnRequestReceived.BindLambda(
            [ServerPtr, Capture, TicketId, TerminalMarker](
                const FString& RequestId, const FString& Method,
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

                if (!bStreaming)
                {
                    TSharedPtr<FJsonObject> Running = MakeShared<FJsonObject>();
                    Running->SetStringField(TEXT("status"), TEXT("running"));
                    Running->SetStringField(TEXT("ticket_id"), TicketId);
                    Running->SetStringField(TEXT("method"), Method);
                    ServerPtr->ResolveCompletion(RequestId, /*bSuccess=*/true,
                        TEXT("OK"), Running, FString());
                    return;
                }

                TSharedPtr<FJsonValue> TokenValue;
                FString ProgressToken = TEXT("missing-progress-token");
                if (ServerPtr->GetProgressToken(RequestId, TokenValue)
                    && TokenValue.IsValid()
                    && TokenValue->Type == EJson::String)
                {
                    ProgressToken = TokenValue->AsString();
                }

                TSharedPtr<FJsonObject> ProgressExtra = MakeShared<FJsonObject>();
                ProgressExtra->SetStringField(TEXT("ticket_id"), TicketId);
                ProgressExtra->SetStringField(TEXT("status"), TEXT("running"));
                ServerPtr->WriteStreamFrame(RequestId,
                    BuildProgressNotification(ProgressToken, 1.0, ProgressExtra));

                TSharedPtr<FJsonObject> FullResult = MakeShared<FJsonObject>();
                FullResult->SetStringField(TEXT("status"), TEXT("completed"));
                FullResult->SetStringField(TEXT("method"), Method);
                FullResult->SetStringField(TEXT("value"), TerminalMarker);
                ServerPtr->ResolveCompletion(RequestId, /*bSuccess=*/true,
                    TEXT("OK"), FullResult, FString());
            });
    }
}

// ============================================================================
// A method without args is a documentation lookup, not an executable call.
// It must return a complete documentation tool result without reaching the
// executable dispatcher.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStreamGateNoArgsDocumentationTest,
    "PinWright.transport.stream_gate.NoArgsDocumentation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStreamGateNoArgsDocumentationTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightSocketTest;
    using namespace PinWrightStreamGateTest;

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
    BindCaptureDispatcher(*Server, Capture, EDispatchMode::ResolveAll);

    // arguments has a method but deliberately no args field: this is the
    // documented wiki-page form from the call tool contract.
    TSharedRef<FJsonObject> Arguments = MakeShared<FJsonObject>();
    Arguments->SetStringField(TEXT("method"), TEXT("actor"));
    TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("name"), TEXT("call"));
    Params->SetObjectField(TEXT("arguments"), Arguments);
    TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
    Envelope->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
    Envelope->SetNumberField(TEXT("id"), 700);
    Envelope->SetStringField(TEXT("method"), TEXT("tools/call"));
    Envelope->SetObjectField(TEXT("params"), Params);

    FSocketTestClient Client;
    if (!TestTrue(TEXT("client connected"), Client.Connect(Port)))
    {
        return true;
    }
    if (!TestTrue(TEXT("no-args documentation request sent"), Client.SendString(
            BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
                {{TEXT("Content-Type"), TEXT("application/json")},
                 {TEXT("Accept"), TEXT("application/json")}},
                SerializeJsonCondensed(Envelope)),
            Server.Get())))
    {
        return true;
    }

    FParsedHttpResponse R;
    int32 Next = 0;
    if (!TestTrue(TEXT("documentation response received"),
            RecvHttpResponse(Client, Server.Get(), R, Next)))
    {
        return true;
    }
    TestEqual(TEXT("documentation response status is 200"), R.Code, 200);
    TestTrue(TEXT("documentation response is application/json"),
        GetHeader(R, TEXT("content-type")).Contains(TEXT("application/json")));
    TestFalse(TEXT("documentation response is not an SSE stream"),
        GetHeader(R, TEXT("content-type")).Contains(TEXT("text/event-stream")));
    TestEqual(TEXT("documentation request did not reach executable dispatcher"),
        Capture->Num(), 0);

    TSharedPtr<FJsonObject> Response = ParseJsonObject(R.Body);
    if (!TestTrue(TEXT("documentation response parses as JSON"), Response.IsValid()))
    {
        return true;
    }
    const TSharedPtr<FJsonObject>* ResultPtr = nullptr;
    if (!TestTrue(TEXT("documentation response has a result"),
            Response->TryGetObjectField(TEXT("result"), ResultPtr)
                && ResultPtr && ResultPtr->IsValid()))
    {
        return true;
    }
    const TSharedPtr<FJsonObject> Result = *ResultPtr;
    bool bIsError = true;
    Result->TryGetBoolField(TEXT("isError"), bIsError);
    TestFalse(TEXT("documentation result is not an error"), bIsError);
    const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
    TestTrue(TEXT("documentation result carries content"),
        Result->TryGetArrayField(TEXT("content"), Content) && Content && Content->Num() > 0);
    TestFalse(TEXT("documentation result has no running ticket"),
        Result->HasField(TEXT("ticket_id")));
    FString Status;
    TestFalse(TEXT("documentation result is not a running job"),
        Result->TryGetStringField(TEXT("status"), Status)
            && Status.Equals(TEXT("running"), ESearchCase::CaseSensitive));
    return true;
}

// ============================================================================
// Executable args with wait:false stay buffered and return the running ticket.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStreamGateBufferedExecutableWaitFalseTest,
    "PinWright.transport.stream_gate.BufferedExecutableWaitFalseReturnsTicket",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStreamGateBufferedExecutableWaitFalseTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightSocketTest;
    using namespace PinWrightStreamGateTest;

    const FString TicketId = TEXT("stream-gate-ticket");
    const FString TerminalMarker = TEXT("stream-gate-terminal");

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
    BindRunningTicketContractDispatcher(*Server, Capture, TicketId, TerminalMarker);

    TSharedPtr<FJsonObject> ExtraArgs = MakeShared<FJsonObject>();
    ExtraArgs->SetStringField(TEXT("input"), TEXT("buffered-executable"));
    FSocketTestClient Client;
    if (!TestTrue(TEXT("client connected"), Client.Connect(Port)))
    {
        return true;
    }
    if (!TestTrue(TEXT("buffered wait=false request sent"), Client.SendString(
            BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
                {{TEXT("Content-Type"), TEXT("application/json")},
                 {TEXT("Accept"), TEXT("text/event-stream")}},
                BuildToolsCallBody(701, TEXT("test.executable"),
                    /*bProgressToken=*/true, EWaitArg::False, ExtraArgs)),
            Server.Get())))
    {
        return true;
    }

    if (!TestTrue(TEXT("buffered executable reached the dispatcher"),
            WaitForCondition([&Capture]() { return Capture->Num() > 0; }, Server.Get())))
    {
        return true;
    }
    const FDispatchCapture::FEntry Entry = Capture->Get(0);
    TestFalse(TEXT("wait=false executable request is not streaming"),
        Entry.bStreamingAtDispatch);
    TestEqual(TEXT("buffered executable method reached the dispatcher"),
        Entry.Method, FString(TEXT("test.executable")));
    if (TestTrue(TEXT("buffered executable args were captured"), Entry.Params.IsValid()))
    {
        TestEqual(TEXT("buffered executable args preserve the input"),
            Entry.Params->GetStringField(TEXT("input")),
            FString(TEXT("buffered-executable")));
    }

    FParsedHttpResponse R;
    int32 Next = 0;
    if (!TestTrue(TEXT("buffered running response received"),
            RecvHttpResponse(Client, Server.Get(), R, Next)))
    {
        return true;
    }
    TestEqual(TEXT("buffered running response status is 200"), R.Code, 200);
    TestTrue(TEXT("buffered running response is application/json"),
        GetHeader(R, TEXT("content-type")).Contains(TEXT("application/json")));
    TestFalse(TEXT("buffered running response is not SSE"),
        GetHeader(R, TEXT("content-type")).Contains(TEXT("text/event-stream")));

    TSharedPtr<FJsonObject> Response = ParseJsonObject(R.Body);
    if (!TestTrue(TEXT("buffered running response parses as JSON"), Response.IsValid()))
    {
        return true;
    }
    const TSharedPtr<FJsonObject>* ResultPtr = nullptr;
    if (!TestTrue(TEXT("buffered running response has a result"),
            Response->TryGetObjectField(TEXT("result"), ResultPtr)
                && ResultPtr && ResultPtr->IsValid()))
    {
        return true;
    }
    const TSharedPtr<FJsonObject> Result = *ResultPtr;
    const TSharedPtr<FJsonObject>* StructuredPtr = nullptr;
    if (!TestTrue(TEXT("buffered running result has structured content"),
            Result->TryGetObjectField(TEXT("structuredContent"), StructuredPtr)
                && StructuredPtr && StructuredPtr->IsValid()))
    {
        return true;
    }
    const TSharedPtr<FJsonObject> Structured = *StructuredPtr;
    TestEqual(TEXT("buffered executable returns status running"),
        Structured->GetStringField(TEXT("status")), FString(TEXT("running")));
    FString ReturnedTicketId;
    TestTrue(TEXT("buffered executable returns ticket_id"),
        Structured->TryGetStringField(TEXT("ticket_id"), ReturnedTicketId));
    TestEqual(TEXT("buffered executable ticket_id is the running ticket"),
        ReturnedTicketId, TicketId);
    return true;
}

// ============================================================================
// A progress-token SSE request with wait absent or true must finish with the
// complete terminal result. The running ticket belongs in progress, not in the
// terminal JSON-RPC result.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStreamGateSseWaitShapesReturnFullResultTest,
    "PinWright.transport.stream_gate.SseWaitAbsentAndTrueReturnFullResult",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStreamGateSseWaitShapesReturnFullResultTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightSocketTest;
    using namespace PinWrightStreamGateTest;

    const FString TicketId = TEXT("stream-gate-ticket");
    const FString TerminalMarker = TEXT("stream-gate-terminal");
    const EWaitArg WaitShapes[] = { EWaitArg::Absent, EWaitArg::True };

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
    BindRunningTicketContractDispatcher(*Server, Capture, TicketId, TerminalMarker);

    for (int32 Index = 0; Index < UE_ARRAY_COUNT(WaitShapes); ++Index)
    {
        const EWaitArg Wait = WaitShapes[Index];
        const FString Label = FString::Printf(TEXT("SSE wait=%s"), WaitArgLabel(Wait));
        TSharedPtr<FJsonObject> ExtraArgs = MakeShared<FJsonObject>();
        ExtraArgs->SetStringField(TEXT("input"), TEXT("streaming-executable"));

        FSocketTestClient Client;
        if (!TestTrue(Label + TEXT(": client connected"), Client.Connect(Port)))
        {
            return true;
        }
        const int32 Prior = Capture->Num();
        if (!TestTrue(Label + TEXT(": request sent"), Client.SendString(
                BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
                    {{TEXT("Content-Type"), TEXT("application/json")},
                     {TEXT("Accept"), TEXT("text/event-stream")}},
                    BuildToolsCallBody(702 + Index, TEXT("test.executable"),
                        /*bProgressToken=*/true, Wait, ExtraArgs)),
                Server.Get())))
        {
            return true;
        }
        if (!TestTrue(Label + TEXT(": request reached the dispatcher"),
                WaitForCondition([&Capture, Prior]()
                    { return Capture->Num() > Prior; }, Server.Get())))
        {
            return true;
        }

        const FDispatchCapture::FEntry Entry = Capture->Get(Capture->Num() - 1);
        TestTrue(Label + TEXT(": request is streaming"), Entry.bStreamingAtDispatch);

        FParsedHttpResponse Head;
        int32 Next = 0;
        if (!TestTrue(Label + TEXT(": SSE response head received"),
                RecvHttpResponse(Client, Server.Get(), Head, Next)))
        {
            return true;
        }
        TestEqual(Label + TEXT(": SSE response status is 200"), Head.Code, 200);
        TestTrue(Label + TEXT(": SSE response is text/event-stream"),
            GetHeader(Head, TEXT("content-type")).Contains(TEXT("text/event-stream")));
        if (!TestTrue(Label + TEXT(": SSE stream closes after terminal result"),
                Client.RecvUntilClosed(Server.Get())))
        {
            return true;
        }

        const TArray<FSseEvent> DataEvents =
            DataEventsOnly(ParseSseEvents(Client.Received, Head.BodyStart));
        if (!TestEqual(Label + TEXT(": exactly one progress and one terminal frame"),
                DataEvents.Num(), 2))
        {
            return true;
        }

        TSharedPtr<FJsonObject> Progress = ParseJsonObject(DataEvents[0].Data);
        if (TestTrue(Label + TEXT(": progress frame parses"), Progress.IsValid()))
        {
            TestEqual(Label + TEXT(": progress frame is notifications/progress"),
                Progress->GetStringField(TEXT("method")),
                FString(TEXT("notifications/progress")));
            const TSharedPtr<FJsonObject>* ProgressParams = nullptr;
            if (TestTrue(Label + TEXT(": progress params are present"),
                    Progress->TryGetObjectField(TEXT("params"), ProgressParams)
                        && ProgressParams && ProgressParams->IsValid()))
            {
                FString ProgressTicket;
                TestTrue(Label + TEXT(": progress frame carries ticket_id"),
                    (*ProgressParams)->TryGetStringField(TEXT("ticket_id"), ProgressTicket));
                TestEqual(Label + TEXT(": progress ticket_id is the running ticket"),
                    ProgressTicket, TicketId);
            }
            TestFalse(Label + TEXT(": progress frame is not terminal"),
                DataEvents[0].Data.Contains(TEXT("\"result\""), ESearchCase::CaseSensitive));
        }

        TSharedPtr<FJsonObject> Terminal = ParseJsonObject(DataEvents[1].Data);
        if (!TestTrue(Label + TEXT(": terminal frame parses"), Terminal.IsValid()))
        {
            continue;
        }
        TestFalse(Label + TEXT(": terminal frame is not a notification"),
            Terminal->HasField(TEXT("method")));
        const TSharedPtr<FJsonObject>* ResultPtr = nullptr;
        if (!TestTrue(Label + TEXT(": terminal frame has a result"),
                Terminal->TryGetObjectField(TEXT("result"), ResultPtr)
                    && ResultPtr && ResultPtr->IsValid()))
        {
            continue;
        }
        const TSharedPtr<FJsonObject> Result = *ResultPtr;
        bool bIsError = true;
        Result->TryGetBoolField(TEXT("isError"), bIsError);
        TestFalse(Label + TEXT(": terminal result is not an error"), bIsError);
        const TSharedPtr<FJsonObject>* StructuredPtr = nullptr;
        if (TestTrue(Label + TEXT(": terminal result has structured content"),
                Result->TryGetObjectField(TEXT("structuredContent"), StructuredPtr)
                    && StructuredPtr && StructuredPtr->IsValid()))
        {
            const TSharedPtr<FJsonObject> Structured = *StructuredPtr;
            TestEqual(Label + TEXT(": terminal result is completed"),
                Structured->GetStringField(TEXT("status")), FString(TEXT("completed")));
            TestEqual(Label + TEXT(": terminal result carries the full value"),
                Structured->GetStringField(TEXT("value")), TerminalMarker);
            TestEqual(Label + TEXT(": terminal result carries the executable method"),
                Structured->GetStringField(TEXT("method")),
                FString(TEXT("test.executable")));
            TestFalse(Label + TEXT(": terminal result is not the running ticket"),
                Structured->HasField(TEXT("ticket_id")));
        }
    }
    return true;
}
