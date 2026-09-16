// Copyright (c) 2026 Alexander Penkin. MIT License.

// HTTP/1.1 request-parsing tests for FSocketHttpServer: request-line and
// header tolerance, split/short/oversized bodies, auth gating, method/path
// routing, and garbage-byte resilience. Each test starts its own server on an
// ephemeral high port and talks to it with a raw loopback socket so the exact
// bytes on the wire are under test control.
#include "Misc/AutomationTest.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/Transport/SocketTestClient.h"

namespace PinWrightSocketHttpParseTest
{
    using namespace PinWrightSocketTest;

    // Common setup: auth-off server + connected client + a bound dispatcher.
    // Returns false (after emitting the assertions-skipped marker) when the environment can't
    // bind a port. Every caller turns that false into `return true`, so without the marker all
    // eight tests would report success having measured nothing.
    struct FParseFixture
    {
        FScopedAuthRequirement Auth{false};
        uint32 Port = 0;
        TSharedPtr<FSocketHttpServer> Server;
        TSharedRef<FDispatchCapture> Capture = MakeShared<FDispatchCapture>();

        bool Init(FAutomationTestBase& Test, EDispatchMode Mode = EDispatchMode::ResolveAll)
        {
            Server = StartServerOnFreePort(Port);
            if (!Server.IsValid())
            {
                PinWrightTestSkip::SkipAssertions(Test, TEXT("test-port-unbindable"),
                    TEXT("Could not bind a test port for FSocketHttpServer; skipping."));
                return false;
            }
            BindCaptureDispatcher(*Server, Capture, Mode);
            return true;
        }

        ~FParseFixture()
        {
            if (Server.IsValid())
            {
                Server->Stop();
            }
        }
    };
}

// ============================================================================
// Request-line + headers: lowercase header names and duplicate headers must
// parse; a plain ping round-trips to 200.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSocketHttpParseHeaderVariantsTest,
    "PinWright.transport.socket_http.Parse.HeaderVariants",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSocketHttpParseHeaderVariantsTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightSocketTest;
    PinWrightSocketHttpParseTest::FParseFixture Fx;
    if (!Fx.Init(*this))
    {
        return true;
    }

    FSocketTestClient Client;
    if (!TestTrue(TEXT("client connected"), Client.Connect(Fx.Port)))
    {
        return true;
    }

    // Lowercase header names + duplicate innocuous headers. Content-Length is
    // hand-rolled lowercase, so the builder's canonical one is suppressed.
    const FString Body = BuildPingBody(1);
    FTCHARToUTF8 BodyUtf8(*Body);
    FString Req = TEXT("POST /mcp HTTP/1.1\r\n");
    Req += TEXT("host: 127.0.0.1\r\n");
    Req += TEXT("content-type: application/json\r\n");
    Req += FString::Printf(TEXT("content-length: %d\r\n"), BodyUtf8.Length());
    Req += TEXT("x-dup: one\r\n");
    Req += TEXT("x-dup: two\r\n");
    Req += TEXT("\r\n");
    Req += Body;
    TestTrue(TEXT("request sent"), Client.SendString(Req, Fx.Server.Get()));

    FParsedHttpResponse R;
    int32 Next = 0;
    TestTrue(TEXT("response received"), RecvHttpResponse(Client, Fx.Server.Get(), R, Next));
    TestEqual(TEXT("HTTP 200 despite lowercase/duplicate headers"), R.Code, 200);
    TestTrue(TEXT("body is the ping result envelope"),
        R.Body.Contains(TEXT("\"result\""), ESearchCase::CaseSensitive));
    return true;
}

