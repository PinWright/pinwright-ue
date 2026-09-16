// Copyright (c) 2026 Alexander Penkin. MIT License.

// HTTP protocol-correctness tests for FSocketHttpServer: Content-Type on
// every status, accurate Content-Length framing, keep-alive request
// sequencing on one connection, Expect: 100-continue, and Origin filtering.
#include "Misc/AutomationTest.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/Transport/SocketTestClient.h"

namespace PinWrightSocketHttpCorrectnessTest
{
    using namespace PinWrightSocketTest;

    // Asserts the response carries a Content-Type and — when Content-Length is
    // present — that the declared length matched the bytes actually framed
    // (ParseOneHttpResponse only completes once Content-Length bytes arrived)
    // with nothing extra dribbling in afterwards.
    inline void AssertResponseFraming(FAutomationTestBase& Test, const TCHAR* Label,
        FSocketTestClient& Client, FSocketHttpServer* Server,
        const FParsedHttpResponse& R, int32 NextOffset)
    {
        Test.TestFalse(FString::Printf(TEXT("%s: Content-Type header present"), Label),
            GetHeader(R, TEXT("content-type")).IsEmpty());

        const FString ContentLength = GetHeader(R, TEXT("content-length"));
        if (!ContentLength.IsEmpty())
        {
            Test.TestEqual(FString::Printf(TEXT("%s: Content-Length matches framed body bytes"), Label),
                FCString::Atoi(*ContentLength), R.BodyByteLen);

            // Drain briefly: no bytes may exist beyond the declared body on a
            // single-request connection (that would desync keep-alive).
            for (int32 i = 0; i < 10; ++i)
            {
                if (Client.RecvStep() != ERecvStep::Data)
                {
                    PumpOnce(Server);
                }
            }
            Test.TestEqual(FString::Printf(TEXT("%s: no stray bytes after the declared body"), Label),
                Client.Received.Num(), NextOffset);
        }
    }

    // One-shot: fresh connection, send raw request text, parse one response.
    inline bool RoundTrip(FAutomationTestBase& Test, const TCHAR* Label,
        uint32 Port, FSocketHttpServer* Server, const FString& RawRequest,
        FParsedHttpResponse& OutResponse)
    {
        FSocketTestClient Client;
        if (!Test.TestTrue(FString::Printf(TEXT("%s: connected"), Label), Client.Connect(Port)))
        {
            return false;
        }
        if (!Test.TestTrue(FString::Printf(TEXT("%s: request sent"), Label),
                Client.SendString(RawRequest, Server)))
        {
            return false;
        }
        int32 Next = 0;
        if (!Test.TestTrue(FString::Printf(TEXT("%s: response received"), Label),
                RecvHttpResponse(Client, Server, OutResponse, Next)))
        {
            return false;
        }
        AssertResponseFraming(Test, Label, Client, Server, OutResponse, Next);
        return true;
    }
}

