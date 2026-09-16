// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-recorder-list-sessions-limit.
// recorder.list_sessions must accept an optional 'limit' param (default 20, 0 = all)
// that truncates the newest-first session list while totalCount keeps reporting the
// untruncated count so callers can detect elision.
//
// Routes through FRpcDispatcher::ProcessRequest with a real transport so it exercises
// param validation and the production handler body — the limit logic is never
// reimplemented here. Counterfactual: on pre-fix code (RPC_NO_PARAMS) the undeclared
// limit param sailed through validation — ValidateHandlerParams skips the unknown-param
// check when the registration declares zero params — and the handler returned all
// sessions, so the "limit:2 returns exactly 2 sessions" assertion fails with >=3 entries.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/Recorder/RecorderResolver.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Tests/Infra/DispatcherTestHelpers.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

namespace
{
    // Deletes the planted temp session files on scope exit so every early-out path
    // still cleans up Saved/PinWright/Recordings.
    struct FScopedTempSessions
    {
        TArray<FString> Paths;

        ~FScopedTempSessions()
        {
            for (const FString& Path : Paths)
            {
                IFileManager::Get().Delete(*Path);
            }
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecorderListSessionsLimitTest,
    "PinWright.recorder.query.ListSessionsLimit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRecorderListSessionsLimitTest::RunTest(const FString& Parameters)
{
    const FString Dir = RecorderResolver::RecordingsDir();
    IFileManager::Get().MakeDirectory(*Dir, true);

    // Plant 3 uniquely-named files matching the resolver's session-*.ndjson glob.
    // The directory may already hold real recordings, so assertions below only assume
    // "at least the 3 planted files exist".
    const FString Stamp = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    FScopedTempSessions Temp;
    for (int32 Index = 0; Index < 3; ++Index)
    {
        const FString Path = FPaths::Combine(Dir,
            FString::Printf(TEXT("session-__limit_test_%s_%d.ndjson"), *Stamp, Index));
        Temp.Paths.Add(Path);
        if (!TestTrue(TEXT("planted temp session file"),
            FFileHelper::SaveStringToFile(FString(TEXT("{}\n")), *Path)))
        {
            return true;
        }
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    // limit: 2 — exactly 2 entries returned, totalCount still reports the full count.
    TSharedPtr<FJsonObject> LimitParams = MakeShared<FJsonObject>();
    LimitParams->SetNumberField(TEXT("limit"), 2);

    bool bSuccess = false;
    TSharedPtr<FJsonObject> Result;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("recorder.list_sessions"),
        TEXT("req-list-sessions-limit-2"), LimitParams, bSuccess, Result, ErrorCode);

    TestTrue(TEXT("limit:2 accepted (handler succeeded)"), bSuccess);
    TestEqual(TEXT("limit:2 no error code"), ErrorCode, FString());
    if (!TestTrue(TEXT("limit:2 result object present"), Result.IsValid()))
    {
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* SessionsLimited = nullptr;
    if (!TestTrue(TEXT("limit:2 sessions array present"),
        Result->TryGetArrayField(TEXT("sessions"), SessionsLimited)))
    {
        return true;
    }
    TestEqual(TEXT("limit:2 returns exactly 2 sessions"), SessionsLimited->Num(), 2);

    int32 TotalCount = 0;
    TestTrue(TEXT("limit:2 totalCount present"), Result->TryGetNumberField(TEXT("totalCount"), TotalCount));
    TestTrue(TEXT("totalCount counts at least the 3 planted files"), TotalCount >= 3);
    TestTrue(TEXT("totalCount exceeds truncated sessions array (elision detectable)"),
        TotalCount > SessionsLimited->Num());

    // limit: 0 — everything is returned, sessions.Num() == totalCount.
    TSharedPtr<FJsonObject> AllParams = MakeShared<FJsonObject>();
    AllParams->SetNumberField(TEXT("limit"), 0);

    Dispatch(Dispatcher, Sink, TEXT("recorder.list_sessions"),
        TEXT("req-list-sessions-limit-0"), AllParams, bSuccess, Result, ErrorCode);

    TestTrue(TEXT("limit:0 accepted (handler succeeded)"), bSuccess);
    TestEqual(TEXT("limit:0 no error code"), ErrorCode, FString());
    if (!TestTrue(TEXT("limit:0 result object present"), Result.IsValid()))
    {
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* SessionsAll = nullptr;
    if (!TestTrue(TEXT("limit:0 sessions array present"),
        Result->TryGetArrayField(TEXT("sessions"), SessionsAll)))
    {
        return true;
    }
    int32 TotalCountAll = 0;
    TestTrue(TEXT("limit:0 totalCount present"), Result->TryGetNumberField(TEXT("totalCount"), TotalCountAll));
    TestEqual(TEXT("limit:0 returns every session (no truncation)"), SessionsAll->Num(), TotalCountAll);

    return true;
}