// ============================================================================
// Body split across two sends (send half, wait 50ms, send rest) — the server
// must buffer partial bodies per Content-Length, not parse eagerly.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSocketHttpParseSplitBodyTest,
    "PinWright.transport.socket_http.Parse.SplitBody",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSocketHttpParseSplitBodyTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightSocketTest;
    PinWrightSocketHttpParseTest::FParseFixture Fx;
    if (!Fx.Init(*this))
    {
        return true;
    }

    FSocketTestClient Client;
    if (!TestTrue(TEXT("client connected"), Client.Connect(Fx.Port)))
    {
        return true;
    }

    const FString Body = BuildPingBody(2);
    const FString Full = BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
        {{TEXT("Content-Type"), TEXT("application/json")}}, Body);

    // Split mid-body: headers + first half of the body, pause, then the rest.
    FTCHARToUTF8 FullUtf8(*Full);
    TArray<uint8> FullBytes(reinterpret_cast<const uint8*>(FullUtf8.Get()), FullUtf8.Length());
    const int32 SplitAt = FullBytes.Num() - (FullBytes.Num() / 4);
    TArray<uint8> First(FullBytes.GetData(), SplitAt);
    TArray<uint8> Second(FullBytes.GetData() + SplitAt, FullBytes.Num() - SplitAt);

    TestTrue(TEXT("first half sent"), Client.SendBytes(First, Fx.Server.Get()));
    const double PauseUntil = FPlatformTime::Seconds() + 0.05;
    while (FPlatformTime::Seconds() < PauseUntil)
    {
        PumpOnce(Fx.Server.Get());
    }
    TestTrue(TEXT("second half sent"), Client.SendBytes(Second, Fx.Server.Get()));

    FParsedHttpResponse R;
    int32 Next = 0;
    TestTrue(TEXT("response received after split sends"),
        RecvHttpResponse(Client, Fx.Server.Get(), R, Next));
    TestEqual(TEXT("HTTP 200 for split body"), R.Code, 200);
    return true;
}

// ============================================================================
// Missing Content-Length on a POST with a body — the server must answer with
// a 4xx (or close), never hang waiting for an unknowable body length.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSocketHttpParseMissingContentLengthTest,
    "PinWright.transport.socket_http.Parse.MissingContentLength",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSocketHttpParseMissingContentLengthTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightSocketTest;
    PinWrightSocketHttpParseTest::FParseFixture Fx;
    if (!Fx.Init(*this))
    {
        return true;
    }

    FSocketTestClient Client;
    if (!TestTrue(TEXT("client connected"), Client.Connect(Fx.Port)))
    {
        return true;
    }

    const FString Req = BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
        {{TEXT("Content-Type"), TEXT("application/json")}}, BuildPingBody(3),
        /*bIncludeContentLength=*/false);
    TestTrue(TEXT("request sent"), Client.SendString(Req, Fx.Server.Get()));

    // Accept either an explicit 4xx response or a clean close within the
    // deadline — a hang here is the actual regression.
    const bool bAnswered = WaitForCondition([&Client]()
        {
            if (Client.ReceivedString().Contains(TEXT("HTTP/1.1 4"), ESearchCase::CaseSensitive))
            {
                return true;
            }
            return Client.RecvStep() == ERecvStep::Closed;
        }, Fx.Server.Get());
    TestTrue(TEXT("missing Content-Length answered with 4xx or clean close (no hang)"), bAnswered);
    return true;
}

// ============================================================================
// Mismatched Content-Length: the client claims more bytes than it sends, then
// closes. The half-request must be discarded without wedging the server — a
// fresh connection afterwards is served normally.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSocketHttpParseTruncatedBodyTest,
    "PinWright.transport.socket_http.Parse.TruncatedBodyThenClose",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSocketHttpParseTruncatedBodyTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightSocketTest;
    PinWrightSocketHttpParseTest::FParseFixture Fx;
    if (!Fx.Init(*this))
    {
        return true;
    }

    {
        FSocketTestClient Liar;
        if (!TestTrue(TEXT("liar client connected"), Liar.Connect(Fx.Port)))
        {
            return true;
        }
        // Claim 4096 body bytes, deliver 20, then vanish.
        FString Req = TEXT("POST /mcp HTTP/1.1\r\n");
        Req += TEXT("Host: 127.0.0.1\r\n");
        Req += TEXT("Content-Type: application/json\r\n");
        Req += TEXT("Content-Length: 4096\r\n");
        Req += TEXT("\r\n");
        Req += TEXT("{\"jsonrpc\":\"2.0\",");
        TestTrue(TEXT("truncated request sent"), Liar.SendString(Req, Fx.Server.Get()));
        // Give the server a moment to buffer the partial body, then close.
        for (int32 i = 0; i < 10; ++i)
        {
            PumpOnce(Fx.Server.Get());
        }
        Liar.Close();
    }
    for (int32 i = 0; i < 20; ++i)
    {
        PumpOnce(Fx.Server.Get());
    }

    // The server must still serve a well-formed request on a new connection.
    FSocketTestClient Client;
    if (!TestTrue(TEXT("follow-up client connected"), Client.Connect(Fx.Port)))
    {
        return true;
    }
    TestTrue(TEXT("follow-up request sent"), Client.SendString(
        BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
            {{TEXT("Content-Type"), TEXT("application/json")}}, BuildPingBody(4)),
        Fx.Server.Get()));

    FParsedHttpResponse R;
    int32 Next = 0;
    TestTrue(TEXT("follow-up response received"), RecvHttpResponse(Client, Fx.Server.Get(), R, Next));
    TestEqual(TEXT("HTTP 200 after a truncated peer"), R.Code, 200);
    return true;
}

