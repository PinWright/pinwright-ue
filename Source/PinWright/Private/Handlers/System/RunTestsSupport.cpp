// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/System/RunTestsSupport.h"

#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

namespace PinWrightRunTests
{
    namespace
    {
        FCriticalSection ControllerDelegateStatsMutex;
        FControllerDelegateStats ControllerDelegateStats;

#if WITH_DEV_AUTOMATION_TESTS
        bool bHoldControllerJobForTests = false;
        TFunction<void()> HeldControllerJobCompletion;
#endif

        bool TryParseRunTestsNumberBetween(
            const FString& Line,
            const TCHAR* Prefix,
            const TCHAR* Suffix,
            int32& OutValue)
        {
            const int32 PrefixIndex = Line.Find(Prefix, ESearchCase::IgnoreCase);
            if (PrefixIndex == INDEX_NONE)
            {
                return false;
            }

            const int32 NumberIndex = PrefixIndex + FCString::Strlen(Prefix);
            const int32 SuffixIndex = Line.Find(Suffix, ESearchCase::IgnoreCase,
                                                ESearchDir::FromStart, NumberIndex);
            if (SuffixIndex <= NumberIndex)
            {
                return false;
            }

            const FString Digits = Line.Mid(NumberIndex, SuffixIndex - NumberIndex).TrimStartAndEnd();
            if (Digits.IsEmpty())
            {
                return false;
            }
            for (const TCHAR Character : Digits)
            {
                if (!FChar::IsDigit(Character))
                {
                    return false;
                }
            }

            OutValue = FCString::Atoi(*Digits);
            return true;
        }

        bool IsRunTestsArgumentEcho(const FString& Line)
        {
            return Line.Contains(TEXT("Command Line:"), ESearchCase::IgnoreCase) ||
                   Line.Contains(TEXT("commandline="), ESearchCase::IgnoreCase) ||
                   Line.Contains(TEXT("-TestExit"), ESearchCase::IgnoreCase) ||
                   Line.Contains(TEXT("-ExecCmds"), ESearchCase::IgnoreCase);
        }
    }

    uint64 FRunLeaseState::TryAcquire()
    {
        FScopeLock Lock(&Mutex);
        if (ActiveGeneration != 0)
        {
            return 0;
        }

        ActiveGeneration = NextGeneration++;
        if (NextGeneration == 0)
        {
            NextGeneration = 1;
        }
        return ActiveGeneration;
    }

    bool FRunLeaseState::IsOwner(uint64 Generation) const
    {
        FScopeLock Lock(&Mutex);
        return Generation != 0 && ActiveGeneration == Generation;
    }

    bool FRunLeaseState::Release(uint64 Generation)
    {
        FScopeLock Lock(&Mutex);
        if (Generation == 0 || ActiveGeneration != Generation)
        {
            return false;
        }

        ActiveGeneration = 0;
        return true;
    }

    FRunLeaseState& GetProcessRunLeaseState()
    {
        static FRunLeaseState State;
        return State;
    }

    double ClampIsolatedChildTimeoutSeconds(double RequestedSeconds)
    {
        if (!FMath::IsFinite(RequestedSeconds))
        {
            return DefaultIsolatedChildTimeoutSeconds;
        }
        return FMath::Clamp(
            RequestedSeconds,
            MinIsolatedChildTimeoutSeconds,
            MaxIsolatedChildTimeoutSeconds);
    }

    EIsolatedChildPollResult PollIsolatedChild(
        double StartedSeconds,
        double TimeoutSeconds,
        double NowSeconds,
        bool bHasExited)
    {
        if (bHasExited)
        {
            return EIsolatedChildPollResult::Exited;
        }
        return NowSeconds - StartedSeconds >= TimeoutSeconds
            ? EIsolatedChildPollResult::TimedOut
            : EIsolatedChildPollResult::Running;
    }

    EFilterRunPollResult PollFilterRun(
        bool bControllerRunning,
        bool& bInOutSawRunning,
        double ElapsedSeconds,
        double StartTimeoutSeconds)
    {
        if (bControllerRunning)
        {
            bInOutSawRunning = true;
            return EFilterRunPollResult::Waiting;
        }
        if (bInOutSawRunning)
        {
            return EFilterRunPollResult::Drained;
        }
        return ElapsedSeconds >= StartTimeoutSeconds
            ? EFilterRunPollResult::NeverStarted
            : EFilterRunPollResult::Waiting;
    }

    void NoteControllerDelegateBound()
    {
        FScopeLock Lock(&ControllerDelegateStatsMutex);
        ++ControllerDelegateStats.Bound;
        ++ControllerDelegateStats.Active;
    }

