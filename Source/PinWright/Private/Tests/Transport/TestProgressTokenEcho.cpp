// Copyright (c) 2026 Alexander Penkin. MIT License.

// The transport must RETAIN the client's params._meta.progressToken, not merely gate on
// its presence. MCP 2025-06-18 (Progress, "Behavior Requirements") admits progress
// notifications only for tokens "provided in an active request", so a frame carrying a
// server-invented token is one a conforming client cannot correlate with the call it is
// waiting on — it looks like progress for some other request, and may be dropped.
//
// The shipped defect: FRequestDecision parsed the token (McpRequestCore.cpp) and threw it
// away, because FPendingCompletion had nowhere to put it; every frame then went out
// carrying the server's job ticket id instead (PinWrightSubsystem.cpp). Nothing caught it
// because the existing SSE framing test injects the notification it later asserts on, so
// it round-trips the FRAMING and never observes which token the server would have chosen.
//
// These tests assert against the retained value, which is the half the emit path cannot
// fake.
#include "Misc/AutomationTest.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/Transport/SocketTestClient.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProgressTokenRetainedForStreamTest,
    "PinWright.transport.progress_token.StreamRetainsTheClientsToken",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FProgressTokenRetainedForStreamTest::RunTest(const FString& Parameters)
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

    TSharedPtr<FJsonValue> Retained;
    if (!TestTrue(TEXT("server retained a progress token for the streaming request"),
            Server->GetProgressToken(Stream.RequestId, Retained)))
    {
        return true;
    }
    if (!TestTrue(TEXT("retained token is a live value"), Retained.IsValid()))
    {
        return true;
    }
    // The exact value, not merely "some token": the whole defect was emitting a
    // well-formed token that the client had never issued.
    TestEqual(TEXT("retained token is byte-identical to the one the client sent"),
        Retained->AsString(), Stream.ProgressToken);

    // Failure direction: an id the server never issued must not answer with a token.
    TSharedPtr<FJsonValue> Bogus;
    TestFalse(TEXT("an unknown request id yields no token"),
        Server->GetProgressToken(TEXT("no-such-request-id"), Bogus));
    return true;
}

// A plain-JSON request sends no token, so there is nothing to retain — and the getter
// must say so rather than inventing one. This is the case that would otherwise tempt a
// fallback back into existence.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProgressTokenAbsentForPlainRequestTest,
    "PinWright.transport.progress_token.PlainRequestRetainsNoToken",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FProgressTokenAbsentForPlainRequestTest::RunTest(const FString& Parameters)
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
    TSharedRef<FDispatchCapture> Capture = MakeShared<FDispatchCapture>();
    BindCaptureDispatcher(*Server, Capture, EDispatchMode::Hold);

    FSocketTestClient Client;
    if (!TestTrue(TEXT("client connected"), Client.Connect(Port)))
    {
        return true;
    }

    // No progress token, no Accept: text/event-stream — the ordinary buffered path.
    if (!TestTrue(TEXT("plain request sent"), Client.SendString(
            BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
                {{TEXT("Content-Type"), TEXT("application/json")},
                 {TEXT("Accept"), TEXT("application/json")}},
                BuildToolsCallBody(2, TEXT("test.plain"),
                    /*bProgressToken=*/false, EWaitArg::Absent)),
            Server.Get())))
    {
        return true;
    }
    if (!TestTrue(TEXT("request reached the dispatcher"),
            WaitForCondition([&Capture]() { return Capture->Num() > 0; }, Server.Get())))
    {
        return true;
    }
    const FString RequestId = Capture->Get(0).RequestId;
    TestFalse(TEXT("the request is not streaming"), Server->IsStreamingRequest(RequestId));

    TSharedPtr<FJsonValue> Retained;
    TestFalse(TEXT("no token retained when the client sent none"),
        Server->GetProgressToken(RequestId, Retained));
    return true;
}
