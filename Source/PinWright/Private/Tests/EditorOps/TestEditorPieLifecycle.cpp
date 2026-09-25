// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "Containers/Ticker.h"
#include "CoreGlobals.h"
#include "Dispatch/RpcDispatcher.h"
#include "Dispatch/ScopedUnattendedRpc.h"
#include "Editor.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerRegistration.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"
#include "Utils/PieState.h"

namespace EditorPieLifecycleTests
{
    struct FWaitFixtureState
    {
        bool bPieActive = false;
        bool bSessionInProgress = false;
        bool bCancelled = false;
        int32 CompletionCount = 0;
        PinWrightPieState::FPieLifecycleWaitResult Outcome;
    };

    FString ReadHandlerSource(FAutomationTestBase& Test, const TCHAR* RelativePath)
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        Test.TestTrue(TEXT("PinWright plugin is discoverable"), Plugin.IsValid());
        if (!Plugin.IsValid())
        {
            return FString();
        }

        FString Source;
        const FString Path = Plugin->GetBaseDir() / RelativePath;
        Test.TestTrue(*FString::Printf(TEXT("Handler source loads: %s"), RelativePath),
            FFileHelper::LoadFileToString(Source, *Path));
        // The markers below span line breaks. A Windows checkout with core.autocrlf on stores the
        // same sources with CRLF, so normalise to LF and keep the patterns line-ending agnostic.
        Source.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
        return Source;
    }

    FString HandlerBlock(const FString& Source, const TCHAR* StartMarker, const TCHAR* EndMarker)
    {
        const int32 Start = Source.Find(StartMarker);
        const int32 End = Source.Find(
            EndMarker, ESearchCase::CaseSensitive, ESearchDir::FromStart, Start + 1);
        if (Start == INDEX_NONE || End <= Start)
        {
            return FString();
        }

        const FString Block = Source.Mid(Start, End - Start);
        const int32 BodyStart = Block.Find(TEXT("\n{"));
        return BodyStart != INDEX_NONE
            ? NeutralizeSourceText(Block.Mid(BodyStart + 1))
            : FString();
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorPieLifecycleWaitStateTransitionsTest,
    "PinWright.editor.pie.LifecycleWaitStateTransitions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorPieLifecycleWaitStateTransitionsTest::RunTest(const FString& Parameters)
{
    using namespace EditorPieLifecycleTests;

    const TSharedRef<FWaitFixtureState> State = MakeShared<FWaitFixtureState>();
    const auto Complete = [State](const PinWrightPieState::FPieLifecycleWaitResult& Result)
    {
        ++State->CompletionCount;
        State->Outcome = Result;
    };
    const auto Probe = [State]()
    {
        return PinWrightPieState::FPieLifecycleState{
            State->bPieActive,
            State->bSessionInProgress,
        };
    };

    State->bSessionInProgress = true;
    PinWrightPieState::WaitForPieLifecycleState(
        PinWrightPieState::EPieLifecycleWaitTarget::PlayWorldActive,
        1.0, Complete, Probe);
    TestEqual(TEXT("play wait does not complete while the fake PIE world is absent"),
        State->CompletionCount, 0);

    State->bPieActive = true;
    for (int32 Tick = 0; Tick < 3 && State->CompletionCount == 0; ++Tick)
    {
        FTSTicker::GetCoreTicker().Tick(0.1f);
    }
    TestEqual(TEXT("play wait completes once after the fake PIE world appears"),
        State->CompletionCount, 1);
    TestTrue(TEXT("play completion reports pieActive"), State->Outcome.bPieActive);
    TestTrue(TEXT("play completion reports the authoritative session state"),
        State->Outcome.bSessionInProgress);
    TestFalse(TEXT("play completion is not a timeout"), State->Outcome.bTimedOut);

    State->bPieActive = true;
    State->bSessionInProgress = true;
    State->CompletionCount = 0;
    State->Outcome = {};
    PinWrightPieState::WaitForPieLifecycleState(
        PinWrightPieState::EPieLifecycleWaitTarget::SessionInactive,
        1.0, Complete, Probe);
    TestEqual(TEXT("stop wait does not complete while the fake session is active"),
        State->CompletionCount, 0);

    State->bPieActive = false;
    FTSTicker::GetCoreTicker().Tick(0.1f);
    TestEqual(TEXT("stop wait remains pending while session info survives PlayWorld"),
        State->CompletionCount, 0);

    State->bSessionInProgress = false;
    for (int32 Tick = 0; Tick < 3 && State->CompletionCount == 0; ++Tick)
    {
        FTSTicker::GetCoreTicker().Tick(0.1f);
    }
    TestEqual(TEXT("stop wait completes once after the authoritative session ends"),
        State->CompletionCount, 1);
    TestFalse(TEXT("stop completion reports pieActive false"), State->Outcome.bPieActive);
    TestFalse(TEXT("stop completion reports sessionInProgress false"),
        State->Outcome.bSessionInProgress);
    TestFalse(TEXT("stop completion is not a timeout"), State->Outcome.bTimedOut);

    State->bPieActive = false;
    State->bSessionInProgress = true;
    State->CompletionCount = 0;
    State->Outcome = {};
    PinWrightPieState::WaitForPieLifecycleState(
        PinWrightPieState::EPieLifecycleWaitTarget::PlayWorldActive,
        0.0, Complete, Probe);
    TestEqual(TEXT("expired wait completes immediately"), State->CompletionCount, 1);
    TestFalse(TEXT("expired play wait reports the measured inactive world"),
        State->Outcome.bPieActive);
    TestTrue(TEXT("expired play wait reports the measured in-progress session"),
        State->Outcome.bSessionInProgress);
    TestTrue(TEXT("expired play wait reports timedOut"), State->Outcome.bTimedOut);

    State->bPieActive = false;
    State->bSessionInProgress = true;
    State->bCancelled = false;
    State->CompletionCount = 0;
    State->Outcome = {};
    PinWrightPieState::WaitForPieLifecycleState(
        PinWrightPieState::EPieLifecycleWaitTarget::PlayWorldActive,
        1.0,
        Complete,
        Probe,
        [State]() { return State->bCancelled; });
    TestEqual(TEXT("owned play wait starts pending"), State->CompletionCount, 0);

    // Cancellation and a later world's arrival can become visible on the same tick. The old
    // generation must answer cancellation before it considers the reached PlayWorld target.
    State->bCancelled = true;
    State->bPieActive = true;
    FTSTicker::GetCoreTicker().Tick(0.1f);
    TestEqual(TEXT("cancelled stale play wait completes exactly once"),
        State->CompletionCount, 1);
    TestTrue(TEXT("stale play wait reports cancellation"), State->Outcome.bCancelled);
    TestFalse(TEXT("stale play wait cannot claim the later world as success"),
        State->Outcome.bTimedOut);
    FTSTicker::GetCoreTicker().Tick(0.1f);
    TestEqual(TEXT("cancelled stale waiter removes its ticker"), State->CompletionCount, 1);
    return true;
}

// Counterfactual: reverting either the editor.play dispatcher-lifetime lease or
// the wait-control cancellation leaves this request unanswered and/or its ticker
// active after Dispatcher.Reset(), so the response-count, typed-code, or active-
// ticker assertion below fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorPiePlayWaitAbandonsWhenDispatcherEndsTest,
    "PinWright.editor.pie.PlayWaitAbandonsWhenDispatcherEnds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorPiePlayWaitAbandonsWhenDispatcherEndsTest::RunTest(
    const FString& Parameters)
{
    if (!GEditor || GEditor->PlayWorld || GEditor->IsPlaySessionInProgress())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("editor-state-unavailable"),
            TEXT("Skipped: this test needs an editor with no active or pending PIE session."));
        return true;
    }

    FRpcHandlerFunc PlayHandler = nullptr;
    for (const FHandlerRegistration& Registration :
         FAutoRegisterHandler::GetPendingRegistrations())
    {
        if (Registration.MethodName == TEXT("editor.play"))
        {
            PlayHandler = Registration.Func;
            break;
        }
    }
    TestTrue(TEXT("production editor.play handler is registered"), PlayHandler != nullptr);
    if (!PlayHandler)
    {
        return false;
    }

    ON_SCOPE_EXIT
    {
        CancelAnyQueuedPlaySession();
    };

    const TSharedRef<FTestResponseCapture> Capture =
        MakeShared<FTestResponseCapture>();
    const TSharedRef<int32> SerializationProbeCalls = MakeShared<int32>(0);
    TUniquePtr<FRpcDispatcher> Dispatcher = MakeUnique<FRpcDispatcher>();
    FRpcDispatcher* DispatcherForContext = Dispatcher.Get();

    Dispatcher->RegisterHandler(TEXT("editor.play"),
        [PlayHandler, Capture, DispatcherForContext](
            const FString& RequestId,
            const FString& Method,
            const TSharedPtr<FJsonObject>& Payload) -> bool
        {
            FHandlerContext Ctx = FHandlerContext::MakeTestContextWithSharedCapture(
                RequestId, Method, Payload, Capture);
            Ctx.SetDispatcherForTesting(DispatcherForContext);
            return PlayHandler(Ctx);
        });
    Dispatcher->RegisterHandler(TEXT("test.serialization_probe"),
        [SerializationProbeCalls](const FString&, const FString&,
            const TSharedPtr<FJsonObject>&) -> bool
        {
            ++SerializationProbeCalls.Get();
            return true;
        });

    Dispatcher->ProcessRequest(
        TEXT("play-abandon-id"), TEXT("editor.play"), MakeShared<FJsonObject>());
    CancelAnyQueuedPlaySession();
    const TSharedPtr<PinWrightPieState::FPieLifecycleWaitControl> WaitControl =
        PinWrightPieState::GetLastPieLifecycleWaitControlForTesting();

    TestEqual(TEXT("editor.play has not responded while PlayWorld is absent"),
        Capture->CallCount, 0);
    TestTrue(TEXT("editor.play owns a valid lifecycle wait ticker"),
        WaitControl.IsValid() && WaitControl->IsTickerValid());
    TestFalse(TEXT("the dispatcher request guard is released while editor.play waits"),
        Dispatcher->IsProcessingRequestForTesting());

    Dispatcher->ProcessRequest(TEXT("serialization-probe-id"),
        TEXT("test.serialization_probe"), MakeShared<FJsonObject>());
    TestEqual(TEXT("a second request dispatches while editor.play waits"),
        SerializationProbeCalls.Get(), 1);

    Dispatcher.Reset();

    TestEqual(TEXT("dispatcher teardown produces exactly one terminal response"),
        Capture->CallCount, 1);
    TestFalse(TEXT("dispatcher teardown cannot report PIE start success"),
        Capture->bSuccess);
    TestEqual(TEXT("dispatcher teardown uses the typed PIE start failure"),
        Capture->ErrorCode, FString(ErrorCodes::ERR_PIE_START_FAILED));
    TestTrue(TEXT("dispatcher teardown invalidates the lifecycle wait ticker"),
        WaitControl.IsValid() && !WaitControl->IsTickerValid());

    bool bDispatcherEnded = false;
    TestTrue(TEXT("dispatcher teardown response identifies abandonment"),
        Capture->Result.IsValid() &&
        Capture->Result->TryGetBoolField(TEXT("dispatcherEnded"), bDispatcherEnded) &&
        bDispatcherEnded);

    const int32 ResponseCountAfterTeardown = Capture->CallCount;
    FTSTicker::GetCoreTicker().Tick(0.1f);
    TestEqual(TEXT("a later ticker pass cannot overwrite the teardown failure"),
        Capture->CallCount, ResponseCountAfterTeardown);
    TestTrue(TEXT("the abandoned wait ticker stays invalid after a ticker pass"),
        WaitControl.IsValid() && !WaitControl->IsTickerValid());
    return true;
}