    void NoteControllerDelegateUnbound()
    {
        FScopeLock Lock(&ControllerDelegateStatsMutex);
        ++ControllerDelegateStats.Unbound;
        ControllerDelegateStats.Active = FMath::Max(0, ControllerDelegateStats.Active - 1);
    }

    FControllerDelegateStats GetControllerDelegateStats()
    {
        FScopeLock Lock(&ControllerDelegateStatsMutex);
        return ControllerDelegateStats;
    }

#if WITH_DEV_AUTOMATION_TESTS
    FScopedControllerJobHold::FScopedControllerJobHold()
    {
        FScopeLock Lock(&ControllerDelegateStatsMutex);
        check(!bHoldControllerJobForTests);
        check(!HeldControllerJobCompletion);
        bHoldControllerJobForTests = true;
    }

    FScopedControllerJobHold::~FScopedControllerJobHold()
    {
        CompleteHeldJob();
        FScopeLock Lock(&ControllerDelegateStatsMutex);
        bHoldControllerJobForTests = false;
        HeldControllerJobCompletion = nullptr;
    }

    bool FScopedControllerJobHold::CompleteHeldJob()
    {
        TFunction<void()> Completion;
        {
            FScopeLock Lock(&ControllerDelegateStatsMutex);
            Completion = MoveTemp(HeldControllerJobCompletion);
        }
        if (!Completion)
        {
            return false;
        }
        Completion();
        return true;
    }

    bool ShouldHoldControllerJobForTests()
    {
        FScopeLock Lock(&ControllerDelegateStatsMutex);
        return bHoldControllerJobForTests;
    }

    void RegisterHeldControllerJobCompletionForTests(TFunction<void()> Completion)
    {
        FScopeLock Lock(&ControllerDelegateStatsMutex);
        check(bHoldControllerJobForTests);
        check(!HeldControllerJobCompletion);
        HeldControllerJobCompletion = MoveTemp(Completion);
    }
#endif

    FLogVerdict EvaluateAutomationLog(const FString& LogText)
    {
        FLogVerdict Result;
        TArray<FString> Lines;
        LogText.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);

        int32 LastTestLine = INDEX_NONE;
        int32 TerminalMarkerLine = INDEX_NONE;
        bool bFatal = false;