// ============================================================================
// Every response carries a Content-Type + accurate Content-Length: 200
// (ping), 202-equivalent (notification ack), 400 (bad JSON), 404 (unknown
// path), 405 (GET /mcp), 413 (oversized claim), and 401 (auth on).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSocketHttpCorrectnessContentTypeEverywhereTest,
    "PinWright.transport.socket_http.Correctness.ContentTypeAndLengthEveryStatus",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSocketHttpCorrectnessContentTypeEverywhereTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightSocketTest;
    using namespace PinWrightSocketHttpCorrectnessTest;

    // --- Auth-off server: 200 / 202 / 400 / 404 / 405 / 413 ---
    {
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
        BindCaptureDispatcher(*Server, MakeShared<FDispatchCapture>(), EDispatchMode::ResolveAll);

        FParsedHttpResponse R;

        if (RoundTrip(*this, TEXT("200 ping"), Port, Server.Get(),
                BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
                    {{TEXT("Content-Type"), TEXT("application/json")}}, BuildPingBody(1)), R))
        {
            TestEqual(TEXT("ping status is 200"), R.Code, 200);
            TestTrue(TEXT("200 Content-Type is application/json"),
                GetHeader(R, TEXT("content-type")).Contains(TEXT("application/json")));
        }

        // 202-equivalent: a notification (no id) is acked with an empty body.
        if (RoundTrip(*this, TEXT("202 notification"), Port, Server.Get(),
                BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
                    {{TEXT("Content-Type"), TEXT("application/json")}},
                    TEXT("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}")), R))
        {
            TestEqual(TEXT("notification ack status is 202"), R.Code, 202);
            TestTrue(TEXT("202 body is empty"), R.Body.IsEmpty());
        }

        if (RoundTrip(*this, TEXT("400 bad json"), Port, Server.Get(),
                BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
                    {{TEXT("Content-Type"), TEXT("application/json")}}, TEXT("{not json")), R))
        {
            TestEqual(TEXT("malformed JSON status is 400"), R.Code, 400);
        }

        if (RoundTrip(*this, TEXT("404 unknown path"), Port, Server.Get(),
                BuildHttpRequest(TEXT("POST"), TEXT("/nope"),
                    {{TEXT("Content-Type"), TEXT("application/json")}}, BuildPingBody(2)), R))
        {
            TestEqual(TEXT("unknown path status is 404"), R.Code, 404);
        }

        if (RoundTrip(*this, TEXT("405 GET /mcp"), Port, Server.Get(),
                BuildHttpRequest(TEXT("GET"), TEXT("/mcp"), {}, FString(),
                    /*bIncludeContentLength=*/false), R))
        {
            TestEqual(TEXT("GET /mcp status is 405"), R.Code, 405);
        }

        // 413: oversized Content-Length claim; body never fully sent — the
        // response (however produced) must still carry Content-Type.
        {
            FSocketTestClient Client;
            if (TestTrue(TEXT("413: connected"), Client.Connect(Port)))
            {
                FString Head = TEXT("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n");
                Head += TEXT("Content-Type: application/json\r\n");
                Head += FString::Printf(TEXT("Content-Length: %d\r\n\r\n"), 4 * 1024 * 1024);
                TestTrue(TEXT("413: head sent"), Client.SendString(Head, Server.Get()));
                // Feed some body so read-then-reject designs cross the cap too.
                TArray<uint8> Chunk;
                Chunk.Init(static_cast<uint8>('b'), 64 * 1024);
                const double Deadline = FPlatformTime::Seconds() + 10.0;
                while (FPlatformTime::Seconds() < Deadline &&
                       !Client.ReceivedString().Contains(TEXT("HTTP/1.1"), ESearchCase::CaseSensitive))
                {
                    if (Client.RecvStep() == ERecvStep::Closed)
                    {
                        break;
                    }
                    if (!Client.SendBytes(Chunk, Server.Get(), 1.0))
                    {
                        for (int32 i = 0; i < 5; ++i) { PumpOnce(Server.Get()); }
                    }
                }
                int32 Next = 0;
                FParsedHttpResponse R413;
                if (TestTrue(TEXT("413: response received"),
                        RecvHttpResponse(Client, Server.Get(), R413, Next)))
                {
                    TestEqual(TEXT("oversized status is 413"), R413.Code, 413);
                    TestFalse(TEXT("413: Content-Type header present"),
                        GetHeader(R413, TEXT("content-type")).IsEmpty());
                }
            }
        }
    }

    // --- Auth-on server: 401 ---
    {
        FScopedAuthRequirement Auth(true);
        uint32 Port = 0;
        TSharedPtr<FSocketHttpServer> Server = StartServerOnFreePort(Port);
        if (!Server.IsValid())
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("test-port-unbindable"),
                TEXT("Could not bind an auth-on test port; skipping the 401 leg."));
            return true;
        }
        ON_SCOPE_EXIT { Server->Stop(); };
        BindCaptureDispatcher(*Server, MakeShared<FDispatchCapture>(), EDispatchMode::ResolveAll);

        FParsedHttpResponse R;
        if (RoundTrip(*this, TEXT("401 no auth"), Port, Server.Get(),
                BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
                    {{TEXT("Content-Type"), TEXT("application/json")}}, BuildPingBody(3)), R))
        {
            TestEqual(TEXT("unauthenticated status is 401"), R.Code, 401);
        }
    }
    return true;
}