// ============================================================================
// Oversized body — 413. The declared Content-Length exceeds the 1MB default
// cap; body bytes are drip-fed so both header-time and read-time rejection
// designs terminate with a 413 instead of a stall.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSocketHttpParseOversized413Test,
    "PinWright.transport.socket_http.Parse.Oversized413",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSocketHttpParseOversized413Test::RunTest(const FString& Parameters)
{
    using namespace PinWrightSocketTest;
    PinWrightSocketHttpParseTest::FParseFixture Fx;
    if (!Fx.Init(*this))
    {
        return true;
    }

    FSocketTestClient Client;
    if (!TestTrue(TEXT("client connected"), Client.Connect(Fx.Port)))
    {
        return true;
    }

    const int32 ClaimedBytes = 2 * 1024 * 1024 + 64;  // over the 1MB default cap
    FString Head = TEXT("POST /mcp HTTP/1.1\r\n");
    Head += TEXT("Host: 127.0.0.1\r\n");
    Head += TEXT("Content-Type: application/json\r\n");
    Head += FString::Printf(TEXT("Content-Length: %d\r\n"), ClaimedBytes);
    Head += TEXT("\r\n");
    TestTrue(TEXT("oversized head sent"), Client.SendString(Head, Fx.Server.Get()));

    // Drip the body in 64KB chunks, stopping as soon as a response (or close)
    // shows up. A header-time reject answers before any body arrives; a
    // read-time reject answers once the cap is crossed.
    TArray<uint8> Chunk;
    Chunk.Init(static_cast<uint8>('a'), 64 * 1024);
    int32 SentBytes = 0;
    const double Deadline = FPlatformTime::Seconds() + 10.0;
    bool bGotAnswer = false;
    while (FPlatformTime::Seconds() < Deadline)
    {
        if (Client.ReceivedString().Contains(TEXT("HTTP/1.1 413"), ESearchCase::CaseSensitive))
        {
            bGotAnswer = true;
            break;
        }
        const ERecvStep Step = Client.RecvStep();
        if (Step == ERecvStep::Closed)
        {
            break;  // reject-and-close; the 413 check below decides pass/fail
        }
        if (SentBytes < ClaimedBytes)
        {
            if (Client.SendBytes(Chunk, Fx.Server.Get(), /*TimeoutSeconds=*/1.0))
            {
                SentBytes += Chunk.Num();
            }
            else
            {
                // Send refused — the server likely already tore the connection down.
                for (int32 i = 0; i < 5; ++i)
                {
                    PumpOnce(Fx.Server.Get());
                }
            }
        }
        else
        {
            PumpOnce(Fx.Server.Get());
        }
    }

    TestTrue(TEXT("oversized request answered with HTTP 413"),
        bGotAnswer || Client.ReceivedString().Contains(TEXT("HTTP/1.1 413"), ESearchCase::CaseSensitive));
    return true;
}

// ============================================================================
// Auth: token required, no Authorization header — 401.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSocketHttpParseAuthMissing401Test,
    "PinWright.transport.socket_http.Parse.AuthMissing401",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSocketHttpParseAuthMissing401Test::RunTest(const FString& Parameters)
{
    using namespace PinWrightSocketTest;
    FScopedAuthRequirement Auth(true);
    uint32 Port = 0;
    TSharedPtr<FSocketHttpServer> Server = StartServerOnFreePort(Port);
    if (!Server.IsValid())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("test-port-unbindable"),
            TEXT("Could not bind a test port for FSocketHttpServer; skipping."));
        return true;
    }
    ON_SCOPE_EXIT { Server->Stop(); };
    BindCaptureDispatcher(*Server, MakeShared<PinWrightSocketTest::FDispatchCapture>(),
        EDispatchMode::ResolveAll);

    FSocketTestClient Client;
    if (!TestTrue(TEXT("client connected"), Client.Connect(Port)))
    {
        return true;
    }
    TestTrue(TEXT("request sent"), Client.SendString(
        BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
            {{TEXT("Content-Type"), TEXT("application/json")}}, BuildPingBody(5)),
        Server.Get()));

    FParsedHttpResponse R;
    int32 Next = 0;
    TestTrue(TEXT("response received"), RecvHttpResponse(Client, Server.Get(), R, Next));
    TestEqual(TEXT("HTTP 401 without Authorization"), R.Code, 401);
    TestFalse(TEXT("401 carries a Content-Type"), GetHeader(R, TEXT("content-type")).IsEmpty());
    // RFC 7235: a 401 MUST carry a challenge naming the expected scheme.
    TestEqual(TEXT("401 carries WWW-Authenticate: Bearer"),
        GetHeader(R, TEXT("www-authenticate")), FString(TEXT("Bearer")));
    return true;
}

