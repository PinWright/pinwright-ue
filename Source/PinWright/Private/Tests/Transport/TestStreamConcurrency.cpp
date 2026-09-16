// Copyright (c) 2026 Alexander Penkin. MIT License.

// Concurrency isolation: 8 simultaneous SSE streams receive ONLY their own
// interleaved frames, 4 buffered requests are served while the streams are
// open, and FailAllCompletions terminates every stream with an error frame
// and a clean close.
#include "Misc/AutomationTest.h"
#include "Tests/Transport/SocketTestClient.h"
#include "Tests/TestSkipReporting.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStreamConcurrencyEightStreamsTest,
    "PinWright.transport.stream_concurrency.EightStreamsFourBufferedFailAll",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStreamConcurrencyEightStreamsTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightSocketTest;

    constexpr int32 NumStreams = 8;
    constexpr int32 NumBuffered = 4;
    constexpr int32 FramesPerStream = 2;

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
    // Streaming requests are held open; buffered ones resolve immediately.
    BindCaptureDispatcher(*Server, Capture, EDispatchMode::ResolveBuffered);

    // --- Establish 8 streams, each tagged so frames are attributable. ---
    TArray<TUniquePtr<FSocketTestClient>> StreamClients;
    TArray<FEstablishedStream> Streams;
    for (int32 i = 0; i < NumStreams; ++i)
    {
        TUniquePtr<FSocketTestClient> Client = MakeUnique<FSocketTestClient>();
        if (!TestTrue(FString::Printf(TEXT("stream %d connected"), i), Client->Connect(Port)))
        {
            return true;
        }
        TSharedPtr<FJsonObject> Extra = MakeShared<FJsonObject>();
        Extra->SetStringField(TEXT("tag"), FString::Printf(TEXT("stream-%d"), i));
        const FEstablishedStream Stream = EstablishSseStream(*this, *Client,
            Server.Get(), Capture, 1000 + i, Extra);
        if (!Stream.bOk)
        {
            return true;
        }
        TestTrue(FString::Printf(TEXT("stream %d reported as streaming"), i),
            Server->IsStreamingRequest(Stream.RequestId));
        Streams.Add(Stream);
        StreamClients.Add(MoveTemp(Client));
    }

    // --- Interleave frames: round 0 to every stream, then round 1. ---
    for (int32 Round = 0; Round < FramesPerStream; ++Round)
    {
        for (int32 i = 0; i < NumStreams; ++i)
        {
            TSharedPtr<FJsonObject> Extra = MakeShared<FJsonObject>();
            Extra->SetStringField(TEXT("tag"), FString::Printf(TEXT("stream-%d"), i));
            Extra->SetNumberField(TEXT("seq"), Round);
            TestTrue(FString::Printf(TEXT("frame round %d for stream %d written"), Round, i),
                Server->WriteStreamFrame(Streams[i].RequestId,
                    BuildProgressNotification(Streams[i].ProgressToken, Round + 1.0, Extra)));
        }
        PumpOnce(Server.Get());
    }

    // --- 4 concurrent buffered requests while all 8 streams stay open. ---
    {
        TArray<TUniquePtr<FSocketTestClient>> BufferedClients;
        for (int32 i = 0; i < NumBuffered; ++i)
        {
            TUniquePtr<FSocketTestClient> Client = MakeUnique<FSocketTestClient>();
            if (!TestTrue(FString::Printf(TEXT("buffered %d connected"), i), Client->Connect(Port)))
            {
                return true;
            }
            TestTrue(FString::Printf(TEXT("buffered %d sent"), i), Client->SendString(
                BuildHttpRequest(TEXT("POST"), TEXT("/mcp"),
                    {{TEXT("Content-Type"), TEXT("application/json")}},
                    BuildToolsCallBody(2000 + i, TEXT("test.buffered"),
                        /*bProgressToken=*/false, EWaitArg::Absent)),
                Server.Get()));
            BufferedClients.Add(MoveTemp(Client));
        }
        for (int32 i = 0; i < NumBuffered; ++i)
        {
            FParsedHttpResponse R;
            int32 Next = 0;
            TestTrue(FString::Printf(TEXT("buffered %d answered"), i),
                RecvHttpResponse(*BufferedClients[i], Server.Get(), R, Next));
            TestEqual(FString::Printf(TEXT("buffered %d is 200"), i), R.Code, 200);
            TestTrue(FString::Printf(TEXT("buffered %d is application/json"), i),
                GetHeader(R, TEXT("content-type")).Contains(TEXT("application/json")));
            TestTrue(FString::Printf(TEXT("buffered %d carries the stub result"), i),
                R.Body.Contains(TEXT("\"ok\""), ESearchCase::CaseSensitive));
        }
    }

    // --- FailAllCompletions: every stream gets a terminal error and closes. ---
    Server->FailAllCompletions(TEXT("server shutting down"), TEXT("SHUTDOWN"));

    for (int32 i = 0; i < NumStreams; ++i)
    {
        TestTrue(FString::Printf(TEXT("stream %d closes after FailAllCompletions"), i),
            StreamClients[i]->RecvUntilClosed(Server.Get(), /*TimeoutSeconds=*/10.0));
    }

    // --- Per-stream frame audit on the full byte captures. ---
    for (int32 i = 0; i < NumStreams; ++i)
    {
        const FString Label = FString::Printf(TEXT("stream %d"), i);
        const TArray<FSseEvent> DataEvents =
            DataEventsOnly(ParseSseEvents(StreamClients[i]->Received, Streams[i].Head.BodyStart));

        // 2 progress frames + 1 terminal error frame.
        TestEqual(Label + TEXT(": exactly 3 data frames (2 progress + terminal)"),
            DataEvents.Num(), FramesPerStream + 1);

        const FString OwnTag = FString::Printf(TEXT("\"stream-%d\""), i);
        int32 ProgressSeen = 0;
        for (int32 e = 0; e < DataEvents.Num(); ++e)
        {
            const bool bIsTerminal = (e == DataEvents.Num() - 1);
            if (!bIsTerminal)
            {
                TestTrue(FString::Printf(TEXT("%s: progress frame %d carries its OWN tag"), *Label, e),
                    DataEvents[e].Data.Contains(OwnTag, ESearchCase::CaseSensitive));
                TSharedPtr<FJsonObject> Frame = ParseJsonObject(DataEvents[e].Data);
                if (Frame.IsValid())
                {
                    const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
                    if (Frame->TryGetObjectField(TEXT("params"), ParamsPtr) && ParamsPtr)
                    {
                        TestEqual(FString::Printf(TEXT("%s: frame %d in send order"), *Label, e),
                            static_cast<int32>((*ParamsPtr)->GetNumberField(TEXT("seq"))),
                            ProgressSeen);
                    }
                }
                ++ProgressSeen;
            }
            else
            {
                TestTrue(Label + TEXT(": terminal frame carries the SHUTDOWN error"),
                    DataEvents[e].Data.Contains(TEXT("SHUTDOWN"), ESearchCase::CaseSensitive));
            }
        }

        // No cross-contamination: no other stream's tag ever appears here.
        const FString Raw = StreamClients[i]->ReceivedString();
        for (int32 j = 0; j < NumStreams; ++j)
        {
            if (j == i)
            {
                continue;
            }
            TestFalse(FString::Printf(TEXT("%s: never carries stream %d's frames"), *Label, j),
                Raw.Contains(FString::Printf(TEXT("\"stream-%d\""), j), ESearchCase::CaseSensitive));
        }
    }
    return true;
}
