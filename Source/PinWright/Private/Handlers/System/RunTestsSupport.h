// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

namespace PinWrightRunTests
{
    inline constexpr double DefaultIsolatedChildTimeoutSeconds = 3600.0;
    inline constexpr double MinIsolatedChildTimeoutSeconds = 1.0;
    inline constexpr double MaxIsolatedChildTimeoutSeconds = 14400.0;

    class FRunLeaseState
    {
    public:
        // Returns zero while another generation owns the process-global automation controller.
        uint64 TryAcquire();
        bool IsOwner(uint64 Generation) const;
        bool Release(uint64 Generation);

    private:
        mutable FCriticalSection Mutex;
        uint64 NextGeneration = 1;
        uint64 ActiveGeneration = 0;
    };

    FRunLeaseState& GetProcessRunLeaseState();
    double ClampIsolatedChildTimeoutSeconds(double RequestedSeconds);

    enum class EIsolatedChildPollResult : uint8
    {
        Running,
        Exited,
        TimedOut
    };

    EIsolatedChildPollResult PollIsolatedChild(
        double StartedSeconds,
        double TimeoutSeconds,
        double NowSeconds,
        bool bHasExited);

    // Filter/RunAll completion. The job issues `Automation RunTests|RunAll` and
    // FAutomationExecCmd drives the controller. OnTestsComplete fires only when a
    // run the controller started drains; a filter that matches nothing never calls
    // RunTests, so it never broadcasts. The job therefore also polls the controller
    // state FAutomationExecCmd polls for its "Test Queue Empty" marker: Drained once
    // the controller has been Running and no longer is, NeverStarted if it has not
    // entered Running within StartTimeoutSeconds.
    enum class EFilterRunPollResult : uint8
    {
        Waiting,
        Drained,
        NeverStarted
    };

    EFilterRunPollResult PollFilterRun(
        bool bControllerRunning,
        bool& bInOutSawRunning,
        double ElapsedSeconds,
        double StartTimeoutSeconds);

    struct FControllerDelegateStats
    {
        int32 Bound = 0;
        int32 Unbound = 0;
        int32 Active = 0;
    };

    void NoteControllerDelegateBound();
    void NoteControllerDelegateUnbound();
    FControllerDelegateStats GetControllerDelegateStats();

#if WITH_DEV_AUTOMATION_TESTS
    class FScopedControllerJobHold
    {
    public:
        FScopedControllerJobHold();
        ~FScopedControllerJobHold();
        FScopedControllerJobHold(const FScopedControllerJobHold&) = delete;
        FScopedControllerJobHold& operator=(const FScopedControllerJobHold&) = delete;

        bool CompleteHeldJob();
    };

    bool ShouldHoldControllerJobForTests();
    void RegisterHeldControllerJobCompletionForTests(TFunction<void()> Completion);
#endif

    enum class ELogVerdict : uint8
    {
        CompletedClean,
        CompletedWithFailures,
        CompletedWithSkips,
        NoTests,
        DidNotComplete,
        Crashed
    };

    struct FLogVerdict
    {
        ELogVerdict Verdict = ELogVerdict::DidNotComplete;
        int32 Found = INDEX_NONE;
        int32 Started = 0;
        int32 Succeeded = 0;
        int32 Failed = 0;
        int32 Skipped = 0;
        int32 Performed = INDEX_NONE;
        bool bTerminalMarker = false;
        bool bCountsReconciled = false;
    };

    FLogVerdict EvaluateAutomationLog(const FString& LogText);
    const TCHAR* LogVerdictToString(ELogVerdict Verdict);

    bool SplitIsolatedGroups(const FString& Filter, TArray<FString>& OutGroups, FString& OutError);
    FString ResolveCommandletExecutable();
    FString BuildIsolatedGroupCommandLine(
        const FString& ProjectPath,
        const FString& GroupFilter,
        const FString& LogPath);
}
