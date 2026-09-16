// Copyright (c) 2026 Alexander Penkin. MIT License.

// Client-drop resilience: when the client closes its socket mid-stream, the
// server must detect the dead stream (WriteStreamFrame eventually returns
// false), survive ResolveCompletion on the dropped request, and keep serving
// fresh connections.
#include "Misc/AutomationTest.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/Transport/SocketTestClient.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStreamDropClientClosesMidStreamTest,
    "PinWright.transport.stream_drop.ClientClosesMidStream",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStreamDropClientClosesMidStreamTest::RunTest(const FString& Parameters)
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

    FString RequestId;
    FString ProgressToken;
    {
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
        RequestId = Stream.RequestId;
        ProgressToken = Stream.ProgressToken;

        // Prove the stream is live first: one frame flows end to end.
        TestTrue(TEXT("pre-drop frame accepted"), Server->WriteStreamFrame(RequestId,
            BuildProgressNotification(ProgressToken, 1.0)));
        TestTrue(TEXT("pre-drop frame arrived at the client"),
            Client.RecvUntilContains(TEXT("notifications/progress"), Server.Get()));

        // Drop: the client vanishes mid-stream (scope closes the socket).
        Client.Close();
    }

    // Give the server a moment to observe the close.
    for (int32 i = 0; i < 20; ++i)
    {
        PumpOnce(Server.Get());
    }

    // WriteStreamFrame must start reporting the dead stream. The very first
    // write after the drop MAY still "succeed" into the OS send buffer, so
    // allow a bounded run of attempts before requiring false.
    bool bDeadStreamDetected = false;
    for (int32 Attempt = 0; Attempt < 50 && !bDeadStreamDetected; ++Attempt)
    {
        bDeadStreamDetected = !Server->WriteStreamFrame(RequestId,
            BuildProgressNotification(ProgressToken, 2.0 + Attempt));
        for (int32 i = 0; i < 3; ++i)
        {
            PumpOnce(Server.Get(), 0.05f);
        }
    }
    TestTrue(TEXT("WriteStreamFrame returns false once the client is gone"),
        bDeadStreamDetected);

    // ResolveCompletion on the dropped request must not crash; any return
    // value is acceptable — the contract is survival.
    Server->ResolveCompletion(RequestId, /*bSuccess=*/true, TEXT("OK"),
        MakeShared<FJsonObject>(), FString());
    for (int32 i = 0; i < 10; ++i)
    {
        PumpOnce(Server.Get());
    }

    // The connection slot is released: a brand-new connection is served
    // normally (both a fresh stream and a plain buffered request).
    {
        FSocketTestClient Fresh;
        if (!TestTrue(TEXT("post-drop client connected"), Fresh.Connect(Port)))
        {
            return true;
        }
        TestTrue(TEXT("post-drop request sent"), Fresh.SendString(
            BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
                {{TEXT("Content-Type"), TEXT("application/json")}}, BuildPingBody(2)),
            Server.Get()));
        FParsedHttpResponse R;
        int32 Next = 0;
        TestTrue(TEXT("post-drop response received"),
            RecvHttpResponse(Fresh, Server.Get(), R, Next));
        TestEqual(TEXT("post-drop request is served with 200"), R.Code, 200);
    }

    {
        FSocketTestClient FreshStream;
        if (!TestTrue(TEXT("post-drop stream client connected"), FreshStream.Connect(Port)))
        {
            return true;
        }
        const FEstablishedStream Stream2 = EstablishSseStream(*this, FreshStream,
            Server.Get(), Capture, 3);
        if (Stream2.bOk)
        {
            TestTrue(TEXT("a new stream still accepts frames after the drop"),
                Server->WriteStreamFrame(Stream2.RequestId,
                    BuildProgressNotification(Stream2.ProgressToken, 1.0)));
            Server->ResolveCompletion(Stream2.RequestId, true, TEXT("OK"),
                MakeShared<FJsonObject>(), FString());
            FreshStream.RecvUntilClosed(Server.Get());
        }
    }
    return true;
}