// ============================================================================
// Keep-alive: two sequential requests on ONE connection are both answered
// in order with no byte desync between the framed responses.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSocketHttpCorrectnessKeepAliveTest,
    "PinWright.transport.socket_http.Correctness.KeepAliveTwoRequests",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSocketHttpCorrectnessKeepAliveTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightSocketTest;
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
    BindCaptureDispatcher(*Server, MakeShared<FDispatchCapture>(), EDispatchMode::ResolveAll);

    FSocketTestClient Client;
    if (!TestTrue(TEXT("client connected"), Client.Connect(Port)))
    {
        return true;
    }

    // Request 1.
    TestTrue(TEXT("request 1 sent"), Client.SendString(
        BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
            {{TEXT("Content-Type"), TEXT("application/json")}}, BuildPingBody(101)),
        Server.Get()));
    FParsedHttpResponse R1;
    int32 AfterFirst = 0;
    TestTrue(TEXT("response 1 received"), RecvHttpResponse(Client, Server.Get(), R1, AfterFirst));
    TestEqual(TEXT("response 1 is 200"), R1.Code, 200);
    TestTrue(TEXT("response 1 echoes id 101"),
        R1.Body.Contains(TEXT("101"), ESearchCase::CaseSensitive));

    // Request 2 on the SAME connection; parse strictly after response 1's bytes.
    TestTrue(TEXT("request 2 sent"), Client.SendString(
        BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
            {{TEXT("Content-Type"), TEXT("application/json")}}, BuildPingBody(202)),
        Server.Get()));
    FParsedHttpResponse R2;
    int32 AfterSecond = 0;
    TestTrue(TEXT("response 2 received on the same connection"),
        RecvHttpResponse(Client, Server.Get(), R2, AfterSecond,
            kDefaultTimeoutSeconds, /*Offset=*/AfterFirst));
    TestEqual(TEXT("response 2 is 200"), R2.Code, 200);
    TestTrue(TEXT("response 2 echoes id 202 (no desync)"),
        R2.Body.Contains(TEXT("202"), ESearchCase::CaseSensitive));
    TestFalse(TEXT("response 2 does not re-echo id 101"),
        R2.Body.Contains(TEXT("101"), ESearchCase::CaseSensitive));
    return true;
}

// ============================================================================
// Expect: 100-continue — the request must complete (100 interim + final, or
// final directly). What it must NEVER do is stall waiting for a body the
// client is holding back pending the interim response.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSocketHttpCorrectnessExpect100Test,
    "PinWright.transport.socket_http.Correctness.Expect100Continue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSocketHttpCorrectnessExpect100Test::RunTest(const FString& Parameters)
{
    using namespace PinWrightSocketTest;
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
    BindCaptureDispatcher(*Server, MakeShared<FDispatchCapture>(), EDispatchMode::ResolveAll);

    FSocketTestClient Client;
    if (!TestTrue(TEXT("client connected"), Client.Connect(Port)))
    {
        return true;
    }

    // Send the head only (with Expect), hold the body back like a real client.
    const FString Body = BuildPingBody(9);
    FTCHARToUTF8 BodyUtf8(*Body);
    FString Head = TEXT("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n");
    Head += TEXT("Content-Type: application/json\r\n");
    Head += TEXT("Expect: 100-continue\r\n");
    Head += FString::Printf(TEXT("Content-Length: %d\r\n\r\n"), BodyUtf8.Length());
    TestTrue(TEXT("head sent"), Client.SendString(Head, Server.Get()));

    // Wait up to ~1.5s for a 100 Continue interim. Its absence is tolerated —
    // servers MAY skip it — but the final response must then still arrive
    // once we send the body.
    const bool bGot100 = Client.RecvUntilContains(TEXT("HTTP/1.1 100"), Server.Get(), 1.5);
    TestTrue(TEXT("body sent"), Client.SendString(Body, Server.Get()));

    // Parse past the interim response if one arrived.
    int32 Offset = 0;
    if (bGot100)
    {
        FParsedHttpResponse Interim;
        int32 Next = 0;
        if (TestTrue(TEXT("interim 100 parses"),
                RecvHttpResponse(Client, Server.Get(), Interim, Next)))
        {
            TestEqual(TEXT("interim status is 100"), Interim.Code, 100);
            Offset = Next;
        }
    }

    FParsedHttpResponse Final;
    int32 Next = 0;
    TestTrue(TEXT("final response arrives (no 100-continue stall)"),
        RecvHttpResponse(Client, Server.Get(), Final, Next, kDefaultTimeoutSeconds, Offset));
    TestEqual(TEXT("final response is 200"), Final.Code, 200);
    return true;
}

