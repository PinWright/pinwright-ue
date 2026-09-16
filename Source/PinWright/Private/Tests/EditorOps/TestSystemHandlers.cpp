// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for System domain handlers (SessionsHandler.cpp, SystemControlHandler.cpp)
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/System/RunTestsSupport.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "State/PluginState.h"
#include "Tests/TestUtils.h"

// ============================================================================
// session.add_local_player
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionAddLocalPlayerNoCrashTest,
    "PinWright.session.add_local_player.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSessionAddLocalPlayerNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("controllerId"), 1);

    // Handler will fail early (no active game instance in editor test context) — must not crash
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("session.add_local_player"), Payload));
    return true;
}

// ============================================================================
// session.remove_local_player  (RPC_PARAM_REQ: playerIndex)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionRemoveLocalPlayerValidParamsNoCrashTest,
    "PinWright.session.remove_local_player.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSessionRemoveLocalPlayerValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    // Index 1 — handler rejects index 0 and requires a live game instance, either way no crash
    Payload->SetNumberField(TEXT("playerIndex"), 1);

    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("session.remove_local_player"), Payload));
    return true;
}

// ============================================================================
// session.host_lan_server  (RPC_PARAM_REQ: mapName)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionHostLanServerValidParamsNoCrashTest,
    "PinWright.session.host_lan_server.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSessionHostLanServerValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("mapName"), TEXT("TestLevel"));
    Payload->SetStringField(TEXT("serverName"), TEXT("TestServer"));
    Payload->SetNumberField(TEXT("maxPlayers"), 4);
    // executeTravel=false so no world travel is attempted during tests
    Payload->SetBoolField(TEXT("executeTravel"), false);

    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("session.host_lan_server"), Payload));
    return true;
}

// ============================================================================
// session.get_sessions_info  (no params)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionGetSessionsInfoNoCrashTest,
    "PinWright.session.get_sessions_info.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSessionGetSessionsInfoNoCrashTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("session.get_sessions_info"), MakeShared<FJsonObject>()));
    return true;
}

// ============================================================================
// system.run_ubt
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemRunUbtNoCrashTest,
    "PinWright.system.run_ubt.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemRunUbtNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("target"), TEXT("MyProject"));
    Payload->SetStringField(TEXT("platform"), TEXT("Win64"));
    Payload->SetStringField(TEXT("configuration"), TEXT("Development"));

    // Handler will fail early if UBT batch file is absent from the test environment — must not crash
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("system.run_ubt"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemRunUbtEmptyPayloadNoCrashTest,
    "PinWright.system.run_ubt.EmptyPayloadNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemRunUbtEmptyPayloadNoCrashTest::RunTest(const FString& Parameters)
{
    // All params optional — defaults kick in; handler gracefully fails if UBT is absent
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("system.run_ubt"), MakeShared<FJsonObject>()));
    return true;
}