// ============================================================================
// Auth: token required, wrong bearer credentials — 401; correct — 200.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSocketHttpParseAuthWrongBearer401Test,
    "PinWright.transport.socket_http.Parse.AuthWrongBearer401",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSocketHttpParseAuthWrongBearer401Test::RunTest(const FString& Parameters)
{
    using namespace PinWrightSocketTest;
    FScopedAuthRequirement Auth(true);
    uint32 Port = 0;
    TSharedPtr<FSocketHttpServer> Server = StartServerOnFreePort(Port);
    if (!Server.IsValid())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("test-port-unbindable"),
            TEXT("Could not bind a test port for FSocketHttpServer; skipping."));
        return true;
    }
    ON_SCOPE_EXIT { Server->Stop(); };
    BindCaptureDispatcher(*Server, MakeShared<PinWrightSocketTest::FDispatchCapture>(),
        EDispatchMode::ResolveAll);

    // Wrong bearer -> 401.
    {
        FSocketTestClient Client;
        if (!TestTrue(TEXT("client connected (wrong token)"), Client.Connect(Port)))
        {
            return true;
        }
        TestTrue(TEXT("request sent (wrong token)"), Client.SendString(
            BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
                {{TEXT("Content-Type"), TEXT("application/json")},
                 {TEXT("Authorization"), TEXT("Bearer deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef")}},
                BuildPingBody(6)),
            Server.Get()));

        FParsedHttpResponse R;
        int32 Next = 0;
        TestTrue(TEXT("response received (wrong token)"), RecvHttpResponse(Client, Server.Get(), R, Next));
        TestEqual(TEXT("HTTP 401 for wrong bearer"), R.Code, 401);
        // RFC 7235: a 401 MUST carry a challenge naming the expected scheme.
        TestEqual(TEXT("401 carries WWW-Authenticate: Bearer"),
            GetHeader(R, TEXT("www-authenticate")), FString(TEXT("Bearer")));
    }

    // Correct bearer (real gateway token) -> 200, proving the 401 above is the
    // credential check, not a broken auth pipeline.
    {
        FSocketTestClient Client;
        if (!TestTrue(TEXT("client connected (correct token)"), Client.Connect(Port)))
        {
            return true;
        }
        TestTrue(TEXT("request sent (correct token)"), Client.SendString(
            BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
                {{TEXT("Content-Type"), TEXT("application/json")}, MakeAuthHeader()},
                BuildPingBody(7)),
            Server.Get()));

        FParsedHttpResponse R;
        int32 Next = 0;
        TestTrue(TEXT("response received (correct token)"), RecvHttpResponse(Client, Server.Get(), R, Next));
        TestEqual(TEXT("HTTP 200 for the real gateway token"), R.Code, 200);
    }

    // Duplicate Authorization fields remain independently parseable: one bad
    // credential must not hide a later valid bearer value during header folding.
    {
        FSocketTestClient Client;
        if (!TestTrue(TEXT("client connected (duplicate auth)"), Client.Connect(Port)))
        {
            return true;
        }
        TestTrue(TEXT("request sent (duplicate auth)"), Client.SendString(
            BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
                {{TEXT("Content-Type"), TEXT("application/json")},
                 {TEXT("Authorization"), TEXT("Bearer deadbeef")},
                 MakeAuthHeader()},
                BuildPingBody(8)),
            Server.Get()));

        FParsedHttpResponse R;
        int32 Next = 0;
        TestTrue(TEXT("response received (duplicate auth)"),
            RecvHttpResponse(Client, Server.Get(), R, Next));
        TestEqual(TEXT("valid duplicate Authorization is accepted"), R.Code, 200);
    }
    return true;
}