// ============================================================================
// Origin filtering: a non-local Origin is rejected with 403 (DNS-rebinding
// defense); localhost or an absent Origin passes.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSocketHttpCorrectnessOriginEvilTest,
    "PinWright.transport.socket_http.Correctness.OriginEvil403",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSocketHttpCorrectnessOriginEvilTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightSocketTest;
    using namespace PinWrightSocketHttpCorrectnessTest;
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
    BindCaptureDispatcher(*Server, MakeShared<FDispatchCapture>(), EDispatchMode::ResolveAll);

    FParsedHttpResponse R;
    if (RoundTrip(*this, TEXT("evil origin"), Port, Server.Get(),
            BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
                {{TEXT("Content-Type"), TEXT("application/json")},
                 {TEXT("Origin"), TEXT("http://evil.example")}},
                BuildPingBody(10)), R))
    {
        TestEqual(TEXT("non-local Origin is 403"), R.Code, 403);
    }
    if (RoundTrip(*this, TEXT("localhost prefix lookalike"), Port, Server.Get(),
            BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
                {{TEXT("Content-Type"), TEXT("application/json")},
                 {TEXT("Origin"), TEXT("http://localhost.evil.example")}},
                BuildPingBody(11)), R))
    {
        TestEqual(TEXT("localhost prefix lookalike is 403"), R.Code, 403);
    }
    if (RoundTrip(*this, TEXT("localhost userinfo smuggling"), Port, Server.Get(),
            BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
                {{TEXT("Content-Type"), TEXT("application/json")},
                 {TEXT("Origin"), TEXT("http://localhost:80@evil.example")}},
                BuildPingBody(12)), R))
    {
        TestEqual(TEXT("localhost userinfo smuggling is 403"), R.Code, 403);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSocketHttpCorrectnessOriginLocalhostTest,
    "PinWright.transport.socket_http.Correctness.OriginLocalhostAllowed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSocketHttpCorrectnessOriginLocalhostTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightSocketTest;
    using namespace PinWrightSocketHttpCorrectnessTest;
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
    BindCaptureDispatcher(*Server, MakeShared<FDispatchCapture>(), EDispatchMode::ResolveAll);

    FParsedHttpResponse R;
    if (RoundTrip(*this, TEXT("localhost origin"), Port, Server.Get(),
            BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
                {{TEXT("Content-Type"), TEXT("application/json")},
                 {TEXT("Origin"), TEXT("http://localhost:1234")}},
                BuildPingBody(11)), R))
    {
        TestEqual(TEXT("localhost Origin is served (200)"), R.Code, 200);
    }
    if (RoundTrip(*this, TEXT("case-insensitive localhost origin"), Port, Server.Get(),
            BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
                {{TEXT("Content-Type"), TEXT("application/json")},
                 {TEXT("Origin"), TEXT("HTTP://LOCALHOST:1234")}},
                BuildPingBody(12)), R))
    {
        TestEqual(TEXT("case-insensitive localhost Origin is served (200)"), R.Code, 200);
    }

    // Absent Origin is the normal non-browser client path — also served.
    if (RoundTrip(*this, TEXT("no origin"), Port, Server.Get(),
            BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
                {{TEXT("Content-Type"), TEXT("application/json")}},
                BuildPingBody(12)), R))
    {
        TestEqual(TEXT("absent Origin is served (200)"), R.Code, 200);
    }
    return true;
}
