// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Utils/JobMonitorLog.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJobMonitorLogAppendsTest,
    "PinWright.JobMonitorLog.Append",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJobMonitorLogAppendsTest::RunTest(const FString& Parameters)
{
    const FString TmpPath = FPaths::ProjectIntermediateDir() / TEXT("JobMonitorLogTest.jsonl");
    IFileManager::Get().Delete(*TmpPath);

    FJobMonitorLog Log(TmpPath, /*MaxBytes=*/1024 * 1024, /*KeepRotations=*/3);

    auto Line = MakeShared<FJsonObject>();
    Line->SetStringField(TEXT("ticket_id"), TEXT("j_test"));
    Line->SetStringField(TEXT("event"), TEXT("started"));
    Log.AppendEvent(Line);

    FString Contents;
    TestTrue(TEXT("File created"),
        FFileHelper::LoadFileToString(Contents, *TmpPath, FFileHelper::EHashOptions::None, FILEREAD_AllowWrite));
    TestTrue(TEXT("Contains ticket"), Contents.Contains(TEXT("\"ticket_id\":\"j_test\"")));
    TestTrue(TEXT("Single trailing newline"), Contents.EndsWith(TEXT("\n")));

    IFileManager::Get().Delete(*TmpPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJobMonitorLogRotatesTest,
    "PinWright.JobMonitorLog.Rotate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FJobMonitorLogRotatesTest::RunTest(const FString& Parameters)
{
    const FString TmpPath = FPaths::ProjectIntermediateDir() / TEXT("JobMonitorLogRotate.jsonl");
    IFileManager::Get().Delete(*TmpPath);
    IFileManager::Get().Delete(*(TmpPath + TEXT(".1")));

    FJobMonitorLog Log(TmpPath, /*MaxBytes=*/64, /*KeepRotations=*/3);

    for (int32 i = 0; i < 8; ++i)
    {
        auto Line = MakeShared<FJsonObject>();
        Line->SetNumberField(TEXT("i"), i);
        Line->SetStringField(TEXT("filler"),
            TEXT("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"));
        Log.AppendEvent(Line);
    }

    TestTrue(TEXT("Primary file exists"),
        IFileManager::Get().FileExists(*TmpPath));
    TestTrue(TEXT("Rotation .1 exists"),
        IFileManager::Get().FileExists(*(TmpPath + TEXT(".1"))));

    IFileManager::Get().Delete(*TmpPath);
    IFileManager::Get().Delete(*(TmpPath + TEXT(".1")));
    return true;
}
