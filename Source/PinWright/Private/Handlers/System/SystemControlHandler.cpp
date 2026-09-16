// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ScalabilityConsoleGuard.h"
#include "Handlers/System/RunTestsSupport.h"
#include "PinWrightSubsystem.h"
#include "PinWrightHelpers.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Containers/Ticker.h"
#include "Editor/UnrealEd/Public/Editor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FilterCollection.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/MonitoredProcess.h"
#include "Misc/AutomationTest.h"
#include "IAutomationControllerModule.h"
#include "IAutomationControllerManager.h"
#include "Modules/ModuleManager.h"
#include "Engine/Engine.h"
#if __has_include("Subsystems/UnrealEditorSubsystem.h")
#include "Subsystems/UnrealEditorSubsystem.h"
#elif __has_include("UnrealEditorSubsystem.h")
#include "UnrealEditorSubsystem.h"
#endif
#include "Handlers/BuildTools/ProcPollBind.h"
#include "Handlers/BuildTools/UbtEntryPoint.h"
#include "State/PluginState.h"

namespace
{
    constexpr double RunTestsReadyTimeoutSeconds = 30.0;
    constexpr double RunTestsDiscoveryTimeoutSeconds = 60.0;

    struct FRunTestsRequest
    {
        FString Filter;
        TArray<FString> Tests;
        bool bRunAll = false;
        bool bUsesExactTests = false;
        bool bIsolateGroups = false;
        double ChildTimeoutSeconds = PinWrightRunTests::DefaultIsolatedChildTimeoutSeconds;
    };

    TArray<TSharedPtr<FJsonValue>> MakeStringJsonArray(const TArray<FString>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        Out.Reserve(Values.Num());
        for (const FString& Value : Values)
        {
            Out.Add(MakeShared<FJsonValueString>(Value));
        }
        return Out;
    }

    bool AddUniqueTrimmedTestName(const FString& Raw, TArray<FString>& Tests)
    {
        FString Name = Raw;
        Name.TrimStartAndEndInline();
        if (Name.IsEmpty())
        {
            return false;
        }
        if (!Tests.Contains(Name))
        {
            Tests.Add(Name);
        }
        return true;
    }