        for (int32 LineIndex = 0; LineIndex < Lines.Num(); ++LineIndex)
        {
            const FString& Line = Lines[LineIndex];
            if (Line.Contains(TEXT("Test Started."), ESearchCase::IgnoreCase))
            {
                ++Result.Started;
                LastTestLine = LineIndex;
            }
            if (Line.Contains(TEXT("Test Completed."), ESearchCase::IgnoreCase))
            {
                LastTestLine = LineIndex;
                if (Line.Contains(TEXT("Result={Success}"), ESearchCase::IgnoreCase))
                {
                    ++Result.Succeeded;
                }
                else if (Line.Contains(TEXT("Result={Fail}"), ESearchCase::IgnoreCase))
                {
                    ++Result.Failed;
                }
            }

            int32 Found = 0;
            if (!IsRunTestsArgumentEcho(Line) &&
                Line.Contains(TEXT("LogAutomationCommandLine"), ESearchCase::IgnoreCase) &&
                TryParseRunTestsNumberBetween(Line, TEXT("Found "), TEXT(" automation test"), Found))
            {
                Result.Found = Found;
            }

            int32 Performed = 0;
            if (!IsRunTestsArgumentEcho(Line) &&
                Line.Contains(TEXT("LogAutomationCommandLine"), ESearchCase::IgnoreCase) &&
                TryParseRunTestsNumberBetween(
                    Line,
                    TEXT("Automation Test Queue Empty "),
                    TEXT(" tests performed"),
                    Performed))
            {
                Result.Performed = Performed;
                TerminalMarkerLine = LineIndex;
            }

            bFatal |= Line.Contains(TEXT("Fatal error:"), ESearchCase::IgnoreCase) ||
                      Line.Contains(TEXT("Assertion failed:"), ESearchCase::IgnoreCase) ||
                      Line.Contains(TEXT("Unhandled Exception:"), ESearchCase::IgnoreCase) ||
                      Line.Contains(TEXT("CrashContext runtime-xml"), ESearchCase::IgnoreCase);
            if (Line.Contains(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED"), ESearchCase::CaseSensitive))
            {
                ++Result.Skipped;
            }
        }

        Result.bTerminalMarker = TerminalMarkerLine != INDEX_NONE &&
                                 TerminalMarkerLine > LastTestLine;
        const int32 Finished = Result.Succeeded + Result.Failed;
        Result.bCountsReconciled = Result.Started == Finished &&
                                   Result.Found != INDEX_NONE &&
                                   Result.Performed != INDEX_NONE &&
                                   Result.Performed == Finished &&
                                   Result.Performed == Result.Found;

        if (bFatal)
        {
            Result.Verdict = ELogVerdict::Crashed;
        }
        else if (!Result.bTerminalMarker || !Result.bCountsReconciled)
        {
            Result.Verdict = ELogVerdict::DidNotComplete;
        }
        else if (Result.Performed == 0)
        {
            Result.Verdict = ELogVerdict::NoTests;
        }
        else if (Result.Failed > 0)
        {
            Result.Verdict = ELogVerdict::CompletedWithFailures;
        }
        else if (Result.Skipped > 0)
        {
            Result.Verdict = ELogVerdict::CompletedWithSkips;
        }
        else
        {
            Result.Verdict = ELogVerdict::CompletedClean;
        }
        return Result;
    }

    const TCHAR* LogVerdictToString(ELogVerdict Verdict)
    {
        switch (Verdict)
        {
        case ELogVerdict::CompletedClean:         return TEXT("COMPLETED_CLEAN");
        case ELogVerdict::CompletedWithFailures: return TEXT("COMPLETED_WITH_FAILURES");
        case ELogVerdict::CompletedWithSkips:    return TEXT("COMPLETED_WITH_SKIPS");
        case ELogVerdict::NoTests:               return TEXT("NO_TESTS");
        case ELogVerdict::DidNotComplete:        return TEXT("DID_NOT_COMPLETE");
        case ELogVerdict::Crashed:               return TEXT("CRASHED");
        }
        return TEXT("DID_NOT_COMPLETE");
    }

    bool SplitIsolatedGroups(const FString& Filter, TArray<FString>& OutGroups, FString& OutError)
    {
        OutGroups.Reset();
        OutError.Reset();

        TArray<FString> Groups;
        Filter.ParseIntoArray(Groups, TEXT("+"), /*bCullEmpty=*/false);
        for (FString& Group : Groups)
        {
            Group.TrimStartAndEndInline();
            if (Group.IsEmpty())
            {
                OutError = TEXT("filter contains an empty '+'-separated group");
                return false;
            }
            if (Group.Contains(TEXT(",")) || Group.Contains(TEXT(";")) ||
                Group.Contains(TEXT("\"")))
            {
                OutError = TEXT("isolated group filters must not contain quotes or ExecCmds separators");
                return false;
            }
            for (const TCHAR Character : Group)
            {
                if (FChar::IsControl(Character))
                {
                    OutError = TEXT("isolated group filters must not contain control characters");
                    return false;
                }
            }
            OutGroups.AddUnique(Group);
        }

        if (OutGroups.IsEmpty())
        {
            OutError = TEXT("isolateGroups requires a non-empty filter");
            return false;
        }
        return true;
    }

    FString ResolveCommandletExecutable()
    {
        const FString EditorExecutable = FPlatformProcess::ExecutablePath();
        const FString BaseName = FPaths::GetBaseFilename(EditorExecutable);
        if (BaseName.EndsWith(TEXT("-Cmd"), ESearchCase::IgnoreCase))
        {
            return EditorExecutable;
        }

        const FString Extension = FPaths::GetExtension(EditorExecutable, /*bIncludeDot=*/true);
        const FString Candidate = FPaths::Combine(
            FPaths::GetPath(EditorExecutable), BaseName + TEXT("-Cmd") + Extension);
        return FPaths::FileExists(Candidate) ? Candidate : EditorExecutable;
    }

    FString BuildIsolatedGroupCommandLine(
        const FString& ProjectPath,
        const FString& GroupFilter,
        const FString& GroupLogPath)
    {
        const FString PrivateMonitorPath = FPaths::ChangeExtension(GroupLogPath, TEXT("jobs.jsonl"));
        return FString::Printf(
            TEXT("\"%s\" -ExecCmds=\"Automation RunTests %s,Quit\" ")
            TEXT("-TestExit=\"Automation Test Queue Empty\" -unattended -nopause -nosplash ")
            TEXT("-nosound -RenderOffscreen -nocefaccelpaint -RunningUnattendedScript -Multiprocess ")
            TEXT("-PinWrightIsolatedTestChild=\"%s\" -Abslog=\"%s\""),
            *ProjectPath,
            *GroupFilter,
            *PrivateMonitorPath,
            *GroupLogPath);
    }
}