// B-compile-error-bp-wedges-next-play. The engine consumes editor.play's queued request on a
// later editor tick, outside the dispatcher's per-handler unattended scope, and that tick raises
// the "Blueprint Asset Compilation Error" modal for any Blueprint in BS_Error. editor.play must
// keep the automation-mode interval open across the deferred start and close it when the wait
// ends. Counterfactual: without the interval, IsActive() is false once the handler returns and
// the first assertion fails (a visible editor then wedges on the modal).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorPiePlayHoldsModalSuppressionAcrossDeferredStartTest,
    "PinWright.editor.pie.PlayHoldsModalSuppressionAcrossDeferredStart",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorPiePlayHoldsModalSuppressionAcrossDeferredStartTest::RunTest(
    const FString& Parameters)
{
    if (!GEditor || GEditor->PlayWorld || GEditor->IsPlaySessionInProgress() ||
        PinWrightAutomationMode::IsActive() ||
        !GetDefault<UPinWrightSettings>()->bSuppressModalDialogsDuringRpc)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("editor-state-unavailable"),
            TEXT("Skipped: needs no PIE session, no open automation-mode interval, and modal suppression enabled."));
        return true;
    }

    FRpcHandlerFunc PlayHandler = nullptr;
    for (const FHandlerRegistration& Registration :
         FAutoRegisterHandler::GetPendingRegistrations())
    {
        if (Registration.MethodName == TEXT("editor.play"))
        {
            PlayHandler = Registration.Func;
            break;
        }
    }
    TestTrue(TEXT("production editor.play handler is registered"), PlayHandler != nullptr);
    if (!PlayHandler)
    {
        return false;
    }

    ON_SCOPE_EXIT
    {
        CancelAnyQueuedPlaySession();
        PinWrightAutomationMode::ResetForTests();
    };

    const TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    TUniquePtr<FRpcDispatcher> Dispatcher = MakeUnique<FRpcDispatcher>();
    FRpcDispatcher* DispatcherForContext = Dispatcher.Get();
    Dispatcher->RegisterHandler(TEXT("editor.play"),
        [PlayHandler, Capture, DispatcherForContext](
            const FString& RequestId,
            const FString& Method,
            const TSharedPtr<FJsonObject>& Payload) -> bool
        {
            FHandlerContext Ctx = FHandlerContext::MakeTestContextWithSharedCapture(
                RequestId, Method, Payload, Capture);
            Ctx.SetDispatcherForTesting(DispatcherForContext);
            return PlayHandler(Ctx);
        });

    Dispatcher->ProcessRequest(
        TEXT("play-modal-scope-id"), TEXT("editor.play"), MakeShared<FJsonObject>());

    TestEqual(TEXT("editor.play is still waiting for PlayWorld"), Capture->CallCount, 0);
    TestFalse(TEXT("the dispatcher's own handler scope has closed"),
        Dispatcher->IsProcessingRequestForTesting());
    TestTrue(TEXT("modal suppression stays open while the engine has the start request queued"),
        PinWrightAutomationMode::IsActive());
    TestTrue(TEXT("GIsRunningUnattendedScript is set for the tick that consumes the request"),
        GIsRunningUnattendedScript);

    CancelAnyQueuedPlaySession();
    Dispatcher.Reset();

    TestEqual(TEXT("dispatcher teardown answers the play call"), Capture->CallCount, 1);
    TestFalse(TEXT("modal suppression closes when the play wait ends"),
        PinWrightAutomationMode::IsActive());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorPieLifecycleDecisionSeamsTest,
    "PinWright.editor.pie.LifecycleDecisionSeams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorPieLifecycleDecisionSeamsTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightPieState;

    TestTrue(TEXT("unreached lifecycle state remains pending"),
        EvaluatePieLifecycleWait(EPieLifecycleWaitTarget::PlayWorldActive,
            {false, true, true}, false, false) == EPieLifecycleWaitStatus::Pending);
    TestTrue(TEXT("PlayWorld reaches the play target"),
        EvaluatePieLifecycleWait(EPieLifecycleWaitTarget::PlayWorldActive,
            {true, true, false}, false, false) == EPieLifecycleWaitStatus::Reached);
    TestTrue(TEXT("deadline reaches timeout when the target is absent"),
        EvaluatePieLifecycleWait(EPieLifecycleWaitTarget::PlayWorldActive,
            {false, true, false}, true, false) == EPieLifecycleWaitStatus::TimedOut);
    TestTrue(TEXT("cancellation wins over a later PlayWorld"),
        EvaluatePieLifecycleWait(EPieLifecycleWaitTarget::PlayWorldActive,
            {true, true, false}, false, true) == EPieLifecycleWaitStatus::Cancelled);
    TestTrue(TEXT("session-inactive wait also requires PlayWorld to be gone"),
        EvaluatePieLifecycleWait(EPieLifecycleWaitTarget::SessionInactive,
            {true, false, false}, false, false) == EPieLifecycleWaitStatus::Pending);

    TestTrue(TEXT("an active PlayWorld selects end play even if a start is also queued"),
        EvaluatePieStopAction({true, true, true}) == EPieStopAction::RequestEndPlay);
    TestTrue(TEXT("session info without PlayWorld waits for a safe transition"),
        EvaluatePieStopAction({false, true, false}) == EPieStopAction::WaitForTransition);
    TestTrue(TEXT("a queued start without PlayWorld selects queued-request cancellation"),
        EvaluatePieStopAction({false, true, true}) == EPieStopAction::CancelQueuedStartRequest);
    TestTrue(TEXT("an inactive session needs no stop action"),
        EvaluatePieStopAction({false, false, false}) == EPieStopAction::None);

    FPieLifecycleOperationOwner Owner;
    const TSharedPtr<FPieLifecycleOperation> First =
        Owner.TryBegin(EPieLifecycleOperationType::Play);
    TestTrue(TEXT("first lifecycle operation takes ownership"), First.IsValid());
    TestFalse(TEXT("overlapping lifecycle operation is rejected"),
        Owner.TryBegin(EPieLifecycleOperationType::Stop).IsValid());
    if (!First.IsValid())
    {
        return false;
    }

    Owner.CancelActiveOperation();
    TestTrue(TEXT("cancelled generation is marked"), First->bCancelled);
    TestFalse(TEXT("cancelled generation no longer owns lifecycle state"),
        Owner.IsCurrent(First.ToSharedRef()));
    const TSharedPtr<FPieLifecycleOperation> Second =
        Owner.TryBegin(EPieLifecycleOperationType::Stop);
    TestTrue(TEXT("replacement lifecycle operation takes ownership"), Second.IsValid());
    if (!Second.IsValid())
    {
        return false;
    }
    TestTrue(TEXT("replacement receives a newer generation"),
        Second->Generation > First->Generation);
    TestTrue(TEXT("replacement generation is current"), Owner.IsCurrent(Second.ToSharedRef()));
    Owner.Complete(First.ToSharedRef());
    TestTrue(TEXT("a stale generation cannot clear its replacement"),
        Owner.IsCurrent(Second.ToSharedRef()));
    Second->Type = EPieLifecycleOperationType::Cleanup;
    TestFalse(TEXT("a cleanup generation retains exclusive lifecycle ownership"),
        Owner.TryBegin(EPieLifecycleOperationType::Play).IsValid());
    Owner.Complete(Second.ToSharedRef());
    TestFalse(TEXT("completed lifecycle operation releases ownership"),
        Owner.HasActiveOperation());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorPieLifecycleHandlersUseBoundedWaitTest,
    "PinWright.editor.pie.LifecycleHandlersUseBoundedWait",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorPieLifecycleHandlersUseBoundedWaitTest::RunTest(const FString& Parameters)
{
    using namespace EditorPieLifecycleTests;

    const FString Source = ReadHandlerSource(*this,
        TEXT("Source/PinWright/Private/Handlers/Editor/PIEHandler.cpp"));
    const FString Play = HandlerBlock(
        Source, TEXT("// ---- editor.play ----"), TEXT("// ---- editor.stop ----"));
    const FString Stop = HandlerBlock(
        Source, TEXT("// ---- editor.stop ----"), TEXT("// ---- editor.pause ----"));
    const FString CleanupProbe = HandlerBlock(
        Source, TEXT("PinWrightPieState::FPieLifecycleState CaptureAndApplyPieStopState"),
        TEXT("void StartPieTimeoutCleanupWatcher"));
    const FString CleanupWatcher = HandlerBlock(
        Source, TEXT("void StartPieTimeoutCleanupWatcher"),
        TEXT("// ---- editor.play ----"));
    const FString SubsystemSource = ReadHandlerSource(*this,
        TEXT("Source/PinWright/Private/PinWrightSubsystem.cpp"));
    const FString TransportSource = ReadHandlerSource(*this,
        TEXT("Source/PinWright/Private/Transport/SocketHttpServer.cpp"));
    const FString AcceptLoop = HandlerBlock(TransportSource,
        TEXT("void FSocketHttpServer::AcceptPendingConnections"),
        TEXT("bool FSocketHttpServer::PumpConnectionRead"));
    const FString ReadLoop = HandlerBlock(TransportSource,
        TEXT("bool FSocketHttpServer::PumpConnectionRead"),
        TEXT("void FSocketHttpServer::PumpParse"));
    const FString DispatcherSource = ReadHandlerSource(*this,
        TEXT("Source/PinWright/Private/Dispatch/RpcDispatcher.cpp"));
    TestFalse(TEXT("editor.play source block is bounded"), Play.IsEmpty());
    TestFalse(TEXT("editor.stop source block is bounded"), Stop.IsEmpty());
    TestFalse(TEXT("timeout cleanup state probe source block is bounded"), CleanupProbe.IsEmpty());
    TestFalse(TEXT("timeout cleanup watcher source block is bounded"), CleanupWatcher.IsEmpty());
    TestFalse(TEXT("transport accept loop source block is bounded"), AcceptLoop.IsEmpty());
    TestFalse(TEXT("transport read loop source block is bounded"), ReadLoop.IsEmpty());
    if (Play.IsEmpty() || Stop.IsEmpty() || CleanupProbe.IsEmpty() || CleanupWatcher.IsEmpty()
        || AcceptLoop.IsEmpty() || ReadLoop.IsEmpty())
    {
        return false;
    }

    const int32 PlayOwnership = Play.Find(TEXT("TryBegin"));
    const int32 PlayRequest = Play.Find(TEXT("RequestPlayInEditorSession"));
    const int32 PlayWait = Play.Find(TEXT("WaitForPieLifecycleState"));
    const int32 PlaySyncSuccess = Play.Find(TEXT("Ctx.SendSuccess"),
        ESearchCase::CaseSensitive, ESearchDir::FromStart, PlayRequest);
    const int32 PlayAsyncToken = Play.Find(TEXT("MakeAsyncToken"));
    const int32 PlayInProgressError = Play.Find(TEXT("PIE_START_IN_PROGRESS"));
    TestTrue(TEXT("editor.play rejects overlapping owned or engine lifecycle state"),
        Play.Contains(TEXT("GetActiveOperation")) &&
        Play.Contains(TEXT("IsPlaySessionInProgress")) &&
        PlayInProgressError != INDEX_NONE && PlayInProgressError < PlayRequest);
    TestTrue(TEXT("editor.play owns a generation before issuing the deferred request"),
        PlayOwnership != INDEX_NONE && PlayOwnership < PlayRequest);
    TestTrue(TEXT("editor.play waits after issuing the deferred start request"),
        PlayRequest != INDEX_NONE && PlayWait > PlayRequest);
    TestTrue(TEXT("editor.play handles an immediate world through the live context before making an async token"),
        PlaySyncSuccess > PlayRequest && PlaySyncSuccess < PlayAsyncToken &&
        PlayAsyncToken < PlayWait);
    TestTrue(TEXT("editor.play waits specifically for PlayWorld and reports measured lifecycle state"),
        Play.Contains(TEXT("EPieLifecycleWaitTarget::PlayWorldActive")) &&
        Play.Contains(TEXT("Outcome.bPieActive")) &&
        Play.Contains(TEXT("Outcome.bSessionInProgress")) &&
        Play.Contains(TEXT("Outcome.bTimedOut")));
    TestTrue(TEXT("editor.play cancellation wins before a stale waiter can report success"),
        Play.Contains(TEXT("Outcome.bCancelled")) &&
        Play.Contains(TEXT("PIE_START_CANCELLED")) &&
        Play.Contains(TEXT("IsCurrent(Operation)")) &&
        Play.Contains(TEXT("editor.stop invalidated this PIE start request generation")));
    const int32 PlayTimeoutState = Play.Find(TEXT("TimeoutState"));
    const int32 PlayTimeoutCleanup = Play.Find(TEXT("ApplyPieStopAction(TimeoutState"));
    TestTrue(TEXT("editor.play timeout snapshots state before applying cleanup"),
        PlayTimeoutState != INDEX_NONE && PlayTimeoutCleanup > PlayTimeoutState &&
        Play.Contains(TEXT("sessionInProgressAtTimeout")));
    TestTrue(TEXT("editor.play timeout retains a bounded cleanup generation for residual session state"),
        Play.Contains(TEXT("StartPieTimeoutCleanupWatcher")) &&
        CleanupWatcher.Contains(TEXT("EPieLifecycleOperationType::Cleanup")) &&
        CleanupWatcher.Contains(TEXT("EPieLifecycleWaitTarget::SessionInactive")) &&
        CleanupWatcher.Contains(TEXT("GPieLifecycleOperationOwner.Complete(Operation)")) &&
        CleanupWatcher.Contains(TEXT("ReleaseDispatcherLifetime(LifetimeLease)")));
    TestTrue(TEXT("editor.play owns its wait without retaining dispatcher serialization"),
        Play.Contains(TEXT("RetainAsyncRequestLifetime")) &&
        Play.Contains(TEXT("WaitState->WaitControl =")));
    const int32 CleanupGenerationGuard = CleanupProbe.Find(TEXT("IsCurrent(Operation)"));
    const int32 CleanupEngineAction = CleanupProbe.Find(TEXT("ApplyPieStopAction"));
    TestTrue(TEXT("a stale cleanup generation cannot apply an action to a later session"),
        CleanupGenerationGuard != INDEX_NONE && CleanupEngineAction > CleanupGenerationGuard &&
        CleanupWatcher.Contains(TEXT("!GPieLifecycleOperationOwner.IsCurrent(Operation)")));

    const int32 StopInitialState = Stop.Find(TEXT("InitialState"));
    const int32 StopAction = Stop.Find(TEXT("CaptureAndApplyPieStopState(Operation"));
    const int32 StopWait = Stop.Find(TEXT("WaitForPieLifecycleState"));
    const int32 StopStateAfterRequest = Stop.Find(TEXT("StateAfterRequest"));
    const int32 StopSyncSuccess = Stop.Find(TEXT("Ctx.SendSuccess"),
        ESearchCase::CaseSensitive, ESearchDir::FromStart, StopStateAfterRequest);
    const int32 StopAsyncToken = Stop.Find(TEXT("MakeAsyncToken"));
    TestTrue(TEXT("editor.stop snapshots PlayWorld before applying any engine stop action"),
        StopInitialState != INDEX_NONE && StopAction > StopInitialState);
    TestTrue(TEXT("editor.stop invalidates an owned play waiter before replacing it"),
        Stop.Contains(TEXT("CancelActiveOperation")) &&
        Stop.Contains(TEXT("cancelledPlayWaiter")));
    TestTrue(TEXT("editor.stop rejects a redundant overlapping stop waiter"),
        Stop.Contains(TEXT("PIE_STOP_IN_PROGRESS")) &&
        Stop.Contains(TEXT("EPieLifecycleOperationType::Play")));
    TestTrue(TEXT("editor.stop waits after applying the selected stop action"),
        StopAction != INDEX_NONE && StopWait > StopAction);
    TestTrue(TEXT("editor.stop uses Unreal's authoritative session predicate"),
        Stop.Contains(TEXT("CaptureEditorPieLifecycleState")) &&
        Stop.Contains(TEXT("EPieLifecycleWaitTarget::SessionInactive")));
    TestTrue(TEXT("editor.stop handles synchronous cancellation through the live context before making an async token"),
        StopSyncSuccess > StopStateAfterRequest && StopSyncSuccess < StopAsyncToken &&
        StopAsyncToken < StopWait);
    TestTrue(TEXT("editor.stop owns its wait without retaining dispatcher serialization"),
        Stop.Contains(TEXT("RetainAsyncRequestLifetime")) &&
        Stop.Contains(TEXT("WaitState->WaitControl =")));
    TestTrue(TEXT("editor.stop probe applies the state-selected action when a late PlayWorld appears"),
        Stop.Contains(TEXT("CaptureStopState")) &&
        Stop.Contains(TEXT("CaptureAndApplyPieStopState(Operation")) &&
        CleanupProbe.Contains(TEXT("ApplyPieStopAction")));
    const int32 StopTimeoutState = Stop.Find(TEXT("TimeoutState"));
    const int32 StopTimeoutCleanup = Stop.Find(TEXT("CaptureAndApplyPieStopState(Operation"),
        ESearchCase::CaseSensitive, ESearchDir::FromStart, StopTimeoutState);
    TestTrue(TEXT("editor.stop timeout snapshots session-info-only state before safe cleanup"),
        StopTimeoutState != INDEX_NONE && StopTimeoutCleanup > StopTimeoutState &&
        Stop.Contains(TEXT("startRequestQueuedAtTimeout")));
    const int32 QueuedCancellationRecheck =
        Source.Find(TEXT("if (GEditor->IsPlaySessionRequestQueued())"));
    const int32 QueuedCancellationCall = Source.Find(TEXT("GEditor->CancelRequestPlaySession()"),
        ESearchCase::CaseSensitive, ESearchDir::FromStart, QueuedCancellationRecheck);
    TestTrue(TEXT("stop action helper cancels only after a live queued-state recheck"),
        Source.Contains(TEXT("EPieStopAction::CancelQueuedStartRequest")) &&
        Source.Contains(TEXT("EPieStopAction::WaitForTransition")) &&
        QueuedCancellationRecheck != INDEX_NONE &&
        QueuedCancellationCall > QueuedCancellationRecheck);
    TestTrue(TEXT("endPlayRequested directly reports Unreal's current end-request state"),
        Source.Contains(TEXT("EPieStopAction::RequestEndPlay")) &&
        Source.Contains(TEXT("GEditor->RequestEndPlayMap()")) &&
        Source.Contains(TEXT("const bool bEndPlayRequested = GEditor && GEditor->ShouldEndPlayMap()")) &&
        Source.Contains(TEXT("SetBoolField(TEXT(\"endPlayRequested\"), bEndPlayRequested)")));
    TestTrue(TEXT("responses distinguish safe queued cancellation from a pending engine transition"),
        Source.Contains(TEXT("startRequestCancelled")) &&
        Source.Contains(TEXT("startRequestWasQueued")) &&
        Source.Contains(TEXT("transitionCleanupPending")));
    TestTrue(TEXT("editor.stop reports pieActive, sessionInProgress, and timedOut"),
        Stop.Contains(TEXT("Outcome.bPieActive")) &&
        Stop.Contains(TEXT("Outcome.bSessionInProgress")) &&
        Stop.Contains(TEXT("Outcome.bTimedOut")));

    const int32 ShutdownQuiesce = SubsystemSource.Find(
        TEXT("StreamingTransport->QuiesceRequestIntake()"));
    const int32 ShutdownDispatcher = SubsystemSource.Find(TEXT("Dispatcher.Reset()"),
        ESearchCase::CaseSensitive, ESearchDir::FromStart, ShutdownQuiesce);
    const int32 ShutdownFailPending = SubsystemSource.Find(
        TEXT("StreamingTransport->FailAllCompletions"), ESearchCase::CaseSensitive,
        ESearchDir::FromStart, ShutdownDispatcher);
    const int32 ShutdownStop = SubsystemSource.Find(TEXT("StreamingTransport->Stop()"),
        ESearchCase::CaseSensitive, ESearchDir::FromStart, ShutdownFailPending);
    TestTrue(TEXT("subsystem quiesces intake before typed dispatcher abandonment and transport drain"),
        ShutdownQuiesce != INDEX_NONE && ShutdownDispatcher > ShutdownQuiesce &&
        ShutdownFailPending > ShutdownDispatcher && ShutdownStop > ShutdownFailPending);

    const int32 TransportStop = TransportSource.Find(TEXT("void FSocketHttpServer::Stop()"));
    const int32 StopQuiesce = TransportSource.Find(TEXT("QuiesceRequestIntake();"),
        ESearchCase::CaseSensitive, ESearchDir::FromStart, TransportStop);
    const int32 StopFailPending = TransportSource.Find(TEXT("FailAllCompletions("),
        ESearchCase::CaseSensitive, ESearchDir::FromStart, StopQuiesce);
    const int32 StopDrain = TransportSource.Find(TEXT("DrainPendingWrites("),
        ESearchCase::CaseSensitive, ESearchDir::FromStart, StopFailPending);
    const int32 StopSignal = TransportSource.Find(TEXT("IoRunnable->Stop()"),
        ESearchCase::CaseSensitive, ESearchDir::FromStart, StopDrain);
    const int32 StopJoin = TransportSource.Find(TEXT("IoThread->WaitForCompletion()"),
        ESearchCase::CaseSensitive, ESearchDir::FromStart, StopSignal);
    TestTrue(TEXT("transport boundedly drains resolved responses before stopping its I/O thread"),
        TransportStop != INDEX_NONE && StopQuiesce > TransportStop &&
        StopFailPending > StopQuiesce && StopDrain > StopFailPending &&
        StopSignal > StopDrain && StopJoin > StopSignal);
    const int32 SocketShutdown = TransportSource.Find(
        TEXT("Conn->Socket->Shutdown(ESocketShutdownMode::Write)"));
    const int32 LingerEntered = TransportSource.Find(TEXT("Conn->bLingering = true"),
        ESearchCase::CaseSensitive, ESearchDir::FromStart, SocketShutdown);
    const int32 LingerDeadline = TransportSource.Find(
        TEXT("Conn->LingerDeadlineSeconds = FPlatformTime::Seconds() + kCloseLingerSeconds"),
        ESearchCase::CaseSensitive, ESearchDir::FromStart, LingerEntered);
    const int32 LingerCap = TransportSource.Find(
        TEXT("Conn->LingerAbortSeconds = FPlatformTime::Seconds() + kMaxLingerSeconds"),
        ESearchCase::CaseSensitive, ESearchDir::FromStart, LingerDeadline);
    const int32 DrainCompleted = TransportSource.Find(TEXT("LastCompletedDrainId.store"),
        ESearchCase::CaseSensitive, ESearchDir::FromStart, LingerCap);
    TestTrue(TEXT("shutdown drain waits through the bounded FIN and linger phase"),
        TransportSource.Contains(TEXT("kShutdownWriteDrainSeconds = kMaxLingerSeconds + 1.0")) &&
        TransportSource.Contains(TEXT("Connections.Num() == 0")) &&
        SocketShutdown != INDEX_NONE && LingerEntered > SocketShutdown &&
        LingerDeadline > LingerEntered && LingerCap > LingerDeadline &&
        DrainCompleted > LingerCap);
    const int32 QuiescedDiscard = ReadLoop.Find(
        TEXT("if (!bAcceptingRequests.load(std::memory_order_acquire)"));
    const int32 InboundBufferReset = ReadLoop.Find(TEXT("Conn.InBuffer.Reset()"),
        ESearchCase::CaseSensitive, ESearchDir::FromStart, QuiescedDiscard);
    TestTrue(TEXT("quiesced connections keep boundedly reading and discarding inbound bytes"),
        TransportSource.Contains(TEXT("bool bAlive = PumpConnectionRead")) &&
        ReadLoop.Contains(TEXT("ReadBudgetBytes = kMaxReadBytesPerPump")) &&
        QuiescedDiscard != INDEX_NONE && InboundBufferReset > QuiescedDiscard);
    TestTrue(TEXT("accept and continuous-read loops observe shutdown gates"),
        AcceptLoop.Contains(TEXT("while (!bStopRequested.load(std::memory_order_acquire)\n           && bAcceptingRequests.load")) &&
        ReadLoop.Contains(TEXT("while (!bStopRequested.load(std::memory_order_acquire)\n           && ReadBudgetBytes > 0")));
    TestTrue(TEXT("transport serializes request handoff with its quiesce gate"),
        TransportSource.Contains(TEXT("FScopeLock IntakeLock(&RequestIntakeMutex)")) &&
        TransportSource.Contains(TEXT("bAcceptingRequests.load(std::memory_order_acquire)")) &&
        TransportSource.Contains(TEXT("OnRequestReceived.Unbind()")));
    TestTrue(TEXT("transport request callback does not retain raw subsystem access"),
        SubsystemSource.Contains(TEXT("OnRequestReceived.BindLambda(\n        [WeakDispatcher]")) &&
        SubsystemSource.Contains(TEXT("WeakDispatcher.Pin()")));
    TestTrue(TEXT("queued transport requests validate dispatcher lifetime on the game thread"),
        DispatcherSource.Contains(TEXT("[WeakLifetime, RequestId, Method, Params]")) &&
        DispatcherSource.Contains(TEXT("Lifetime.IsValid() && Lifetime->Dispatcher")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorStartRecordingRequiresActiveGameWorldTest,
    "PinWright.editor.start_recording.RequiresActiveGameWorld",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorStartRecordingRequiresActiveGameWorldTest::RunTest(const FString& Parameters)
{
    if (!GEditor || GEditor->PlayWorld)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("editor-state-unavailable"),
            TEXT("Skipped: this test needs an editor in edit mode and never starts PIE."));
        return true;
    }

    TGuardValue<bool> PieWorldFlagGuard(GIsPlayInEditorWorld, false);
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("PW_RequiresActiveGameWorld"));
    FTestResponseCapture Capture;
    TestTrue(TEXT("editor.start_recording handler is registered"),
        InvokeHandlerWithCapture(TEXT("editor.start_recording"), Payload, Capture));
    TestTrue(TEXT("edit-mode refusal responded"), Capture.bWasCalled);
    TestFalse(TEXT("edit mode cannot report recording success"), Capture.bSuccess);
    TestEqual(TEXT("edit mode reports NO_ACTIVE_GAME_WORLD"),
        Capture.ErrorCode, FString(TEXT("NO_ACTIVE_GAME_WORLD")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorStartRecordingVerifiesReplayStateTest,
    "PinWright.editor.start_recording.VerifiesReplayState",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorStartRecordingVerifiesReplayStateTest::RunTest(const FString& Parameters)
{
    using namespace EditorPieLifecycleTests;

    const FString Source = ReadHandlerSource(*this,
        TEXT("Source/PinWright/Private/Handlers/Editor/EditorCommandHandler.cpp"));
    const FString Recording = HandlerBlock(Source,
        TEXT("// ---- editor.start_recording ----"),
        TEXT("// ---- editor.stop_recording ----"));
    TestFalse(TEXT("editor.start_recording source block is bounded"), Recording.IsEmpty());
    if (Recording.IsEmpty())
    {
        return false;
    }

    TestTrue(TEXT("recording requires a GameInstance"),
        Recording.Contains(TEXT("GetGameInstance")) &&
        Recording.Contains(TEXT("NO_ACTIVE_GAME_WORLD")));
    TestTrue(TEXT("recording starts through the GameInstance replay API"),
        Recording.Contains(TEXT("StartRecordingReplay")));
    TestTrue(TEXT("recording success is gated by replay-state readback"),
        Recording.Contains(TEXT("ReplaySubsystem->IsRecording")) &&
        Recording.Contains(TEXT("REPLAY_RECORDING_FAILED")));
    TestTrue(TEXT("recording failure preserves the requested name without an active-name claim"),
        Recording.Contains(TEXT("ErrData->SetStringField(TEXT(\"requestedRecordingName\"), RequestedRecordingName)")) &&
        !Recording.Contains(TEXT("ErrData->SetStringField(TEXT(\"recordingName\"), RequestedRecordingName)")));
    TestTrue(TEXT("recording success returns requested and active replay names"),
        Recording.Contains(TEXT("Resp->SetStringField(TEXT(\"requestedRecordingName\"), RequestedRecordingName)")) &&
        Recording.Contains(TEXT("Resp->SetStringField(TEXT(\"recordingName\"), ReplaySubsystem->GetActiveReplayName())")));
    TestTrue(TEXT("recording response labels the demo path as a base directory"),
        Recording.Contains(TEXT("GetDemoNetDriver")) &&
        Recording.Contains(TEXT("Resp->SetStringField(TEXT(\"recordingBasePath\"), DemoNetDriver->GetDemoPath())")) &&
        !Recording.Contains(TEXT("\"recordingPath\"")));
    TestFalse(TEXT("recording no longer uses the consumed-no-op DemoRec command"),
        Recording.Contains(TEXT("GEditor->Exec")));
    return true;
}