    bool ParseRunTestsRequest(FHandlerContext& Ctx, FRunTestsRequest& Out)
    {
        const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
        const bool bHasFilter = Payload.IsValid() && Payload->HasField(TEXT("filter"));
        const bool bHasTest = Payload.IsValid() && Payload->HasField(TEXT("test"));
        const bool bHasTests = Payload.IsValid() && Payload->HasField(TEXT("tests"));
        const bool bHasIsolateGroups = Payload.IsValid() && Payload->HasField(TEXT("isolateGroups"));
        const bool bHasChildTimeout = Payload.IsValid() && Payload->HasField(TEXT("childTimeoutSeconds"));

        if (bHasFilter && !Payload->HasTypedField<EJson::String>(TEXT("filter")))
        {
            Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("filter must be a string"));
            return false;
        }
        if (bHasTest && !Payload->HasTypedField<EJson::String>(TEXT("test")))
        {
            Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("test must be a string"));
            return false;
        }
        if (bHasTests && !Payload->HasTypedField<EJson::Array>(TEXT("tests")))
        {
            Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("tests must be an array of strings"));
            return false;
        }
        if (bHasIsolateGroups && !Payload->HasTypedField<EJson::Boolean>(TEXT("isolateGroups")))
        {
            Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("isolateGroups must be a boolean"));
            return false;
        }
        double RequestedChildTimeoutSeconds = PinWrightRunTests::DefaultIsolatedChildTimeoutSeconds;
        if (bHasChildTimeout &&
            !Ctx.RequireNumber(TEXT("childTimeoutSeconds"), RequestedChildTimeoutSeconds))
        {
            return false;
        }
        if (bHasFilter && (bHasTest || bHasTests))
        {
            Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("filter is mutually exclusive with test/tests"));
            return false;
        }

        if (bHasFilter)
        {
            Out.Filter = Ctx.GetString(TEXT("filter"));
            Out.Filter.TrimStartAndEndInline();
        }

        if (bHasTest)
        {
            const FString TestName = Ctx.GetString(TEXT("test"));
            if (!AddUniqueTrimmedTestName(TestName, Out.Tests))
            {
                Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("test must be a non-empty string"));
                return false;
            }
        }

        if (bHasTests)
        {
            const TArray<TSharedPtr<FJsonValue>>* TestsArray = Ctx.GetArray(TEXT("tests"));
            if (!TestsArray || TestsArray->IsEmpty())
            {
                Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("tests must contain at least one test name"));
                return false;
            }

            for (const TSharedPtr<FJsonValue>& Value : *TestsArray)
            {
                if (!Value.IsValid() || Value->Type != EJson::String)
                {
                    Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("tests must contain only strings"));
                    return false;
                }
                if (!AddUniqueTrimmedTestName(Value->AsString(), Out.Tests))
                {
                    Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("tests must not contain empty strings"));
                    return false;
                }
            }
        }

        Out.bUsesExactTests = Out.Tests.Num() > 0;
        Out.bRunAll = !Out.bUsesExactTests && Out.Filter.IsEmpty();
        Out.bIsolateGroups = Ctx.GetBool(TEXT("isolateGroups"), false);
        Out.ChildTimeoutSeconds =
            PinWrightRunTests::ClampIsolatedChildTimeoutSeconds(RequestedChildTimeoutSeconds);
        if (bHasChildTimeout && !Out.bIsolateGroups)
        {
            Ctx.SendError(
                TEXT("INVALID_PARAMS"),
                TEXT("childTimeoutSeconds is only valid when isolateGroups is true"));
            return false;
        }
        if (Out.bIsolateGroups && (Out.bUsesExactTests || Out.bRunAll))
        {
            Ctx.SendError(
                TEXT("INVALID_PARAMS"),
                TEXT("isolateGroups requires filter mode; it cannot be used with test/tests or RunAll"));
            return false;
        }
        return true;
    }

    TSharedPtr<FJsonObject> MakeRunTestsResult(const TArray<FString>& RequestedTests,
                                               const TArray<FString>& ResolvedTests,
                                               const TArray<FString>& MissingTests,
                                               bool bHasErrors)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("has_errors"), bHasErrors);
        Result->SetArrayField(TEXT("requestedTests"), MakeStringJsonArray(RequestedTests));
        Result->SetArrayField(TEXT("resolvedTests"), MakeStringJsonArray(ResolvedTests));
        Result->SetArrayField(TEXT("missingTests"), MakeStringJsonArray(MissingTests));
        return Result;
    }

    enum class ERunTestsByNameState : uint8
    {
        Initializing,
        WaitingForTests,
        Running,
        Complete
    };

    class FRunAutomationTestsByNameJob
    {
    public:
        FRunAutomationTestsByNameJob(TArray<FString> InRequestedTests,
                                     TSharedRef<FString, ESPMode::ThreadSafe> InTicketIdRef,
                                     uint64 InLeaseGeneration,
                                     FJobOnComplete InOnComplete)
            : RequestedTests(MoveTemp(InRequestedTests))
            , TicketIdRef(MoveTemp(InTicketIdRef))
            , LeaseGeneration(InLeaseGeneration)
            , OnComplete(MoveTemp(InOnComplete))
        {
            IAutomationControllerModule& Module =
                FModuleManager::LoadModuleChecked<IAutomationControllerModule>(TEXT("AutomationController"));
            Controller = Module.GetAutomationController();
        }

        void Start(TSharedRef<FRunAutomationTestsByNameJob> Self)
        {
            if (!PinWrightRunTests::GetProcessRunLeaseState().IsOwner(LeaseGeneration))
            {
                Finish(false, nullptr, TEXT("AUTOMATION_RUN_IN_PROGRESS"));
                return;
            }

#if WITH_DEV_AUTOMATION_TESTS
            const bool bHoldForTest = PinWrightRunTests::ShouldHoldControllerJobForTests();
            if (!bHoldForTest)
#endif
            {
                Controller->Init();
            }
            TestsRefreshedHandle = Controller->OnTestsRefreshed().AddLambda(
                [Self]()
                {
                    Self->HandleTestsRefreshed();
                });
            PinWrightRunTests::NoteControllerDelegateBound();
            TestsCompleteHandle = Controller->OnTestsComplete().AddLambda(
                [Self]()
                {
                    Self->HandleTestsComplete();
                });
            PinWrightRunTests::NoteControllerDelegateBound();
#if WITH_DEV_AUTOMATION_TESTS
            if (bHoldForTest)
            {
                PinWrightRunTests::RegisterHeldControllerJobCompletionForTests(
                    [Self]()
                    {
                        TSharedPtr<FJsonObject> Result = MakeRunTestsResult(
                            Self->RequestedTests,
                            Self->RequestedTests,
                            TArray<FString>(),
                            /*bHasErrors=*/false);
                        Self->Finish(true, Result, FString());
                    });
                return;
            }
#endif
            TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
                FTickerDelegate::CreateLambda(
                    [Self](float DeltaTime)
                    {
                        return Self->Tick(DeltaTime);
                    }));
        }

    private:
        bool Tick(float DeltaTime)
        {
            if (State == ERunTestsByNameState::Complete)
            {
                return false;
            }
            if (!PinWrightRunTests::GetProcessRunLeaseState().IsOwner(LeaseGeneration))
            {
                Finish(false, nullptr, TEXT("AUTOMATION_RUN_IN_PROGRESS"));
                return false;
            }

            ElapsedSeconds += DeltaTime;
            Controller->Tick();

            if (State == ERunTestsByNameState::Initializing)
            {
                if (Controller->IsReadyForTests())
                {
                    Controller->RequestAvailableWorkers(FApp::GetSessionId());
                    State = ERunTestsByNameState::WaitingForTests;
                    ElapsedSeconds = 0.0;
                }
                else if (ElapsedSeconds >= RunTestsReadyTimeoutSeconds)
                {
                    TSharedPtr<FJsonObject> Result = MakeRunTestsResult(
                        RequestedTests, ResolvedTests, MissingTests, /*bHasErrors=*/true);
                    Finish(false, Result, TEXT("AUTOMATION_NOT_READY"));
                    return false;
                }
            }
            else if (State == ERunTestsByNameState::WaitingForTests &&
                     ElapsedSeconds >= RunTestsDiscoveryTimeoutSeconds)
            {
                TSharedPtr<FJsonObject> Result = MakeRunTestsResult(
                    RequestedTests, ResolvedTests, MissingTests, /*bHasErrors=*/true);
                Finish(false, Result, TEXT("TEST_DISCOVERY_TIMEOUT"));
                return false;
            }

            return State != ERunTestsByNameState::Complete;
        }

        void HandleTestsRefreshed()
        {
            if (State != ERunTestsByNameState::WaitingForTests ||
                !PinWrightRunTests::GetProcessRunLeaseState().IsOwner(LeaseGeneration) ||
                Controller->GetNumDeviceClusters() == 0)
            {
                return;
            }

            TSharedPtr<AutomationFilterCollection> AutomationFilters =
                MakeShareable(new AutomationFilterCollection());
            Controller->SetFilter(AutomationFilters);
            Controller->SetVisibleTestsEnabled(true);

            TArray<FString> AvailableTests;
            Controller->GetEnabledTestNames(AvailableTests);

            TSet<FString> AvailableSet;
            AvailableSet.Reserve(AvailableTests.Num());
            for (const FString& AvailableTest : AvailableTests)
            {
                AvailableSet.Add(AvailableTest);
            }

            ResolvedTests.Reset();
            MissingTests.Reset();
            for (const FString& RequestedTest : RequestedTests)
            {
                if (AvailableSet.Contains(RequestedTest))
                {
                    ResolvedTests.Add(RequestedTest);
                }
                else
                {
                    MissingTests.Add(RequestedTest);
                }
            }

            if (ResolvedTests.IsEmpty())
            {
                Controller->SetEnabledTests(ResolvedTests);
                TSharedPtr<FJsonObject> Result = MakeRunTestsResult(
                    RequestedTests, ResolvedTests, MissingTests, /*bHasErrors=*/true);
                Finish(false, Result, TEXT("NO_TESTS_MATCHED"));
                return;
            }

            Controller->StopTests();
            Controller->SetEnabledTests(ResolvedTests);
            State = ERunTestsByNameState::Running;
            ElapsedSeconds = 0.0;
            Controller->RunTests();

            // Milestone progress: discovery finished, the run itself begins now.
            // TicketIdRef is filled by the handler right after Ctx.StartJob returns
            // (same game-thread turn), before any controller delegate can fire.
            if (!TicketIdRef->IsEmpty())
            {
                auto Progress = MakeShared<FJsonObject>();
                Progress->SetNumberField(TEXT("resolvedTests"), ResolvedTests.Num());
                Progress->SetNumberField(TEXT("missingTests"), MissingTests.Num());
                FPluginState::Get().GetJobRegistry().RecordProgress(
                    *TicketIdRef,
                    FString::Printf(TEXT("resolved %d test(s), running"), ResolvedTests.Num()),
                    Progress, /*bBypassRateLimit=*/true);
            }
        }

        void HandleTestsComplete()
        {
            if (State != ERunTestsByNameState::Running)
            {
                return;
            }
            if (!PinWrightRunTests::GetProcessRunLeaseState().IsOwner(LeaseGeneration))
            {
                Finish(false, nullptr, TEXT("AUTOMATION_RUN_IN_PROGRESS"));
                return;
            }

            const bool bHasErrors = Controller->ReportsHaveErrors();
            TSharedPtr<FJsonObject> Result = MakeRunTestsResult(
                RequestedTests, ResolvedTests, MissingTests, bHasErrors);
            Finish(!bHasErrors, Result, bHasErrors ? TEXT("TESTS_FAILED") : FString());
        }

        void Finish(bool bSuccess, TSharedPtr<FJsonObject> Result, const FString& Error)
        {
            if (State == ERunTestsByNameState::Complete)
            {
                return;
            }
            State = ERunTestsByNameState::Complete;

            if (TestsRefreshedHandle.IsValid())
            {
                Controller->OnTestsRefreshed().Remove(TestsRefreshedHandle);
                TestsRefreshedHandle.Reset();
                PinWrightRunTests::NoteControllerDelegateUnbound();
            }
            if (TestsCompleteHandle.IsValid())
            {
                Controller->OnTestsComplete().Remove(TestsCompleteHandle);
                TestsCompleteHandle.Reset();
                PinWrightRunTests::NoteControllerDelegateUnbound();
            }
            if (TickerHandle.IsValid())
            {
                FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
                TickerHandle.Reset();
            }
            FJobOnComplete Completion = MoveTemp(OnComplete);
            Completion(bSuccess, Result, Error);
            PinWrightRunTests::GetProcessRunLeaseState().Release(LeaseGeneration);
        }

        IAutomationControllerManagerPtr Controller;
        TArray<FString> RequestedTests;
        TArray<FString> ResolvedTests;
        TArray<FString> MissingTests;
        // Job ticket for progress events; owned as a shared string because the
        // handler learns the ticket id only after StartJob (which invokes the
        // bind delegate that constructs this job) has returned.
        TSharedRef<FString, ESPMode::ThreadSafe> TicketIdRef;
        uint64 LeaseGeneration = 0;
        FJobOnComplete OnComplete;
        FDelegateHandle TestsRefreshedHandle;
        FDelegateHandle TestsCompleteHandle;
        FTSTicker::FDelegateHandle TickerHandle;
        ERunTestsByNameState State = ERunTestsByNameState::Initializing;
        double ElapsedSeconds = 0.0;
    };

    class FRunAutomationTestsByFilterJob
    {
    public:
        FRunAutomationTestsByFilterJob(
            FString InCommand,
            uint64 InLeaseGeneration,
            FJobOnComplete InOnComplete)
            : Command(MoveTemp(InCommand))
            , LeaseGeneration(InLeaseGeneration)
            , OnComplete(MoveTemp(InOnComplete))
        {
            IAutomationControllerModule& Module =
                FModuleManager::LoadModuleChecked<IAutomationControllerModule>(TEXT("AutomationController"));
            Controller = Module.GetAutomationController();
        }

        void Start(TSharedRef<FRunAutomationTestsByFilterJob> Self)
        {
            if (!PinWrightRunTests::GetProcessRunLeaseState().IsOwner(LeaseGeneration))
            {
                Finish(false, nullptr, TEXT("AUTOMATION_RUN_IN_PROGRESS"));
                return;
            }

            TestsCompleteHandle = Controller->OnTestsComplete().AddLambda(
                [Self]()
                {
                    Self->HandleTestsComplete();
                });
            PinWrightRunTests::NoteControllerDelegateBound();

#if WITH_DEV_AUTOMATION_TESTS
            if (PinWrightRunTests::ShouldHoldControllerJobForTests())
            {
                PinWrightRunTests::RegisterHeldControllerJobCompletionForTests(
                    [Self]()
                    {
                        auto Result = MakeShared<FJsonObject>();
                        Result->SetBoolField(TEXT("has_errors"), false);
                        Self->Finish(true, Result, FString());
                    });
                return;
            }
#endif

            if (!GEngine)
            {
                Finish(false, nullptr, TEXT("AUTOMATION_ERROR"));
                return;
            }
            GEngine->Exec(nullptr, *Command);
        }

    private:
        void HandleTestsComplete()
        {
            if (bComplete)
            {
                return;
            }
            if (!PinWrightRunTests::GetProcessRunLeaseState().IsOwner(LeaseGeneration))
            {
                Finish(false, nullptr, TEXT("AUTOMATION_RUN_IN_PROGRESS"));
                return;
            }

            const bool bHasErrors = Controller->ReportsHaveErrors();
            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("has_errors"), bHasErrors);
            Finish(!bHasErrors, Result, bHasErrors ? TEXT("TESTS_FAILED") : FString());
        }

        void Finish(bool bSuccess, TSharedPtr<FJsonObject> Result, const FString& Error)
        {
            if (bComplete)
            {
                return;
            }
            bComplete = true;

            if (TestsCompleteHandle.IsValid())
            {
                Controller->OnTestsComplete().Remove(TestsCompleteHandle);
                TestsCompleteHandle.Reset();
                PinWrightRunTests::NoteControllerDelegateUnbound();
            }

            FJobOnComplete Completion = MoveTemp(OnComplete);
            Completion(bSuccess, Result, Error);
            PinWrightRunTests::GetProcessRunLeaseState().Release(LeaseGeneration);
        }

        FString Command;
        uint64 LeaseGeneration = 0;
        FJobOnComplete OnComplete;
        IAutomationControllerManagerPtr Controller;
        FDelegateHandle TestsCompleteHandle;
        bool bComplete = false;
    };

    class FRunAutomationGroupsIsolatedJob
    {
    public:
        FRunAutomationGroupsIsolatedJob(
            TArray<FString> InGroups,
            FString InRunDirectory,
            uint64 InLeaseGeneration,
            double InChildTimeoutSeconds)
            : Groups(MoveTemp(InGroups))
            , RunDirectory(MoveTemp(InRunDirectory))
            , LeaseGeneration(InLeaseGeneration)
            , ChildTimeoutSeconds(InChildTimeoutSeconds)
            , ExecutablePath(PinWrightRunTests::ResolveCommandletExecutable())
            , ProjectPath(FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()))
        {
        }

        void Start(
            TSharedRef<FRunAutomationGroupsIsolatedJob> Self,
            FJobOnComplete InOnComplete)
        {
            OnComplete = MoveTemp(InOnComplete);
            if (!PinWrightRunTests::GetProcessRunLeaseState().IsOwner(LeaseGeneration))
            {
                Finish(false, TEXT("AUTOMATION_RUN_IN_PROGRESS"));
                return;
            }
            if (ProjectPath.IsEmpty() || !FPaths::FileExists(ProjectPath) ||
                !FPaths::FileExists(ExecutablePath) ||
                !IFileManager::Get().MakeDirectory(*RunDirectory, /*Tree=*/true))
            {
                Finish(false, TEXT("AUTOMATION_ERROR"));
                return;
            }
            if (!LaunchCurrentGroup())
            {
                Finish(false, TEXT("AUTOMATION_ERROR"));
                return;
            }

            TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
                FTickerDelegate::CreateLambda(
                    [Self](float)
                    {
                        return Self->Tick();
                    }),
                0.5f);
        }

        void Cancel()
        {
            if (bComplete)
            {
                return;
            }
            TerminateCurrentChild();
            Finish(false, TEXT("CANCELLED"));
        }

    private:
        bool LaunchCurrentGroup()
        {
            CurrentLogPath = FPaths::Combine(
                RunDirectory,
                FString::Printf(TEXT("group-%03d.log"), GroupIndex + 1));
            const FString Arguments = PinWrightRunTests::BuildIsolatedGroupCommandLine(
                ProjectPath, Groups[GroupIndex], CurrentLogPath);

            uint32 ProcessId = 0;
            ProcessHandle = FPlatformProcess::CreateProc(
                *ExecutablePath,
                *Arguments,
                /*bLaunchDetached=*/false,
                /*bLaunchHidden=*/true,
                /*bLaunchReallyHidden=*/true,
                &ProcessId,
                0,
                nullptr,
                nullptr);
            if (ProcessHandle.IsValid())
            {
                ChildStartedSeconds = FPlatformTime::Seconds();
                return true;
            }

            auto GroupResult = MakeShared<FJsonObject>();
            GroupResult->SetStringField(TEXT("filter"), Groups[GroupIndex]);
            GroupResult->SetStringField(TEXT("logPath"), CurrentLogPath);
            GroupResult->SetStringField(TEXT("verdict"), TEXT("PROCESS_LAUNCH_FAILED"));
            GroupResults.Add(MakeShared<FJsonValueObject>(GroupResult));
            return false;
        }

        bool Tick()
        {
            if (bComplete)
            {
                return false;
            }
            if (!PinWrightRunTests::GetProcessRunLeaseState().IsOwner(LeaseGeneration))
            {
                Finish(false, TEXT("AUTOMATION_RUN_IN_PROGRESS"));
                return false;
            }

            int32 ReturnCode = 0;
            bool bHasExited = FPlatformProcess::GetProcReturnCode(ProcessHandle, &ReturnCode);
            if (!bHasExited && !FPlatformProcess::IsProcRunning(ProcessHandle))
            {
                ReturnCode = -1;
                bHasExited = true;
            }
            const double NowSeconds = FPlatformTime::Seconds();
            const PinWrightRunTests::EIsolatedChildPollResult PollResult =
                PinWrightRunTests::PollIsolatedChild(
                    ChildStartedSeconds,
                    ChildTimeoutSeconds,
                    NowSeconds,
                    bHasExited);
            if (PollResult == PinWrightRunTests::EIsolatedChildPollResult::Running)
            {
                return true;
            }
            if (PollResult == PinWrightRunTests::EIsolatedChildPollResult::TimedOut)
            {
                bTimedOut = true;
                TimedOutGroup = Groups[GroupIndex];
                TimedOutGroupIndex = GroupIndex;
                const double ElapsedSeconds = FMath::Max(0.0, NowSeconds - ChildStartedSeconds);
                TerminateCurrentChild();
                AddTimedOutGroupResult(ElapsedSeconds);
                Finish(false, TEXT("TEST_RUN_TIMEOUT"));
                return false;
            }

            FPlatformProcess::CloseProc(ProcessHandle);
            ProcessHandle.Reset();

            FString LogText;
            if (!FFileHelper::LoadFileToString(LogText, *CurrentLogPath))
            {
                AddGroupResult(ReturnCode, PinWrightRunTests::FLogVerdict());
                Finish(false, TEXT("AUTOMATION_ERROR"));
                return false;
            }

            const PinWrightRunTests::FLogVerdict Verdict =
                PinWrightRunTests::EvaluateAutomationLog(LogText);
            AddGroupResult(ReturnCode, Verdict);
            if (Verdict.Verdict != PinWrightRunTests::ELogVerdict::CompletedClean)
            {
                FString Error = TEXT("TEST_RUN_INCOMPLETE");
                if (Verdict.Verdict == PinWrightRunTests::ELogVerdict::CompletedWithFailures)
                {
                    Error = TEXT("TESTS_FAILED");
                }
                else if (Verdict.Verdict == PinWrightRunTests::ELogVerdict::NoTests)
                {
                    Error = TEXT("NO_TESTS_MATCHED");
                }
                else if (Verdict.Verdict == PinWrightRunTests::ELogVerdict::CompletedWithSkips)
                {
                    Error = TEXT("TESTS_SKIPPED");
                }
                Finish(false, Error);
                return false;
            }
            if (ReturnCode != 0)
            {
                Finish(false, TEXT("AUTOMATION_ERROR"));
                return false;
            }

            ++GroupIndex;
            if (GroupIndex == Groups.Num())
            {
                Finish(true, FString());
                return false;
            }
            if (!LaunchCurrentGroup())
            {
                Finish(false, TEXT("AUTOMATION_ERROR"));
                return false;
            }
            return true;
        }

        void AddGroupResult(int32 ReturnCode, const PinWrightRunTests::FLogVerdict& Verdict)
        {
            auto GroupResult = MakeShared<FJsonObject>();
            GroupResult->SetStringField(TEXT("filter"), Groups[GroupIndex]);
            GroupResult->SetStringField(TEXT("logPath"), CurrentLogPath);
            GroupResult->SetNumberField(TEXT("exitCode"), ReturnCode);
            GroupResult->SetStringField(
                TEXT("verdict"), PinWrightRunTests::LogVerdictToString(Verdict.Verdict));
            GroupResult->SetNumberField(TEXT("found"), Verdict.Found);
            GroupResult->SetNumberField(TEXT("started"), Verdict.Started);
            GroupResult->SetNumberField(TEXT("succeeded"), Verdict.Succeeded);
            GroupResult->SetNumberField(TEXT("failed"), Verdict.Failed);
            GroupResult->SetNumberField(TEXT("skipped"), Verdict.Skipped);
            GroupResult->SetNumberField(TEXT("performed"), Verdict.Performed);
            GroupResult->SetBoolField(TEXT("terminalMarker"), Verdict.bTerminalMarker);
            GroupResult->SetBoolField(TEXT("countsReconciled"), Verdict.bCountsReconciled);
            GroupResults.Add(MakeShared<FJsonValueObject>(GroupResult));
        }

        void AddTimedOutGroupResult(double ElapsedSeconds)
        {
            auto GroupResult = MakeShared<FJsonObject>();
            GroupResult->SetStringField(TEXT("filter"), Groups[GroupIndex]);
            GroupResult->SetStringField(TEXT("logPath"), CurrentLogPath);
            GroupResult->SetStringField(TEXT("verdict"), TEXT("TIMED_OUT"));
            GroupResult->SetBoolField(TEXT("timedOut"), true);
            GroupResult->SetNumberField(TEXT("timeoutSeconds"), ChildTimeoutSeconds);
            GroupResult->SetNumberField(TEXT("elapsedSeconds"), ElapsedSeconds);
            GroupResults.Add(MakeShared<FJsonValueObject>(GroupResult));
        }

        TSharedPtr<FJsonObject> MakeResult(bool bHasErrors) const
        {
            auto Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("has_errors"), bHasErrors);
            Result->SetBoolField(TEXT("isolatedGroups"), true);
            Result->SetStringField(TEXT("runDirectory"), RunDirectory);
            Result->SetNumberField(TEXT("requestedGroups"), Groups.Num());
            Result->SetNumberField(TEXT("processedGroups"), GroupResults.Num());
            Result->SetNumberField(TEXT("successfulGroups"), GroupIndex);
            Result->SetNumberField(TEXT("childTimeoutSeconds"), ChildTimeoutSeconds);
            Result->SetBoolField(TEXT("timedOut"), bTimedOut);
            if (bTimedOut)
            {
                Result->SetStringField(TEXT("timedOutGroup"), TimedOutGroup);
                Result->SetNumberField(TEXT("timedOutGroupIndex"), TimedOutGroupIndex + 1);
            }
            Result->SetArrayField(TEXT("groups"), GroupResults);
            return Result;
        }

        void TerminateCurrentChild()
        {
            if (!ProcessHandle.IsValid())
            {
                return;
            }
            if (FPlatformProcess::IsProcRunning(ProcessHandle))
            {
                FPlatformProcess::TerminateProc(ProcessHandle, /*KillTree=*/true);
            }
            FPlatformProcess::CloseProc(ProcessHandle);
            ProcessHandle.Reset();
        }

        void Finish(bool bSuccess, const FString& Error)
        {
            if (bComplete)
            {
                return;
            }
            bComplete = true;

            if (TickerHandle.IsValid())
            {
                FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
                TickerHandle.Reset();
            }
            TerminateCurrentChild();

            FJobOnComplete Completion = MoveTemp(OnComplete);
            Completion(bSuccess, MakeResult(!bSuccess), Error);
            PinWrightRunTests::GetProcessRunLeaseState().Release(LeaseGeneration);
        }

        TArray<FString> Groups;
        FString RunDirectory;
        uint64 LeaseGeneration = 0;
        double ChildTimeoutSeconds = PinWrightRunTests::DefaultIsolatedChildTimeoutSeconds;
        FJobOnComplete OnComplete;
        FString ExecutablePath;
        FString ProjectPath;
        FString CurrentLogPath;
        FProcHandle ProcessHandle;
        TArray<TSharedPtr<FJsonValue>> GroupResults;
        FTSTicker::FDelegateHandle TickerHandle;
        int32 GroupIndex = 0;
        int32 TimedOutGroupIndex = INDEX_NONE;
        double ChildStartedSeconds = 0.0;
        FString TimedOutGroup;
        bool bTimedOut = false;
        bool bComplete = false;
    };
}