// ============================================================================
// system.run_tests
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemRunTestsNoCrashTest,
    "PinWright.system.run_tests.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemRunTestsNoCrashTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("Handler is registered"), IsHandlerRegistered(TEXT("system.run_tests")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemRunTestsEmptyPayloadNoCrashTest,
    "PinWright.system.run_tests.EmptyPayloadNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemRunTestsEmptyPayloadNoCrashTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("Handler is registered"), IsHandlerRegistered(TEXT("system.run_tests")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemRunTestsSchemaIncludesTestsArrayTest,
    "PinWright.system.run_tests.SchemaIncludesTestsArray",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemRunTestsSchemaIncludesTestsArrayTest::RunTest(const FString& Parameters)
{
    bool bFoundHandler = false;
    bool bFoundTestsParam = false;
    for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
    {
        if (Reg.MethodName != TEXT("system.run_tests"))
        {
            continue;
        }

        bFoundHandler = true;
        for (const FParamSpec& Param : Reg.Params)
        {
            if (Param.Name == TEXT("tests"))
            {
                bFoundTestsParam = true;
                TestEqual(TEXT("tests param type"), Param.Type, FString(TEXT("array")));
                TestFalse(TEXT("tests param optional"), Param.bRequired);
            }
        }
    }

    TestTrue(TEXT("system.run_tests handler found"), bFoundHandler);
    TestTrue(TEXT("tests array param found"), bFoundTestsParam);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemRunTestsSchemaIncludesIsolationTest,
    "PinWright.system.run_tests.SchemaIncludesIsolateGroups",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemRunTestsSchemaIncludesIsolationTest::RunTest(const FString& Parameters)
{
    bool bFoundIsolateGroups = false;
    bool bFoundChildTimeout = false;
    for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
    {
        if (Reg.MethodName != TEXT("system.run_tests"))
        {
            continue;
        }
        for (const FParamSpec& Param : Reg.Params)
        {
            if (Param.Name == TEXT("isolateGroups"))
            {
                bFoundIsolateGroups = true;
                TestEqual(TEXT("isolateGroups param type"), Param.Type, FString(TEXT("boolean")));
                TestFalse(TEXT("isolateGroups param optional"), Param.bRequired);
                TestEqual(TEXT("isolateGroups defaults false"), Param.Default, FString(TEXT("false")));
            }
            else if (Param.Name == TEXT("childTimeoutSeconds"))
            {
                bFoundChildTimeout = true;
                TestEqual(TEXT("child timeout param type"), Param.Type, FString(TEXT("number")));
                TestFalse(TEXT("child timeout param optional"), Param.bRequired);
                TestEqual(TEXT("child timeout defaults to one hour"),
                    Param.Default, FString(TEXT("3600")));
            }
        }
    }

    TestTrue(TEXT("isolateGroups param found"), bFoundIsolateGroups);
    TestTrue(TEXT("childTimeoutSeconds param found"), bFoundChildTimeout);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemRunTestsConcurrentJobLeaseTest,
    "PinWright.system.run_tests.ConcurrentJobLease",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemRunTestsConcurrentJobLeaseTest::RunTest(const FString& Parameters)
{
    PinWrightRunTests::FRunLeaseState Lease;
    const uint64 FirstGeneration = Lease.TryAcquire();
    TestTrue(TEXT("first job acquires the lease"), FirstGeneration != 0);
    TestEqual(TEXT("concurrent job is refused"), Lease.TryAcquire(), uint64(0));
    TestFalse(TEXT("stale generation cannot release the owner"), Lease.Release(FirstGeneration + 1));
    TestTrue(TEXT("first generation still owns after stale release"), Lease.IsOwner(FirstGeneration));
    TestTrue(TEXT("owner releases its lease"), Lease.Release(FirstGeneration));

    const uint64 SecondGeneration = Lease.TryAcquire();
    TestTrue(TEXT("next job receives a new generation"),
        SecondGeneration != 0 && SecondGeneration != FirstGeneration);
    TestFalse(TEXT("old generation cannot release the new owner"), Lease.Release(FirstGeneration));
    TestTrue(TEXT("new generation remains owner"), Lease.IsOwner(SecondGeneration));
    TestTrue(TEXT("new owner releases its lease"), Lease.Release(SecondGeneration));

    PinWrightRunTests::FScopedControllerJobHold HeldControllerJob;
    const PinWrightRunTests::FControllerDelegateStats Before =
        PinWrightRunTests::GetControllerDelegateStats();
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("test"), TEXT("PinWright.system.run_tests.ValidParamsNoCrash"));
    FTestResponseCapture FirstCapture;
    TestTrue(TEXT("first real job handler found"),
        InvokeHandlerWithCapture(TEXT("system.run_tests"), Payload, FirstCapture));
    TestTrue(TEXT("first real job starts"), FirstCapture.bSuccess);
    FString FirstTicketId;
    if (!FirstCapture.bSuccess || !FirstCapture.Result.IsValid() ||
        !FirstCapture.Result->TryGetStringField(TEXT("ticket_id"), FirstTicketId))
    {
        AddError(TEXT("first real job did not return a ticket_id"));
        return true;
    }

    const PinWrightRunTests::FControllerDelegateStats AfterFirst =
        PinWrightRunTests::GetControllerDelegateStats();
    TestEqual(TEXT("first exact-name job binds two controller delegates"),
        AfterFirst.Bound - Before.Bound, 2);
    TestEqual(TEXT("first exact-name job owns both active delegates"),
        AfterFirst.Active - Before.Active, 2);

    FTestResponseCapture SecondCapture;
    TestTrue(TEXT("second real job handler found"),
        InvokeHandlerWithCapture(TEXT("system.run_tests"), Payload, SecondCapture));
    TestTrue(TEXT("conflict response captured"), SecondCapture.bWasCalled);
    TestFalse(TEXT("second real job is rejected"), SecondCapture.bSuccess);
    TestEqual(TEXT("typed conflict code"),
        SecondCapture.ErrorCode, FString(TEXT("AUTOMATION_RUN_IN_PROGRESS")));
    const PinWrightRunTests::FControllerDelegateStats AfterSecond =
        PinWrightRunTests::GetControllerDelegateStats();
    TestEqual(TEXT("refused job binds no controller delegates"),
        AfterSecond.Bound, AfterFirst.Bound);
    TestEqual(TEXT("only the first job's delegates remain active"),
        AfterSecond.Active, AfterFirst.Active);

    TestTrue(TEXT("held first job completes through its production cleanup"),
        HeldControllerJob.CompleteHeldJob());
    FJobTicket FirstTicket;
    TestTrue(TEXT("first ticket remains queryable"),
        FPluginState::Get().GetJobRegistry().Get(FirstTicketId, FirstTicket));
    TestEqual(TEXT("first ticket completes"), FirstTicket.Status, FString(TEXT("completed")));
    const PinWrightRunTests::FControllerDelegateStats AfterCompletion =
        PinWrightRunTests::GetControllerDelegateStats();
    TestEqual(TEXT("first job unbinds both controller delegates"),
        AfterCompletion.Unbound - Before.Unbound, 2);
    TestEqual(TEXT("no controller delegates remain active"),
        AfterCompletion.Active, Before.Active);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemRunTestsIsolatedGroupCommandLinesTest,
    "PinWright.system.run_tests.IsolatedGroupCommandLines",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemRunTestsIsolatedGroupCommandLinesTest::RunTest(const FString& Parameters)
{
    TArray<FString> Groups;
    FString Error;
    TestTrue(TEXT("combined filter splits into groups"),
        PinWrightRunTests::SplitIsolatedGroups(
            TEXT("PinWright.actor + PinWright.blueprint"), Groups, Error));
    if (!TestEqual(TEXT("two isolated groups"), Groups.Num(), 2))
    {
        return true;
    }

    const FString FirstCommand = PinWrightRunTests::BuildIsolatedGroupCommandLine(
        TEXT("X:/Project/Host.uproject"), Groups[0], TEXT("X:/Logs/group-001.log"));
    const FString SecondCommand = PinWrightRunTests::BuildIsolatedGroupCommandLine(
        TEXT("X:/Project/Host.uproject"), Groups[1], TEXT("X:/Logs/group-002.log"));
    TestTrue(TEXT("first child runs only the first group"),
        FirstCommand.Contains(Groups[0]) && !FirstCommand.Contains(Groups[1]));
    TestTrue(TEXT("second child runs only the second group"),
        SecondCommand.Contains(Groups[1]) && !SecondCommand.Contains(Groups[0]));
    TestTrue(TEXT("child command requests the queue-empty terminal marker"),
        FirstCommand.Contains(TEXT("-TestExit=\"Automation Test Queue Empty\"")));
    TestTrue(TEXT("child command suppresses shared multiprocess config writes"),
        FirstCommand.Contains(TEXT("-Multiprocess")));
    TestTrue(TEXT("each child receives its own absolute log"),
        FirstCommand.Contains(TEXT("group-001.log")) &&
        SecondCommand.Contains(TEXT("group-002.log")));
    TestTrue(TEXT("each child receives a private PinWright monitor path"),
        FirstCommand.Contains(TEXT("-PinWrightIsolatedTestChild=\"X:/Logs/group-001.jobs.jsonl\"")) &&
        SecondCommand.Contains(TEXT("-PinWrightIsolatedTestChild=\"X:/Logs/group-002.jobs.jsonl\"")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemRunTestsFakeSleepingChildTimeoutTest,
    "PinWright.system.run_tests.FakeSleepingChildTimesOut",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemRunTestsFakeSleepingChildTimeoutTest::RunTest(const FString& Parameters)
{
    const TFunction<bool()> FakeSleepingChildHasExited = []()
    {
        return false;
    };
    constexpr double StartedSeconds = 100.0;
    constexpr double TimeoutSeconds = 5.0;

    TestEqual(TEXT("child timeout is capped at the hard maximum"),
        PinWrightRunTests::ClampIsolatedChildTimeoutSeconds(999999.0),
        PinWrightRunTests::MaxIsolatedChildTimeoutSeconds);
    TestTrue(TEXT("sleeping child remains running before its deadline"),
        PinWrightRunTests::PollIsolatedChild(
            StartedSeconds, TimeoutSeconds, 104.9, FakeSleepingChildHasExited()) ==
        PinWrightRunTests::EIsolatedChildPollResult::Running);
    TestTrue(TEXT("sleeping child times out at its deadline"),
        PinWrightRunTests::PollIsolatedChild(
            StartedSeconds, TimeoutSeconds, 105.0, FakeSleepingChildHasExited()) ==
        PinWrightRunTests::EIsolatedChildPollResult::TimedOut);
    TestTrue(TEXT("an exited child wins over timeout at the same poll"),
        PinWrightRunTests::PollIsolatedChild(
            StartedSeconds, TimeoutSeconds, 105.0, /*bHasExited=*/true) ==
        PinWrightRunTests::EIsolatedChildPollResult::Exited);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemRunTestsIsolatedChildStateBoundaryTest,
    "PinWright.system.run_tests.IsolatedChildStateBoundary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemRunTestsIsolatedChildStateBoundaryTest::RunTest(const FString& Parameters)
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
    if (!TestTrue(TEXT("PinWright plugin resolved for isolation source contract"), Plugin.IsValid()))
    {
        return false;
    }

    FString SubsystemSource;
    FString StateSource;
    FString HandlerSource;
    const FString SubsystemPath = Plugin->GetBaseDir()
        / TEXT("Source/PinWright/Private/PinWrightSubsystem.cpp");
    const FString StatePath = Plugin->GetBaseDir()
        / TEXT("Source/PinWright/Private/State/PluginState.cpp");
    const FString HandlerPath = Plugin->GetBaseDir()
        / TEXT("Source/PinWright/Private/Handlers/System/SystemControlHandler.cpp");
    if (!TestTrue(TEXT("read subsystem isolation boundary"),
            FFileHelper::LoadFileToString(SubsystemSource, *SubsystemPath)) ||
        !TestTrue(TEXT("read private monitor routing"),
            FFileHelper::LoadFileToString(StateSource, *StatePath)) ||
        !TestTrue(TEXT("read isolated child lifecycle"),
            FFileHelper::LoadFileToString(HandlerSource, *HandlerPath)))
    {
        return false;
    }

    SubsystemSource.ReplaceInline(TEXT("\r"), TEXT(""));
    StateSource.ReplaceInline(TEXT("\r"), TEXT(""));
    HandlerSource.ReplaceInline(TEXT("\r"), TEXT(""));
    TestTrue(TEXT("isolated child skips shared wiki generation"),
        SubsystemSource.Contains(
            TEXT("if (!bIsolatedTestChild)\n    {\n        WikiDiskGenerator::Generate();")));
    TestTrue(TEXT("isolated child skips the shared job-monitor wipe"),
        SubsystemSource.Contains(
            TEXT("if (!bIsolatedTestChild)\n    {\n        const FString JobsPath")));
    TestTrue(TEXT("isolated child disables PinWright HTTP startup"),
        SubsystemSource.Contains(
            TEXT("if (bIsolatedTestChild)\n    {\n        UE_LOG")));
    TestTrue(TEXT("isolated child routes jobs to its private monitor path"),
        StateSource.Contains(
            TEXT("bHasPrivateMonitorPath\n            ? IsolatedTestChildMonitorPath")));
    TestTrue(TEXT("isolated job registers a real cancellation callback"),
        HandlerSource.Contains(TEXT("GetJobRegistry().SetCancelCallback(")) &&
        HandlerSource.Contains(TEXT("Job->Cancel();")));
    TestTrue(TEXT("isolated job termination owns the whole child tree"),
        HandlerSource.Contains(
            TEXT("FPlatformProcess::TerminateProc(ProcessHandle, /*KillTree=*/true);")));
    TestTrue(TEXT("timeout result names the expired group"),
        HandlerSource.Contains(TEXT("SetBoolField(TEXT(\"timedOut\"), true);")) &&
        HandlerSource.Contains(TEXT("SetStringField(TEXT(\"timedOutGroup\"), TimedOutGroup);")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemRunTestsTruncatedLogVerdictTest,
    "PinWright.system.run_tests.TruncatedLogIsNotGreen",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemRunTestsTruncatedLogVerdictTest::RunTest(const FString& Parameters)
{
    const FString CommandEcho =
        TEXT("LogInit: Display: Command Line: Host.uproject ")
        TEXT("-TestExit=\"Automation Test Queue Empty\"\n");
    const FString FirstResult =
        TEXT("LogAutomationCommandLine: Display: Found 2 automation tests based on 'PinWright'\n")
        TEXT("LogAutomationController: Display: Test Started. Name={A}\n")
        TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={A}\n");

    const PinWrightRunTests::FLogVerdict Truncated =
        PinWrightRunTests::EvaluateAutomationLog(CommandEcho + FirstResult);
    TestTrue(TEXT("command-line marker echo is not terminal"), !Truncated.bTerminalMarker);
    TestEqual(TEXT("truncated fixture has zero failures"), Truncated.Failed, 0);
    TestTrue(TEXT("zero-failure truncation is not clean"),
        Truncated.Verdict == PinWrightRunTests::ELogVerdict::DidNotComplete);

    const FString StaleMarker =
        FirstResult +
        TEXT("LogAutomationCommandLine: Display: ...Automation Test Queue Empty 1 tests performed.\n")
        TEXT("LogAutomationController: Display: Test Started. Name={B}\n")
        TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={B}\n");
    const PinWrightRunTests::FLogVerdict Stale =
        PinWrightRunTests::EvaluateAutomationLog(StaleMarker);
    TestTrue(TEXT("marker before the final result is not terminal"), !Stale.bTerminalMarker);
    TestTrue(TEXT("stale marker cannot produce green"),
        Stale.Verdict == PinWrightRunTests::ELogVerdict::DidNotComplete);

    const FString MissingFound =
        TEXT("LogAutomationController: Display: Test Started. Name={A}\n")
        TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={A}\n")
        TEXT("LogAutomationCommandLine: Display: ...Automation Test Queue Empty 1 tests performed.\n");
    const PinWrightRunTests::FLogVerdict NoFound =
        PinWrightRunTests::EvaluateAutomationLog(MissingFound);
    TestEqual(TEXT("missing discovery count remains absent"), NoFound.Found, INDEX_NONE);
    TestFalse(TEXT("found, started, finished, and performed must all reconcile"),
        NoFound.bCountsReconciled);
    TestTrue(TEXT("missing Found marker cannot produce green"),
        NoFound.Verdict == PinWrightRunTests::ELogVerdict::DidNotComplete);

    const FString Clean =
        CommandEcho + FirstResult +
        TEXT("LogAutomationController: Display: Test Started. Name={B}\n")
        TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={B}\n")
        TEXT("LogAutomationCommandLine: Display: ...Automation Test Queue Empty 2 tests performed.\n");
    const PinWrightRunTests::FLogVerdict Complete =
        PinWrightRunTests::EvaluateAutomationLog(Clean);
    TestTrue(TEXT("real terminal marker is accepted"), Complete.bTerminalMarker);
    TestTrue(TEXT("complete counts reconcile"), Complete.bCountsReconciled);
    TestTrue(TEXT("complete clean fixture stays green"),
        Complete.Verdict == PinWrightRunTests::ELogVerdict::CompletedClean);

    const PinWrightRunTests::FLogVerdict Skipped = PinWrightRunTests::EvaluateAutomationLog(
        Clean + TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: PinWright.host.Dependent reason=fixture\n"));
    TestEqual(TEXT("skip marker is counted"), Skipped.Skipped, 1);
    TestTrue(TEXT("skipped assertions never classify as clean"),
        Skipped.Verdict == PinWrightRunTests::ELogVerdict::CompletedWithSkips);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemRunTestsFilterAndTestRejectedTest,
    "PinWright.system.run_tests.FilterAndTestRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemRunTestsFilterAndTestRejectedTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("filter"), TEXT("PinWright"));
    Payload->SetStringField(TEXT("test"), TEXT("PinWright.system.run_tests.ValidParamsNoCrash"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("Handler found"), InvokeHandlerWithCapture(TEXT("system.run_tests"), Payload, Capture));
    TestTrue(TEXT("Response captured"), Capture.bWasCalled);
    TestFalse(TEXT("Request rejected"), Capture.bSuccess);
    TestEqual(TEXT("Error code"), Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemRunTestsFilterAndTestsRejectedTest,
    "PinWright.system.run_tests.FilterAndTestsRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemRunTestsFilterAndTestsRejectedTest::RunTest(const FString& Parameters)
{
    TArray<TSharedPtr<FJsonValue>> Tests;
    Tests.Add(MakeShared<FJsonValueString>(TEXT("PinWright.system.run_tests.ValidParamsNoCrash")));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("filter"), TEXT("PinWright"));
    Payload->SetArrayField(TEXT("tests"), Tests);

    FTestResponseCapture Capture;
    TestTrue(TEXT("Handler found"), InvokeHandlerWithCapture(TEXT("system.run_tests"), Payload, Capture));
    TestTrue(TEXT("Response captured"), Capture.bWasCalled);
    TestFalse(TEXT("Request rejected"), Capture.bSuccess);
    TestEqual(TEXT("Error code"), Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemRunTestsNonStringTestsRejectedTest,
    "PinWright.system.run_tests.NonStringTestsRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemRunTestsNonStringTestsRejectedTest::RunTest(const FString& Parameters)
{
    TArray<TSharedPtr<FJsonValue>> Tests;
    Tests.Add(MakeShared<FJsonValueString>(TEXT("PinWright.system.run_tests.ValidParamsNoCrash")));
    Tests.Add(MakeShared<FJsonValueNumber>(42.0));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetArrayField(TEXT("tests"), Tests);

    FTestResponseCapture Capture;
    TestTrue(TEXT("Handler found"), InvokeHandlerWithCapture(TEXT("system.run_tests"), Payload, Capture));
    TestTrue(TEXT("Response captured"), Capture.bWasCalled);
    TestFalse(TEXT("Request rejected"), Capture.bSuccess);
    TestEqual(TEXT("Error code"), Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemRunTestsEmptyTestsRejectedTest,
    "PinWright.system.run_tests.EmptyTestsRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemRunTestsEmptyTestsRejectedTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetArrayField(TEXT("tests"), TArray<TSharedPtr<FJsonValue>>());

    FTestResponseCapture Capture;
    TestTrue(TEXT("Handler found"), InvokeHandlerWithCapture(TEXT("system.run_tests"), Payload, Capture));
    TestTrue(TEXT("Response captured"), Capture.bWasCalled);
    TestFalse(TEXT("Request rejected"), Capture.bSuccess);
    TestEqual(TEXT("Error code"), Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
    return true;
}

// ============================================================================
// system.console.search
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemConsoleSearchWellKnownCvarFoundTest,
    "PinWright.system.console.search.WellKnownCvarFound",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemConsoleSearchWellKnownCvarFoundTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("query"), TEXT("r.ScreenPercentage"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("Handler found"), InvokeHandlerWithCapture(TEXT("system.console.search"), Payload, Capture));
    TestTrue(TEXT("Response captured"), Capture.bWasCalled);
    TestTrue(TEXT("Request succeeded"), Capture.bSuccess);
    if (!Capture.Result.IsValid()) return false;

    const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
    TestTrue(TEXT("results array present"), Capture.Result->TryGetArrayField(TEXT("results"), Results));
    if (!Results || Results->Num() == 0) return false;

    const TSharedPtr<FJsonObject>& Row = (*Results)[0]->AsObject();
    TestEqual(TEXT("first row name"), Row->GetStringField(TEXT("name")), FString(TEXT("r.ScreenPercentage")));
    TestEqual(TEXT("first row kind"), Row->GetStringField(TEXT("kind")), FString(TEXT("variable")));
    TestFalse(TEXT("help non-empty"), Row->GetStringField(TEXT("help")).IsEmpty());
    TestFalse(TEXT("currentValue non-empty"), Row->GetStringField(TEXT("currentValue")).IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemConsoleSearchStatCommandsFoundTest,
    "PinWright.system.console.search.StatCommandsFound",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemConsoleSearchStatCommandsFoundTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    // Trailing space narrows to `Stat <Foo>` style entries that are registered as commands.
    Payload->SetStringField(TEXT("query"), TEXT("Stat "));
    Payload->SetStringField(TEXT("kind"), TEXT("command"));
    Payload->SetNumberField(TEXT("limit"), 5);

    FTestResponseCapture Capture;
    TestTrue(TEXT("Handler found"), InvokeHandlerWithCapture(TEXT("system.console.search"), Payload, Capture));
    TestTrue(TEXT("Response captured"), Capture.bWasCalled);
    TestTrue(TEXT("Request succeeded"), Capture.bSuccess);
    if (!Capture.Result.IsValid()) return false;

    double TotalMatches = 0.0;
    TestTrue(TEXT("totalMatches present"), Capture.Result->TryGetNumberField(TEXT("totalMatches"), TotalMatches));
    TestTrue(TEXT("totalMatches >= 1"), TotalMatches >= 1.0);

    const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
    TestTrue(TEXT("results array present"), Capture.Result->TryGetArrayField(TEXT("results"), Results));
    if (!Results) return false;
    for (const TSharedPtr<FJsonValue>& Val : *Results)
    {
        const TSharedPtr<FJsonObject>& Row = Val->AsObject();
        TestEqual(TEXT("row kind == command"), Row->GetStringField(TEXT("kind")), FString(TEXT("command")));
    }
    return true;
}

// Pins the documented name-only/single-token matching contract (see the
// "Console command discovery" section of docs/wiki-src/system.md). The search
// matches console-OBJECT NAMES, so a multi-token command line like `stat unit`
// — which `system.console_command` executes fine — false-negatives to zero
// matches, while the bare leading token `Stat ` returns rows. This guards the
// contract the docs now describe: if someone silently reintroduced the dropped
// whitespace auto-retry (matching the leading token behind the user's back),
// `stat unit` would start returning matches and this test would fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemConsoleSearchMultiTokenNoMatchTest,
    "PinWright.system.console.search.MultiTokenNoMatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemConsoleSearchMultiTokenNoMatchTest::RunTest(const FString& Parameters)
{
    // The bare leading token `Stat ` -> rows (totalMatches >= 1) is pinned by
    // FSystemConsoleSearchStatCommandsFoundTest directly above; not restated here.
    //
    // The full multi-token command line is not a single console-object name, so
    // the registry substring match returns zero — the documented false-negative.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("query"), TEXT("stat unit"));
        Payload->SetStringField(TEXT("kind"), TEXT("command"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("Handler found (multi-token)"), InvokeHandlerWithCapture(TEXT("system.console.search"), Payload, Capture));
        TestTrue(TEXT("Multi-token request succeeded"), Capture.bSuccess);
        if (!Capture.Result.IsValid()) return false;

        double TotalMatches = -1.0;
        TestTrue(TEXT("multi-token totalMatches present"), Capture.Result->TryGetNumberField(TEXT("totalMatches"), TotalMatches));
        // A space-bearing command line matches no single object name; the handler
        // must NOT silently retry the leading token (that behavior was dropped).
        TestEqual(TEXT("multi-token totalMatches == 0"), TotalMatches, 0.0);

        const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
        TestTrue(TEXT("results array present"), Capture.Result->TryGetArrayField(TEXT("results"), Results));
        if (Results)
        {
            TestEqual(TEXT("no result rows for multi-token query"), Results->Num(), 0);
        }
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemConsoleSearchEmptyQueryRejectedTest,
    "PinWright.system.console.search.EmptyQueryRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemConsoleSearchEmptyQueryRejectedTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("query"), TEXT(""));

    FTestResponseCapture Capture;
    TestTrue(TEXT("Handler found"), InvokeHandlerWithCapture(TEXT("system.console.search"), Payload, Capture));
    TestTrue(TEXT("Response captured"), Capture.bWasCalled);
    TestFalse(TEXT("Request rejected"), Capture.bSuccess);
    TestEqual(TEXT("Error code"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemConsoleSearchLimitTruncatesTest,
    "PinWright.system.console.search.LimitTruncates",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemConsoleSearchLimitTruncatesTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    // `r.` is one of the highest-volume CVar prefixes in UE — hundreds of matches on a stock editor.
    Payload->SetStringField(TEXT("query"), TEXT("r."));
    Payload->SetNumberField(TEXT("limit"), 3);

    FTestResponseCapture Capture;
    TestTrue(TEXT("Handler found"), InvokeHandlerWithCapture(TEXT("system.console.search"), Payload, Capture));
    TestTrue(TEXT("Response captured"), Capture.bWasCalled);
    TestTrue(TEXT("Request succeeded"), Capture.bSuccess);
    if (!Capture.Result.IsValid()) return false;

    const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
    TestTrue(TEXT("results array present"), Capture.Result->TryGetArrayField(TEXT("results"), Results));
    if (!Results) return false;
    TestEqual(TEXT("results clamped to limit"), Results->Num(), 3);

    double TotalMatches = 0.0;
    TestTrue(TEXT("totalMatches present"), Capture.Result->TryGetNumberField(TEXT("totalMatches"), TotalMatches));
    TestTrue(TEXT("totalMatches exceeds limit"), TotalMatches > 3.0);

    bool bTruncated = false;
    TestTrue(TEXT("truncated flag present"), Capture.Result->TryGetBoolField(TEXT("truncated"), bTruncated));
    TestTrue(TEXT("truncated == true"), bTruncated);
    return true;
}

// Regression test for E-console-search-default-limit-spills.
// system.console.search must support a namesOnly/fields projection so a broad
// discovery scan can drop the byte-dominating multi-line `help` string and stay
// inline instead of spilling the response to a file. Counterfactual: on pre-fix
// code (RPC_PARAMS declared only query/kind/limit, and every row emitted `help`
// unconditionally), the namesOnly/fields params were undeclared no-ops and the
// row still carried `help` — so the "namesOnly drops help" / "fields=[name] keeps
// only name" assertions below fail.
//
// Plants a private console variable with a uniquely-named query token and a known
// non-empty help string so the search is deterministic regardless of the host's
// stock cvar set, then routes through the production handler via
// InvokeHandlerWithCapture (the projection logic is never reimplemented here).
namespace
{
    // Unregisters the planted test cvar on scope exit so the registry is left clean
    // even on an early-out assertion path.
    struct FScopedTestConsoleVariable
    {
        IConsoleVariable* Var = nullptr;
        ~FScopedTestConsoleVariable()
        {
            if (Var)
            {
                IConsoleManager::Get().UnregisterConsoleObject(Var);
            }
        }
    };

    // Returns the first result-row object whose `name` equals Name (the planted cvar
    // sorts to relevance tier 0 as an exact match, but other rows may precede it in the
    // array on shared-token queries — match by name to be robust).
    TSharedPtr<FJsonObject> FindResultRowByName(const TSharedPtr<FJsonObject>& Result, const FString& Name)
    {
        return JsonArrayFindObjectByStringField(Result, TEXT("results"), TEXT("name"), Name);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemConsoleSearchNamesOnlyProjectionTest,
    "PinWright.system.console.search.NamesOnlyProjectionDropsHelp",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemConsoleSearchNamesOnlyProjectionTest::RunTest(const FString& Parameters)
{
    // A query token that cannot collide with stock UE cvars, so the only exact-name
    // match is the cvar we plant below.
    const FString CvarName = TEXT("EAGatewayTest.ConsoleSearchProjectionProbe");
    const FString HelpText = TEXT("Probe cvar for the namesOnly/fields projection regression test - this multi-line help string is the byte dominator the projection must be able to drop.");

    FScopedTestConsoleVariable Scoped;
    Scoped.Var = IConsoleManager::Get().RegisterConsoleVariable(
        *CvarName, 7, *HelpText, ECVF_Default);
    if (!TestNotNull(TEXT("planted test cvar registered"), Scoped.Var))
    {
        return true;
    }

    // 1) Unprojected (default) search returns the row WITH help + currentValue —
    //    proving the projection is opt-in and the legacy shape is unchanged.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("query"), CvarName);

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler found (default)"), InvokeHandlerWithCapture(TEXT("system.console.search"), Payload, Capture));
        TestTrue(TEXT("default succeeded"), Capture.bSuccess);
        if (!Capture.Result.IsValid()) return false;

        TSharedPtr<FJsonObject> Row = FindResultRowByName(Capture.Result, CvarName);
        if (!TestTrue(TEXT("default: planted cvar row present"), Row.IsValid())) return true;

        FString Help;
        TestTrue(TEXT("default: help present"), Row->TryGetStringField(TEXT("help"), Help));
        TestEqual(TEXT("default: help is the planted text"), Help, HelpText);
        TestTrue(TEXT("default: currentValue present"), Row->HasField(TEXT("currentValue")));
        TestTrue(TEXT("default: kind present"), Row->HasField(TEXT("kind")));
    }

    // 2) namesOnly:true drops the fat `help` column but keeps name/kind/currentValue/flags —
    //    this is the inline-friendly discovery scan the ticket asks for.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("query"), CvarName);
        Payload->SetBoolField(TEXT("namesOnly"), true);

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler found (namesOnly)"), InvokeHandlerWithCapture(TEXT("system.console.search"), Payload, Capture));
        TestTrue(TEXT("namesOnly succeeded"), Capture.bSuccess);
        if (!Capture.Result.IsValid()) return false;

        TSharedPtr<FJsonObject> Row = FindResultRowByName(Capture.Result, CvarName);
        if (!TestTrue(TEXT("namesOnly: planted cvar row present"), Row.IsValid())) return true;

        TestFalse(TEXT("namesOnly: help column dropped"), Row->HasField(TEXT("help")));
        TestTrue(TEXT("namesOnly: name retained"), Row->HasField(TEXT("name")));
        TestTrue(TEXT("namesOnly: kind retained"), Row->HasField(TEXT("kind")));
        TestTrue(TEXT("namesOnly: currentValue retained (variable)"), Row->HasField(TEXT("currentValue")));
        TestTrue(TEXT("namesOnly: flags retained"), Row->HasField(TEXT("flags")));
    }

    // 3) fields:["name"] returns ONLY the name column — finer-grained projection,
    //    and proves fields wins over the default full row.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("query"), CvarName);
        TArray<TSharedPtr<FJsonValue>> FieldsArr;
        FieldsArr.Add(MakeShared<FJsonValueString>(TEXT("name")));
        Payload->SetArrayField(TEXT("fields"), FieldsArr);

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler found (fields)"), InvokeHandlerWithCapture(TEXT("system.console.search"), Payload, Capture));
        TestTrue(TEXT("fields succeeded"), Capture.bSuccess);
        if (!Capture.Result.IsValid()) return false;

        TSharedPtr<FJsonObject> Row = FindResultRowByName(Capture.Result, CvarName);
        if (!TestTrue(TEXT("fields: planted cvar row present"), Row.IsValid())) return true;

        TestTrue(TEXT("fields=[name]: name retained"), Row->HasField(TEXT("name")));
        TestFalse(TEXT("fields=[name]: help dropped"), Row->HasField(TEXT("help")));
        TestFalse(TEXT("fields=[name]: kind dropped"), Row->HasField(TEXT("kind")));
        TestFalse(TEXT("fields=[name]: currentValue dropped"), Row->HasField(TEXT("currentValue")));
        TestFalse(TEXT("fields=[name]: flags dropped"), Row->HasField(TEXT("flags")));
    }

    return true;
}
