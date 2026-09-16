// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/System/SessionsTravelDecision.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Tests/TestUtils.h"

// Counterfactual: reverting this fix makes refused and already-pending travel look executed again,
// and removes the retained bounded wait required by the structural assertions below.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionHostLanServerTravelOutcomeContractTest,
    "PinWright.session.host_lan_server.TravelOutcomeContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSessionHostLanServerTravelOutcomeContractTest::RunTest(const FString& Parameters)
{
    const FString RequestedURL = TEXT("/Game/Maps/Requested?listen");

    const PinWrightSessionTravel::FServerTravelOutcome Rejected =
        PinWrightSessionTravel::EvaluateServerTravelOutcome(
            false, {}, {}, RequestedURL, false);
    TestFalse(TEXT("a false ServerTravel result is not accepted"), Rejected.bAccepted);
    TestFalse(TEXT("a false ServerTravel result is not queued"), Rejected.bQueued);
    TestFalse(TEXT("a false ServerTravel result is not complete"), Rejected.bCompleted);
    TestTrue(TEXT("a false ServerTravel result is refused"), Rejected.IsRefused());

    const PinWrightSessionTravel::FServerTravelOutcome AcceptedWithoutQueue =
        PinWrightSessionTravel::EvaluateServerTravelOutcome(
            true, {}, {}, RequestedURL, false);
    TestTrue(TEXT("an accepted call remains distinguishable when no travel queues"),
        AcceptedWithoutQueue.bAccepted);
    TestFalse(TEXT("an accepted call with no pending state is not queued"),
        AcceptedWithoutQueue.bQueued);
    TestTrue(TEXT("an accepted call with no newly queued destination is refused"),
        AcceptedWithoutQueue.IsRefused());

    const FString ExistingURL = TEXT("/Game/Maps/AlreadyPending?listen");
    const PinWrightSessionTravel::FServerTravelPendingState ExistingPendingState = {
        ExistingURL,
        false,
    };
    const PinWrightSessionTravel::FServerTravelOutcome ExistingPending =
        PinWrightSessionTravel::EvaluateServerTravelOutcome(
            true, ExistingPendingState, ExistingPendingState, RequestedURL, false);
    TestTrue(TEXT("the engine call can be accepted while prior travel is pending"),
        ExistingPending.bAccepted);
    TestFalse(TEXT("unchanged prior pending travel is not this request's queue"),
        ExistingPending.bQueued);
    TestFalse(TEXT("unchanged prior pending travel is not complete"),
        ExistingPending.bCompleted);
    TestTrue(TEXT("an accepted call that queued nothing is refused"),
        ExistingPending.IsRefused());

    const PinWrightSessionTravel::FServerTravelPendingState RequestedPendingState = {
        TEXT("/Game/Maps/Requested?listen?MaxPlayers=4"),
        false,
    };
    const PinWrightSessionTravel::FServerTravelOutcome Queued =
        PinWrightSessionTravel::EvaluateServerTravelOutcome(
            true, {}, RequestedPendingState, RequestedURL, false);
    TestTrue(TEXT("the accepted request is recorded"), Queued.bAccepted);
    TestTrue(TEXT("the new requested pending URL is queued"), Queued.bQueued);
    TestFalse(TEXT("queued travel is not complete before destination readback"),
        Queued.bCompleted);
    TestFalse(TEXT("a newly queued request is not refused"), Queued.IsRefused());

    const PinWrightSessionTravel::FServerTravelPendingState SeamlessPendingState = {
        TEXT(""),
        true,
    };
    const PinWrightSessionTravel::FServerTravelOutcome SeamlessQueued =
        PinWrightSessionTravel::EvaluateServerTravelOutcome(
            true, {}, SeamlessPendingState, RequestedURL, false);
    TestTrue(TEXT("a newly started seamless transition is queued"), SeamlessQueued.bQueued);
    TestFalse(TEXT("a seamless transition remains incomplete before readback"),
        SeamlessQueued.bCompleted);

    const FName InitiatingContextHandle(TEXT("TravelContext"));
    const uint32 InitialWorldUniqueID = 41;
    auto EvaluateCompletion = [InitiatingContextHandle, InitialWorldUniqueID](
        const FName ObservedContextHandle,
        const uint32 ObservedWorldUniqueID,
        const FString& ObservedMapPackagePath,
        const bool bTravelPending)
    {
        PinWrightSessionTravel::FServerTravelCompletionObservation Observation;
        Observation.InitiatingContextHandle = InitiatingContextHandle;
        Observation.ObservedContextHandle = ObservedContextHandle;
        Observation.InitialWorldUniqueID = InitialWorldUniqueID;
        Observation.ObservedWorldUniqueID = ObservedWorldUniqueID;
        Observation.RequestedMapPackagePath = TEXT("/Game/Maps/Requested");
        Observation.ObservedMapPackagePath = ObservedMapPackagePath;
        Observation.bWorldAvailable = true;
        Observation.bTravelPending = bTravelPending;
        return PinWrightSessionTravel::EvaluateServerTravelCompletionState(Observation);
    };

    const PinWrightSessionTravel::EServerTravelCompletionState WrongContext =
        EvaluateCompletion(
            FName(TEXT("UnrelatedContext")),
            InitialWorldUniqueID + 1,
            TEXT("/Game/Maps/Requested"),
            false);
    TestTrue(TEXT("an unrelated world context abandons this travel observation"),
        WrongContext == PinWrightSessionTravel::EServerTravelCompletionState::ContextLost);

    const PinWrightSessionTravel::EServerTravelCompletionState UnchangedWorld =
        EvaluateCompletion(
            InitiatingContextHandle,
            InitialWorldUniqueID,
            TEXT("/Game/Maps/Requested"),
            false);
    TestTrue(TEXT("the initiating world itself is not completion"),
        UnchangedWorld == PinWrightSessionTravel::EServerTravelCompletionState::Waiting);

    const PinWrightSessionTravel::EServerTravelCompletionState WrongMap =
        EvaluateCompletion(
            InitiatingContextHandle,
            InitialWorldUniqueID + 1,
            TEXT("/Game/Maps/Other"),
            false);
    TestTrue(TEXT("a replacement world on the wrong map remains incomplete"),
        WrongMap == PinWrightSessionTravel::EServerTravelCompletionState::Waiting);

    const PinWrightSessionTravel::EServerTravelCompletionState PendingTravel =
        EvaluateCompletion(
            InitiatingContextHandle,
            InitialWorldUniqueID + 1,
            TEXT("/Game/Maps/Requested"),
            true);
    TestTrue(TEXT("the requested map is not complete while travel remains pending"),
        PendingTravel == PinWrightSessionTravel::EServerTravelCompletionState::Waiting);

    const PinWrightSessionTravel::EServerTravelCompletionState CorrectCompletion =
        EvaluateCompletion(
            InitiatingContextHandle,
            InitialWorldUniqueID + 1,
            TEXT("/Game/Maps/Requested"),
            false);
    TestTrue(TEXT("the replacement map in the initiating context completes travel"),
        CorrectCompletion == PinWrightSessionTravel::EServerTravelCompletionState::Completed);

    const PinWrightSessionTravel::FServerTravelOutcome Completed =
        PinWrightSessionTravel::EvaluateServerTravelOutcome(
            true,
            {},
            RequestedPendingState,
            RequestedURL,
            CorrectCompletion ==
                PinWrightSessionTravel::EServerTravelCompletionState::Completed);
    TestTrue(TEXT("production destination readback completes the queued request"),
        Completed.bCompleted);

    int32 AbandonFirstCallbackCount = 0;
    PinWrightSessionTravel::FServerTravelTerminalResponseGate AbandonFirstGate;
    TestTrue(TEXT("abandonment claims the terminal response"),
        AbandonFirstGate.TryRespond([&AbandonFirstCallbackCount]()
        {
            ++AbandonFirstCallbackCount;
        }));
    TestFalse(TEXT("wait completion cannot respond after abandonment"),
        AbandonFirstGate.TryRespond([&AbandonFirstCallbackCount]()
        {
            ++AbandonFirstCallbackCount;
        }));
    TestEqual(TEXT("abandonment and completion invoke one response callback"),
        AbandonFirstCallbackCount, 1);

    int32 CompletionFirstCallbackCount = 0;
    PinWrightSessionTravel::FServerTravelTerminalResponseGate CompletionFirstGate;
    TestTrue(TEXT("wait completion claims the terminal response"),
        CompletionFirstGate.TryRespond([&CompletionFirstCallbackCount]()
        {
            ++CompletionFirstCallbackCount;
        }));
    TestFalse(TEXT("later abandonment cannot send a second response"),
        CompletionFirstGate.TryRespond([&CompletionFirstCallbackCount]()
        {
            ++CompletionFirstCallbackCount;
        }));
    TestEqual(TEXT("completion and abandonment invoke one response callback"),
        CompletionFirstCallbackCount, 1);

    TestEqual(TEXT("travel refusal uses the registered typed code"),
        FString(ErrorCodes::ERR_TRAVEL_REFUSED), FString(TEXT("TRAVEL_REFUSED")));

    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
    if (!TestTrue(TEXT("PinWright plugin resolves for the source contract"), Plugin.IsValid()))
        return false;

    FString HandlerSource;
    const FString HandlerPath = Plugin->GetBaseDir()
        / TEXT("Source/PinWright/Private/Handlers/System/SessionsHandler.cpp");
    if (!TestTrue(TEXT("SessionsHandler.cpp is readable"),
            FFileHelper::LoadFileToString(HandlerSource, *HandlerPath)))
    {
        return false;
    }

    const int32 HostStart = HandlerSource.Find(TEXT("// ---- session.host_lan_server ----"));
    const int32 HostEnd = HostStart == INDEX_NONE
        ? INDEX_NONE
        : HandlerSource.Find(
            TEXT("// ---- session.get_sessions_info ----"),
            ESearchCase::CaseSensitive,
            ESearchDir::FromStart,
            HostStart);
    if (!TestTrue(TEXT("host_lan_server source block is bounded"),
            HostStart != INDEX_NONE && HostEnd > HostStart))
    {
        return false;
    }

    const FString HostBlock = NeutralizeSourceText(
        HandlerSource.Mid(HostStart, HostEnd - HostStart));
    const FString NeutralizedHandlerSource = NeutralizeSourceText(HandlerSource);
    TestTrue(TEXT("host travel retains its asynchronous request lifetime"),
        HostBlock.Contains(TEXT("RetainAsyncRequestLifetime")));
    TestTrue(TEXT("host travel reuses the shared bounded PIE lifecycle waiter"),
        HostBlock.Contains(
            TEXT("WaitState->WaitControl = PinWrightPieState::WaitForPieLifecycleState")));
    TestTrue(TEXT("the bounded wait invokes the travel completion probe"),
        HostBlock.Contains(TEXT("CaptureServerTravelCompletionState")));
    TestTrue(TEXT("host travel captures its context through the engine lookup"),
        HostBlock.Contains(TEXT("GEngine->GetWorldContextFromWorld(World)")));
    TestTrue(TEXT("host travel polls its context through the engine lookup"),
        NeutralizedHandlerSource.Contains(
            TEXT("GEngine->GetWorldContextFromHandle(InitiatingWorldContextHandle)")));
    TestFalse(TEXT("host travel does not duplicate engine world-context scans"),
        NeutralizedHandlerSource.Contains(TEXT("GetWorldContexts()")));
    TestTrue(TEXT("host travel normalizes the requested map before starting the waiter"),
        HostBlock.Contains(TEXT("const FString RequestedMapPackagePath")));
    TestTrue(TEXT("context disappearance cancels the bounded wait"),
        HostBlock.Contains(TEXT("EServerTravelCompletionState::ContextLost")));
    TestTrue(TEXT("accepted but unqueued travel uses a cause-neutral refusal"),
        HostBlock.Contains(
            TEXT("did not newly queue the requested destination")));
    TestFalse(TEXT("queued status is not written into an unsent response"),
        HostBlock.Contains(
            TEXT("SetStringField(TEXT(\"status\"), TEXT(\"server travel queued\"))")));
    TestFalse(TEXT("host travel does not create a private FTSTicker"),
        HostBlock.Contains(TEXT("FTSTicker")) || HostBlock.Contains(TEXT("AddTicker")));
    TestTrue(TEXT("legacy execution truth is derived only from completed readback"),
        NeutralizedHandlerSource.Contains(
            TEXT("SetBoolField(TEXT(\"travelExecuted\"), Outcome.bCompleted)")));
    return true;
}