// ---- system.run_ubt ----
REGISTER_RPC_HANDLER("system.run_ubt", "system", "Spawn Unreal Build Tool as a child process and capture stdout/stderr. Long-running; runs as a job. Useful for scripted hot-recompile from outside the editor.",
    RPC_PARAMS(
        RPC_PARAM_OPT("target", "string", "UBT target name (e.g. 'MyProjectEditor', 'MyProject'). Defaults to the current editor target."),
        RPC_PARAM_OPT("platform", "string", "Target platform (e.g. 'Win64'). Defaults to the host platform."),
        RPC_PARAM_OPT("configuration", "string", "Build configuration: 'Development', 'Shipping', 'DebugGame', etc. Defaults to 'Development'."),
        RPC_PARAM_OPT("additionalArgs", "string", "Extra command-line arguments appended verbatim to the UBT invocation.")
    ))
{
    FString Target = Ctx.GetString(TEXT("target"));
    FString Platform = Ctx.GetString(TEXT("platform"));
    FString Configuration = Ctx.GetString(TEXT("configuration"));
    FString AdditionalArgs = Ctx.GetString(TEXT("additionalArgs"));

    // Resolve the real per-platform UBT entry point (Build.bat on Windows /
    // Build.sh elsewhere) via the shared resolver in Handlers/BuildTools/UbtEntryPoint.h.
    const FString UBTPath = EARG_Ubt::ResolveUbtEntryPoint();

    if (!FPaths::FileExists(UBTPath))
    {
        Ctx.SendError(TEXT("UBT_NOT_FOUND"), FString::Printf(TEXT("UBT not found at: %s"), *UBTPath));
        return true;
    }

    // Build command line arguments
    FString Arguments;

    if (!Target.IsEmpty())
    {
        Arguments += Target + TEXT(" ");
    }
    else
    {
        FString ProjectPath = FPaths::GetProjectFilePath();
        if (!ProjectPath.IsEmpty())
        {
            Arguments += FString::Printf(TEXT("-project=\"%s\" "), *ProjectPath);
        }
    }

    if (!Platform.IsEmpty())
    {
        Arguments += Platform + TEXT(" ");
    }
    else
    {
#if PLATFORM_WINDOWS
        Arguments += TEXT("Win64 ");
#elif PLATFORM_MAC
        Arguments += TEXT("Mac ");
#else
        Arguments += TEXT("Linux ");
#endif
    }

    if (!Configuration.IsEmpty())
    {
        Arguments += Configuration + TEXT(" ");
    }
    else
    {
        Arguments += TEXT("Development ");
    }

    if (!AdditionalArgs.IsEmpty())
    {
        Arguments += AdditionalArgs;
    }

    FJobBindArgs Args;
    Args.Method = TEXT("system.run_ubt");
    Args.StartedPayload = MakeShared<FJsonObject>();
    Args.StartedPayload->SetStringField(TEXT("ubtPath"), UBTPath);
    Args.StartedPayload->SetStringField(TEXT("arguments"), Arguments);

    Args.BindNativeDelegate =
        [UBTPath, Arguments](FJobOnComplete OnComplete)
    {
        BindProcPollCompletion(UBTPath, Arguments, OnComplete);
    };

    const FString TicketId = Ctx.StartJob(Args);

    // BindProcPollCompletion polls only the child's exit code (no output pipe),
    // so the job has no mid-run observation point. Emit a ~10s heartbeat with
    // the cheap observable state we do have: elapsed time and the size of UBT's
    // own log file, which grows while the build makes progress. RecordProgress
    // returns false once the ticket leaves "running" (completed / failed /
    // cancelled), which self-removes the ticker on every terminal path.
    const FString UbtLogPath = FPaths::Combine(
        FPaths::EngineDir(), TEXT("Programs/UnrealBuildTool/Log.txt"));
    FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda(
            [TicketId, UbtLogPath, StartSeconds = FPlatformTime::Seconds()](float) -> bool
    {
        const int32 Elapsed = FMath::RoundToInt(FPlatformTime::Seconds() - StartSeconds);
        auto Progress = MakeShared<FJsonObject>();
        Progress->SetNumberField(TEXT("elapsedSeconds"), Elapsed);
        const int64 LogBytes = IFileManager::Get().FileSize(*UbtLogPath);
        if (LogBytes >= 0)
        {
            Progress->SetNumberField(TEXT("ubtLogBytes"), static_cast<double>(LogBytes));
        }
        return FPluginState::Get().GetJobRegistry().RecordProgress(
            TicketId,
            FString::Printf(TEXT("UBT running, %ds elapsed"), Elapsed),
            Progress, /*bBypassRateLimit=*/true);
    }), 10.0f);
    return true;
}

