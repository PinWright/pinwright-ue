// Copyright (c) 2026 Alexander Penkin. MIT License.

// Full streaming lifecycle: upgrade -> two ordered progress frames (the first
// carrying the job ticket_id) -> exactly one terminal result frame -> server
// closes the connection.
#include "Misc/AutomationTest.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/Transport/SocketTestClient.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStreamLifecycleFullTest,
    "PinWright.transport.stream_lifecycle.ProgressFramesThenTerminalThenClose",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStreamLifecycleFullTest::RunTest(const FString& Parameters)
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
    TestTrue(TEXT("server reports the request as streaming"),
        Server->IsStreamingRequest(Stream.RequestId));

    // Progress frame 1 — carries the job ticket_id (the streamed-job contract:
    // the first notification tells the client which ticket to poll on drop).
    const FString TicketId = TEXT("job_ticket_abc123");
    {
        TSharedPtr<FJsonObject> Extra = MakeShared<FJsonObject>();
        Extra->SetStringField(TEXT("ticket_id"), TicketId);
        Extra->SetStringField(TEXT("message"), TEXT("phase one"));
        TestTrue(TEXT("progress frame 1 written"), Server->WriteStreamFrame(Stream.RequestId,
            BuildProgressNotification(Stream.ProgressToken, 1.0, Extra)));
    }

    // Progress frame 2.
    {
        TSharedPtr<FJsonObject> Extra = MakeShared<FJsonObject>();
        Extra->SetStringField(TEXT("message"), TEXT("phase two"));
        TestTrue(TEXT("progress frame 2 written"), Server->WriteStreamFrame(Stream.RequestId,
            BuildProgressNotification(Stream.ProgressToken, 2.0, Extra)));
    }

    // Terminal: resolve the completion with a marked result.
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("done"), true);
    Result->SetStringField(TEXT("marker"), TEXT("terminal-xyz"));
    TestTrue(TEXT("ResolveCompletion accepted"), Server->ResolveCompletion(
        Stream.RequestId, /*bSuccess=*/true, TEXT("OK"), Result, FString()));

    // The connection must close after the terminal frame; drain everything.
    TestTrue(TEXT("server closes the connection after the terminal frame"),
        Client.RecvUntilClosed(Server.Get(), /*TimeoutSeconds=*/10.0));

    // Parse the whole stream and assert order + content.
    const TArray<FSseEvent> DataEvents =
        DataEventsOnly(ParseSseEvents(Client.Received, Stream.Head.BodyStart));
    if (!TestEqual(TEXT("exactly 3 data frames: 2 progress + 1 terminal"), DataEvents.Num(), 3))
    {
        return true;
    }

    // Frame 1: progress notification carrying the ticket_id.
    {
        TSharedPtr<FJsonObject> Frame = ParseJsonObject(DataEvents[0].Data);
        if (TestTrue(TEXT("frame 1 parses"), Frame.IsValid()))
        {
            TestEqual(TEXT("frame 1 is notifications/progress"),
                Frame->GetStringField(TEXT("method")),
                FString(TEXT("notifications/progress")));
            const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
            if (TestTrue(TEXT("frame 1 params present"),
                    Frame->TryGetObjectField(TEXT("params"), ParamsPtr) && ParamsPtr))
            {
                TestEqual(TEXT("frame 1 carries the ticket_id"),
                    (*ParamsPtr)->GetStringField(TEXT("ticket_id")), TicketId);
                TestEqual(TEXT("frame 1 progressToken matches the request's token"),
                    (*ParamsPtr)->GetStringField(TEXT("progressToken")), Stream.ProgressToken);
            }
        }
    }

    // Frame 2: the second progress notification, in send order.
    {
        TSharedPtr<FJsonObject> Frame = ParseJsonObject(DataEvents[1].Data);
        if (TestTrue(TEXT("frame 2 parses"), Frame.IsValid()))
        {
            TestEqual(TEXT("frame 2 is notifications/progress"),
                Frame->GetStringField(TEXT("method")),
                FString(TEXT("notifications/progress")));
            const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
            if (Frame->TryGetObjectField(TEXT("params"), ParamsPtr) && ParamsPtr)
            {
                TestEqual(TEXT("frame 2 is the 'phase two' progress event"),
                    (*ParamsPtr)->GetStringField(TEXT("message")), FString(TEXT("phase two")));
            }
        }
    }

    // Frame 3 (last): the terminal JSON-RPC response — has `result`, is not a
    // notification, and carries the resolved payload.
    {
        TSharedPtr<FJsonObject> Frame = ParseJsonObject(DataEvents[2].Data);
        if (TestTrue(TEXT("terminal frame parses"), Frame.IsValid()))
        {
            TestFalse(TEXT("terminal frame is not a notification (no method field)"),
                Frame->HasField(TEXT("method")));
            TestTrue(TEXT("terminal frame carries a result"),
                Frame->HasField(TEXT("result")));
            TestTrue(TEXT("terminal frame carries the resolved marker payload"),
                DataEvents[2].Data.Contains(TEXT("terminal-xyz"), ESearchCase::CaseSensitive));
        }
    }

    // Exactly one terminal: neither progress frame may carry a result.
    TestFalse(TEXT("frame 1 is not a terminal"),
        DataEvents[0].Data.Contains(TEXT("\"result\""), ESearchCase::CaseSensitive));
    TestFalse(TEXT("frame 2 is not a terminal"),
        DataEvents[1].Data.Contains(TEXT("\"result\""), ESearchCase::CaseSensitive));
    return true;
}
