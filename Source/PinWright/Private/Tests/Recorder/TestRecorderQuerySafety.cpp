// Copyright (c) 2026 Alexander Penkin. MIT License.

// Behavioral regression tests for recorder.query's bounded child process and
// deterministic session resolution. The timeout case is latent because the
// handler returns a running job before the child reaches its terminal state.
#include "Misc/AutomationTest.h"

#include "Dispatch/RpcDispatcher.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Recorder/RecorderResolver.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "State/JobRegistry.h"
#include "State/PluginState.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestUtils.h"

namespace
{
    struct FRecorderQueryScopedTempSessions
    {
        TArray<FString> Paths;

        ~FRecorderQueryScopedTempSessions()
        {
            for (const FString& Path : Paths)
            {
                IFileManager::Get().Delete(*Path, /*RequireExists=*/false);
            }
        }
    };

    bool WriteTempSession(FAutomationTestBase& Test,
        const TSharedRef<FRecorderQueryScopedTempSessions>& Temp, const FString& FileName)
    {
        const FString Dir = RecorderResolver::RecordingsDir();
        IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);
        const FString Path = FPaths::Combine(Dir, FileName);
        Temp->Paths.Add(Path);

        const FString Header = FString::Printf(
            TEXT("{\"k\":\"header\",\"session\":\"%s\",\"fmt\":1}\n"),
            *FPaths::GetBaseFilename(FileName));
        return Test.TestTrue(TEXT("temporary recording is written"),
            FFileHelper::SaveStringToFile(Header, *Path));
    }

    bool IsPythonUnavailable(const FString& ErrorCode)
    {
        return ErrorCode == ErrorCodes::ERR_PYTHON_NOT_AVAILABLE ||
            ErrorCode == ErrorCodes::ERR_PYTHON_INIT_FAILED;
    }
}

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FRecorderQueryPollJob, TFunction<bool()>, Poll);
bool FRecorderQueryPollJob::Update()
{
    return Poll();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecorderQueryDeclaresBoundedTimeoutTest,
    "PinWright.recorder.query.DeclaresBoundedTimeout",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRecorderQueryDeclaresBoundedTimeoutTest::RunTest(const FString& Parameters)
{
    const FParamSpec* Spec = GetRegisteredParamSpec(TEXT("recorder.query"), TEXT("timeoutSeconds"));
    if (!TestNotNull(TEXT("recorder.query declares timeoutSeconds"), Spec))
    {
        return false;
    }
    TestEqual(TEXT("timeoutSeconds is numeric"), Spec->Type, FString(TEXT("number")));
    TestFalse(TEXT("timeoutSeconds is optional"), Spec->bRequired);
    TestEqual(TEXT("timeoutSeconds defaults to 30 seconds"), Spec->Default, FString(TEXT("30")));

    const TSharedRef<FRecorderQueryScopedTempSessions> Temp = MakeShared<FRecorderQueryScopedTempSessions>();
    const FString FileName = FString::Printf(TEXT("session-__query_timeout_%s.ndjson"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    if (!WriteTempSession(*this, Temp, FileName))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("session"), FileName);
    Params->SetStringField(TEXT("query"), TEXT("import time\ntime.sleep(1.0)\nreturn q.session_id"));
    Params->SetNumberField(TEXT("timeoutSeconds"), 0.5);

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    bool bSuccess = false;
    TSharedPtr<FJsonObject> Started;
    FString ErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("recorder.query"),
        TEXT("req-recorder-query-timeout"), Params, bSuccess, Started, ErrorCode);
    if (!bSuccess)
    {
        if (IsPythonUnavailable(ErrorCode))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("python-interpreter-unavailable"),
                FString::Printf(TEXT("recorder.query answered %s"), *ErrorCode));
            return true;
        }
        TestTrue(TEXT("recorder.query timeout fixture starts a job"), bSuccess);
        return true;
    }

    FString TicketId;
    if (!TestTrue(TEXT("timeout response carries a ticket id"),
        Started.IsValid() && Started->TryGetStringField(TEXT("ticket_id"), TicketId)))
    {
        return true;
    }

    const double PollDeadline = FPlatformTime::Seconds() + 10.0;
    ADD_LATENT_AUTOMATION_COMMAND(FRecorderQueryPollJob(
        [this, TicketId, Temp, PollDeadline]()
        {
            FJobTicket Ticket;
            if (!FPluginState::Get().GetJobRegistry().Get(TicketId, Ticket))
            {
                if (FPlatformTime::Seconds() >= PollDeadline)
                {
                    AddError(TEXT("timeout job disappeared before reaching a terminal state"));
                    return true;
                }
                return false;
            }

            if (Ticket.Status == TEXT("running"))
            {
                if (FPlatformTime::Seconds() >= PollDeadline)
                {
                    AddError(TEXT("timeout job remained running past the test bound"));
                    return true;
                }
                return false;
            }

            TestEqual(TEXT("sleeping query ends as a failed timeout"), Ticket.Status, FString(TEXT("failed")));
            TestEqual(TEXT("timeout job reports ERR_TIMEOUT"), Ticket.Error,
                FString(ErrorCodes::ERR_TIMEOUT));
            if (TestTrue(TEXT("timeout job has a result payload"), Ticket.Result.IsValid()))
            {
                bool bTimedOut = false;
                bool bTerminationConfirmed = true;
                bool bCleanupIncomplete = false;
                TestTrue(TEXT("timeout result carries timedOut=true"),
                    Ticket.Result->TryGetBoolField(TEXT("timedOut"), bTimedOut) && bTimedOut);
                TestTrue(TEXT("timeout result carries terminationConfirmed"),
                    Ticket.Result->TryGetBoolField(TEXT("terminationConfirmed"), bTerminationConfirmed));
                const bool bHasCleanupIncomplete = Ticket.Result->TryGetBoolField(
                    TEXT("cleanupIncomplete"), bCleanupIncomplete);
                if (bHasCleanupIncomplete)
                {
                    TestTrue(TEXT("cleanupIncomplete is true when present"), bCleanupIncomplete);
                }
                if (!bTerminationConfirmed)
                {
                    TestTrue(TEXT("unconfirmed termination discloses incomplete cleanup"),
                        bHasCleanupIncomplete && bCleanupIncomplete);
                }

                const TSharedPtr<FJsonObject>* Meta = nullptr;
                if (TestTrue(TEXT("timeout result carries metadata"),
                    Ticket.Result->TryGetObjectField(TEXT("meta"), Meta) && Meta && Meta->IsValid()))
                {
                    bool bMetaTimedOut = false;
                    TestTrue(TEXT("timeout metadata carries timedOut=true"),
                        (*Meta)->TryGetBoolField(TEXT("timedOut"), bMetaTimedOut) && bMetaTimedOut);
                }
            }
            return true;
        }));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecorderQueryResolvesSessionsUnambiguouslyTest,
    "PinWright.recorder.query.ResolvesSessionsUnambiguously",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRecorderQueryResolvesSessionsUnambiguouslyTest::RunTest(const FString& Parameters)
{
    const TSharedRef<FRecorderQueryScopedTempSessions> Temp = MakeShared<FRecorderQueryScopedTempSessions>();
    const FString Stamp = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Prefix = FString::Printf(TEXT("session-__query_ambiguous_%s"), *Stamp);
    const FString FirstFileName = Prefix + TEXT("_a.ndjson");
    const FString SecondFileName = Prefix + TEXT("_b.ndjson");
    if (!WriteTempSession(*this, Temp, FirstFileName) ||
        !WriteTempSession(*this, Temp, SecondFileName))
    {
        return false;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> AmbiguousParams = MakeShared<FJsonObject>();
    AmbiguousParams->SetStringField(TEXT("session"), FString::Printf(TEXT("__query_ambiguous_%s"), *Stamp));
    AmbiguousParams->SetStringField(TEXT("query"), TEXT("return q.session_id"));

    bool bSuccess = true;
    TSharedPtr<FJsonObject> Result;
    FString ErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("recorder.query"),
        TEXT("req-recorder-query-ambiguous"), AmbiguousParams, bSuccess, Result, ErrorCode);
    TestFalse(TEXT("shared substring is refused as ambiguous"), bSuccess);
    TestEqual(TEXT("ambiguous substring returns AMBIGUOUS_SESSION"), ErrorCode,
        FString(ErrorCodes::ERR_AMBIGUOUS_SESSION));

    const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
    if (TestTrue(TEXT("ambiguous response carries candidate ids"),
        Result.IsValid() && Result->TryGetArrayField(TEXT("candidates"), Candidates) && Candidates))
    {
        TestTrue(TEXT("ambiguous candidates include the first recording"),
            JsonValueArrayContainsString(Candidates, FPaths::GetBaseFilename(FirstFileName)));
        TestTrue(TEXT("ambiguous candidates include the second recording"),
            JsonValueArrayContainsString(Candidates, FPaths::GetBaseFilename(SecondFileName)));
    }

    TSharedPtr<FJsonObject> ExactParams = MakeShared<FJsonObject>();
    ExactParams->SetStringField(TEXT("session"), FirstFileName);
    ExactParams->SetStringField(TEXT("query"), TEXT("return q.session_id"));
    ExactParams->SetNumberField(TEXT("timeoutSeconds"), 2.0);
    TSharedPtr<FJsonObject> Started;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("recorder.query"),
        TEXT("req-recorder-query-exact"), ExactParams, bSuccess, Started, ErrorCode);
    if (!bSuccess)
    {
        if (IsPythonUnavailable(ErrorCode))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("python-interpreter-unavailable"),
                FString::Printf(TEXT("exact recorder.query answered %s"), *ErrorCode));
            return true;
        }
        TestTrue(TEXT("exact filename resolves to a query job"), bSuccess);
        return true;
    }

    FString TicketId;
    if (!TestTrue(TEXT("exact resolution returns a ticket id"),
        Started.IsValid() && Started->TryGetStringField(TEXT("ticket_id"), TicketId)))
    {
        return true;
    }

    const double PollDeadline = FPlatformTime::Seconds() + 10.0;
    ADD_LATENT_AUTOMATION_COMMAND(FRecorderQueryPollJob(
        [this, TicketId, FirstFileName, Temp, PollDeadline]()
        {
            FJobTicket Ticket;
            if (!FPluginState::Get().GetJobRegistry().Get(TicketId, Ticket))
            {
                if (FPlatformTime::Seconds() >= PollDeadline)
                {
                    AddError(TEXT("exact-session job disappeared before completion"));
                    return true;
                }
                return false;
            }
            if (Ticket.Status == TEXT("running"))
            {
                if (FPlatformTime::Seconds() >= PollDeadline)
                {
                    AddError(TEXT("exact-session job remained running past the test bound"));
                    return true;
                }
                return false;
            }

            TestEqual(TEXT("exact filename query completes"), Ticket.Status, FString(TEXT("completed")));
            if (TestTrue(TEXT("exact filename query returns a result"), Ticket.Result.IsValid()))
            {
                const TSharedPtr<FJsonObject>* Meta = nullptr;
                if (TestTrue(TEXT("exact filename result carries metadata"),
                    Ticket.Result->TryGetObjectField(TEXT("meta"), Meta) && Meta && Meta->IsValid()))
                {
                    FString ResolvedSession;
                    TestTrue(TEXT("exact result identifies the requested recording"),
                        (*Meta)->TryGetStringField(TEXT("session"), ResolvedSession));
                    TestEqual(TEXT("exact result uses the first filename"), ResolvedSession,
                        FPaths::GetCleanFilename(FirstFileName));
                }
            }
            return true;
        }));
    return true;
}