// ---- system.run_tests ----
REGISTER_RPC_HANDLER("system.run_tests", "system", "Run UE automation tests and report pass/fail counts. Long-running; runs as a job. Pass exact test name(s) via test/tests or a broad filter pattern. Set isolateGroups for a fail-closed process boundary per '+'-separated filter group.",
    RPC_PARAMS(
        RPC_PARAM_OPT("filter", "string", "Substring filter applied to test names. Use 'PinWright' / 'EditorTests' to scope to project tests."),
        RPC_PARAM_OPT("test", "string", "Exact name of a single automation test to run. Mutually exclusive with filter."),
        RPC_PARAM_OPT("tests", "array", "Exact names of automation tests to run. Mutually exclusive with filter; test is a single-name alias."),
        RPC_PARAM_DEF("isolateGroups", "boolean", "When true in filter mode, split '+'-joined groups and run each sequentially in its own editor process. Every child log must carry a real terminal queue-drain marker and reconciled counts.", "false"),
        RPC_PARAM_DEF("childTimeoutSeconds", "number", "Wall-clock budget for each isolated child. Used only with isolateGroups; clamped to 1-14400 seconds and defaults to 3600. An expired child process tree is terminated and the job reports timedOut plus the group.", "3600")
    ))
{
    FRunTestsRequest Request;
    if (!ParseRunTestsRequest(Ctx, Request))
    {
        return true;
    }

    TArray<FString> IsolatedGroups;
    if (Request.bIsolateGroups)
    {
        FString SplitError;
        if (!PinWrightRunTests::SplitIsolatedGroups(Request.Filter, IsolatedGroups, SplitError))
        {
            Ctx.SendError(TEXT("INVALID_PARAMS"), SplitError);
            return true;
        }
    }

    const uint64 LeaseGeneration =
        PinWrightRunTests::GetProcessRunLeaseState().TryAcquire();
    if (LeaseGeneration == 0)
    {
        Ctx.SendError(
            TEXT("AUTOMATION_RUN_IN_PROGRESS"),
            TEXT("Another system.run_tests job owns the automation controller; wait for its terminal status"));
        return true;
    }

    const FString Cmd = Request.Filter.IsEmpty()
        ? TEXT("Automation RunAll")
        : FString::Printf(TEXT("Automation RunTests %s"), *Request.Filter);

    FJobBindArgs Args;
    Args.Method = TEXT("system.run_tests");
    Args.StartedPayload = MakeShared<FJsonObject>();

    // Filled with the job ticket id right after Ctx.StartJob returns, so the
    // exact-tests job (constructed inside the bind delegate, i.e. before the
    // ticket id exists) can emit progress events against the right ticket.
    TSharedRef<FString, ESPMode::ThreadSafe> TicketIdRef =
        MakeShared<FString, ESPMode::ThreadSafe>();
    TSharedPtr<FRunAutomationGroupsIsolatedJob> IsolatedJob;

    if (Request.bIsolateGroups)
    {
        const FString RunDirectory = FPaths::ConvertRelativePathToFull(FPaths::Combine(
            FPaths::ProjectSavedDir(),
            TEXT("PinWright/test-runs"),
            FGuid::NewGuid().ToString(EGuidFormats::Digits)));
        Args.StartedPayload->SetStringField(TEXT("selectionMode"), TEXT("isolatedGroups"));
        Args.StartedPayload->SetBoolField(TEXT("isolateGroups"), true);
        Args.StartedPayload->SetBoolField(TEXT("cancellable"), true);
        Args.StartedPayload->SetNumberField(
            TEXT("childTimeoutSeconds"), Request.ChildTimeoutSeconds);
        Args.StartedPayload->SetStringField(TEXT("runDirectory"), RunDirectory);
        Args.StartedPayload->SetArrayField(TEXT("groups"), MakeStringJsonArray(IsolatedGroups));
        IsolatedJob = MakeShared<FRunAutomationGroupsIsolatedJob>(
            MoveTemp(IsolatedGroups),
            RunDirectory,
            LeaseGeneration,
            Request.ChildTimeoutSeconds);
        Args.BindNativeDelegate =
            [IsolatedJob](FJobOnComplete OnComplete)
        {
            IsolatedJob->Start(IsolatedJob.ToSharedRef(), MoveTemp(OnComplete));
        };
    }
    else if (Request.bUsesExactTests)
    {
        Args.StartedPayload->SetStringField(TEXT("selectionMode"), TEXT("tests"));
        Args.StartedPayload->SetArrayField(TEXT("requestedTests"), MakeStringJsonArray(Request.Tests));
        TArray<FString> Tests = Request.Tests;
        Args.BindNativeDelegate =
            [Tests = MoveTemp(Tests), TicketIdRef, LeaseGeneration](FJobOnComplete OnComplete) mutable
        {
            TSharedRef<FRunAutomationTestsByNameJob> Job =
                MakeShared<FRunAutomationTestsByNameJob>(
                    MoveTemp(Tests), TicketIdRef, LeaseGeneration, MoveTemp(OnComplete));
            Job->Start(Job);
        };
    }
    else
    {
        Args.StartedPayload->SetStringField(TEXT("selectionMode"), Request.bRunAll ? TEXT("all") : TEXT("filter"));
        Args.StartedPayload->SetStringField(TEXT("filter"), Request.Filter);
        Args.StartedPayload->SetStringField(TEXT("command"), Cmd);

        Args.BindNativeDelegate =
            [Cmd, LeaseGeneration](FJobOnComplete OnComplete)
        {
            TSharedRef<FRunAutomationTestsByFilterJob> Job =
                MakeShared<FRunAutomationTestsByFilterJob>(
                    Cmd, LeaseGeneration, MoveTemp(OnComplete));
            Job->Start(Job);
        };
    }

    const FString TicketId = Ctx.StartJob(Args);
    *TicketIdRef = TicketId;
    if (IsolatedJob.IsValid())
    {
        TWeakPtr<FRunAutomationGroupsIsolatedJob> WeakJob(IsolatedJob);
        FPluginState::Get().GetJobRegistry().SetCancelCallback(
            TicketId,
            [WeakJob]()
            {
                if (const TSharedPtr<FRunAutomationGroupsIsolatedJob> Job = WeakJob.Pin())
                {
                    Job->Cancel();
                }
            });
    }

    // Emit a ~10s heartbeat while any selection mode owns the lease. In-process
    // automation also publishes the current test via FAutomationTestFramework;
    // isolated children report their per-group evidence when each process exits.
    // RecordProgress returns false once the ticket is terminal, which self-removes
    // the ticker.
    FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda(
            [TicketId, StartSeconds = FPlatformTime::Seconds()](float) -> bool
    {
        const int32 Elapsed = FMath::RoundToInt(FPlatformTime::Seconds() - StartSeconds);
        auto Progress = MakeShared<FJsonObject>();
        Progress->SetNumberField(TEXT("elapsedSeconds"), Elapsed);
        FString Message = FString::Printf(TEXT("tests running, %ds elapsed"), Elapsed);
        if (FAutomationTestBase* CurrentTest = FAutomationTestFramework::Get().GetCurrentTest())
        {
            const FString CurrentName = CurrentTest->GetTestFullName();
            Progress->SetStringField(TEXT("currentTest"), CurrentName);
            Message = FString::Printf(TEXT("running %s, %ds elapsed"), *CurrentName, Elapsed);
        }
        return FPluginState::Get().GetJobRegistry().RecordProgress(
            TicketId, Message, Progress, /*bBypassRateLimit=*/true);
    }), 10.0f);
    return true;
}