// ============================================================================
// GET /mcp — 405 Method Not Allowed.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSocketHttpParseGet405Test,
    "PinWright.transport.socket_http.Parse.GetMcp405",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSocketHttpParseGet405Test::RunTest(const FString& Parameters)
{
    using namespace PinWrightSocketTest;
    PinWrightSocketHttpParseTest::FParseFixture Fx;
    if (!Fx.Init(*this))
    {
        return true;
    }

    FSocketTestClient Client;
    if (!TestTrue(TEXT("client connected"), Client.Connect(Fx.Port)))
    {
        return true;
    }
    TestTrue(TEXT("GET sent"), Client.SendString(
        BuildHttpRequest(TEXT("GET"), TEXT("/mcp"), {}, FString(), /*bIncludeContentLength=*/false),
        Fx.Server.Get()));

    FParsedHttpResponse R;
    int32 Next = 0;
    TestTrue(TEXT("response received"), RecvHttpResponse(Client, Fx.Server.Get(), R, Next));
    TestEqual(TEXT("GET /mcp is 405"), R.Code, 405);
    return true;
}

// ============================================================================
// GET /.well-known/oauth-authorization-server — 404 (unknown path, NOT an
// OAuth discovery endpoint; MCP clients probe this and must get a clean 404).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSocketHttpParseWellKnown404Test,
    "PinWright.transport.socket_http.Parse.WellKnownPath404",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSocketHttpParseWellKnown404Test::RunTest(const FString& Parameters)
{
    using namespace PinWrightSocketTest;
    PinWrightSocketHttpParseTest::FParseFixture Fx;
    if (!Fx.Init(*this))
    {
        return true;
    }

    FSocketTestClient Client;
    if (!TestTrue(TEXT("client connected"), Client.Connect(Fx.Port)))
    {
        return true;
    }
    TestTrue(TEXT("well-known probe sent"), Client.SendString(
        BuildHttpRequest(TEXT("GET"), TEXT("/.well-known/oauth-authorization-server"),
            {}, FString(), /*bIncludeContentLength=*/false),
        Fx.Server.Get()));

    FParsedHttpResponse R;
    int32 Next = 0;
    TestTrue(TEXT("response received"), RecvHttpResponse(Client, Fx.Server.Get(), R, Next));
    TestEqual(TEXT("unknown path is 404"), R.Code, 404);
    return true;
}

// ============================================================================
// Garbage bytes — a non-HTTP byte salad must produce a clean 400 or an
// immediate close, never a hang, and must not wedge subsequent connections.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSocketHttpParseGarbageBytesTest,
    "PinWright.transport.socket_http.Parse.GarbageBytes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSocketHttpParseGarbageBytesTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightSocketTest;
    PinWrightSocketHttpParseTest::FParseFixture Fx;
    if (!Fx.Init(*this))
    {
        return true;
    }

    {
        FSocketTestClient Client;
        if (!TestTrue(TEXT("garbage client connected"), Client.Connect(Fx.Port)))
        {
            return true;
        }
        TArray<uint8> Garbage;
        Garbage.Append({0x01, 0xFF, 0xFE, 0x00, 0x7F});
        for (int32 i = 0; i < 64; ++i)
        {
            Garbage.Add(static_cast<uint8>(0x80 + (i % 0x40)));
        }
        Garbage.Append({'\r', '\n', '\r', '\n'});
        TestTrue(TEXT("garbage sent"), Client.SendBytes(Garbage, Fx.Server.Get()));

        const bool bAnswered = WaitForCondition([&Client]()
            {
                if (Client.ReceivedString().Contains(TEXT("HTTP/1.1 4"), ESearchCase::CaseSensitive))
                {
                    return true;
                }
                return Client.RecvStep() == ERecvStep::Closed;
            }, Fx.Server.Get());
        TestTrue(TEXT("garbage answered with 4xx or clean close (no hang)"), bAnswered);
    }

    // The listener must survive: a valid follow-up request still round-trips.
    FSocketTestClient Client;
    if (!TestTrue(TEXT("follow-up client connected"), Client.Connect(Fx.Port)))
    {
        return true;
    }
    TestTrue(TEXT("follow-up sent"), Client.SendString(
        BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
            {{TEXT("Content-Type"), TEXT("application/json")}}, BuildPingBody(8)),
        Fx.Server.Get()));

    FParsedHttpResponse R;
    int32 Next = 0;
    TestTrue(TEXT("follow-up response received"), RecvHttpResponse(Client, Fx.Server.Get(), R, Next));
    TestEqual(TEXT("HTTP 200 after garbage peer"), R.Code, 200);
    return true;
}