// ---- system.console_command ----
REGISTER_RPC_HANDLER("system.console_command", "system", "Run a console command at the process / GEngine scope. Distinct from editor.console_command which targets the editor world; use this for project / engine-wide commands. A line that SETS a scalability CVar is REFUSED with SCALABILITY_CVAR_USE_TYPED_VERB — either an 'sg.*' group, or any CVar carrying ECVF_Scalability / ECVF_ScalabilityGroup (r.ViewDistanceScale, r.Streaming.PoolSize, r.ScreenPercentage, r.MaxAnisotropy, ...), because a console set pins it at ECVF_SetByConsole above the ECVF_SetByScalability priority the editor's own Settings > Engine Scalability Settings panel writes at, for the rest of the session. Use performance.set_scalability, or pass force:true to accept the pin. READING such a CVar (its name with no value) is not refused, and neither is the aggregate 'scalability N', which routes through Scalability::SetQualityLevels at the panel's own priority.",
    RPC_PARAMS(
        RPC_PARAM_REQ("command", "string", "Full console command line including arguments, e.g. 'log LogStreaming Verbose' or 'stat unit'."),
        RPC_PARAM_DEF("force", "boolean", "Run a scalability-CVar set anyway ('sg.<Group> N' or any ECVF_Scalability CVar), accepting that the CVar is pinned at ECVF_SetByConsole and the editor's own Scalability panel can no longer change its group until the editor restarts. Ignored for every other command.", "false")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();
    if (!Payload)
    {
        Ctx.SendError(TEXT("INVALID_PAYLOAD"), TEXT("system.console_command payload missing."));
        return true;
    }

    // Top-level 'command' only. The nested params.command envelope this used to fall back
    // to was unreachable twice over: 'params' is not a declared parameter, and 'command' is
    // required, so a request carrying only the envelope is refused by the dispatcher's
    // required-param gate before the body runs.
    FString Cmd;
    Payload->TryGetStringField(TEXT("command"), Cmd);
    if (Cmd.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("command required"));
        return true;
    }

    // A console set of ANY scalability-flagged cvar — the sg.* group or one of the ordinary
    // r.* members the group's ini section drives — pins it at ECVF_SetByConsole for the life of
    // the process, permanently outranking the editor's own Scalability panel: a side effect that
    // outlives this call and is not this caller's to spend. Refused in favour of the typed verb
    // that writes at the panel's own priority; see Handlers/ScalabilityConsoleGuard.h. Runs ahead
    // of the GEditor check because it is a fact about the string and the console registry, not
    // about the editor being up.
    if (ScalabilityConsoleGuard::IsScalabilityPinningLine(Cmd) && !Ctx.GetBool(TEXT("force"), false))
    {
        Ctx.SendError(TEXT("SCALABILITY_CVAR_USE_TYPED_VERB"),
            ScalabilityConsoleGuard::MakeScalabilityTypedVerbRefusal(Cmd));
        return true;
    }

    if (!GEditor)
    {
        Ctx.SendError(TEXT("EDITOR_NOT_AVAILABLE"), TEXT("Editor not available"));
        return true;
    }

    bool bExecCalled = false;
    bool bOk = false;

    // Prefer executing with a valid editor world context where possible to
    // avoid assertions inside engine helpers that require a proper world
    // (e.g. when running Open/Map commands).
    UWorld* TargetWorld = nullptr;
    if (GEditor)
    {
        if (UUnrealEditorSubsystem* UES = GEditor->GetEditorSubsystem<UUnrealEditorSubsystem>())
        {
            TargetWorld = UES->GetEditorWorld();
        }
        if (!TargetWorld)
        {
            TargetWorld = GEditor->GetEditorWorldContext().World();
        }
    }

    if (GEditor && TargetWorld)
    {
        bOk = GEditor->Exec(TargetWorld, *Cmd);
        bExecCalled = true;
    }

    // Fallback: try all known engine world contexts if the editor world
    // did not handle the command successfully.
    if (!bOk && GEngine)
    {
        for (const FWorldContext& WCtx : GEngine->GetWorldContexts())
        {
            UWorld* World = WCtx.World();
            if (!World) continue;
            const bool bWorldOk = GEngine->Exec(World, *Cmd);
            bExecCalled = bExecCalled || bWorldOk;
            if (bWorldOk)
            {
                bOk = true;
                break;
            }
        }
    }

    // If we could not find any valid world to execute against, avoid
    // invoking the engine command path entirely and return a structured
    // error instead of risking an assertion.
    if (!bExecCalled && !TargetWorld)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("command"), Cmd);
        Out->SetBoolField(TEXT("success"), false);
        Ctx.SendError(TEXT("EDITOR_WORLD_NOT_AVAILABLE"), TEXT("Editor world not available for command"));
        return true;
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("command"), Cmd);
    Out->SetBoolField(TEXT("success"), bOk);
    if (bOk)
    {
        Ctx.SendSuccess(Out);
    }
    else
    {
        Ctx.SendError(TEXT("EXEC_FAILED"), TEXT("Command not executed"));
    }
    return true;
}
