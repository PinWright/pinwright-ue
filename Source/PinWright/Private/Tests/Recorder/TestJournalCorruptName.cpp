// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression for the drain-tick SIGSEGV in FNdjsonSessionWriter::WriteEvent (FName::ToString on a
// corrupt FName) after a PIE map travel. The producer that crashed a real session called
// KeyFor(this) on a UObject that map travel had already garbage-collected: GetFName() on freed
// memory yields an FName whose entry ids lie outside the name pool, the queue carries it by value,
// and the game-thread drain segfaults resolving it with no trace of the producer.
//
// The test enqueues exactly such a name (an entry id past the last name-pool block, the shape a
// freed object's NamePrivate reads as) in every producer slot: event key, event prop key, value
// key, catalog key. Without the enqueue-time rejection the DrainAndFlush below crashes the editor
// in WriteObject/WriteEvent. With it, the corrupt messages are dropped with a warning and the
// valid marker event in the same drain is still written.
#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "JournalRecorder.h"
#include "JournalTypes.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Tests/TestSkipReporting.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJournalCorruptNameRejectedTest,
    "PinWright.recorder.writer.CorruptNameRejectedAtEnqueue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FJournalCorruptNameRejectedTest::RunTest(const FString& Parameters)
{
    if (FJournalRecorder::IsRecording())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("journal-session-already-open"),
            TEXT("a journal session is already open; skipping JournalCorruptName."));
        return true;
    }

    // A name-pool block index no editor ever reaches (blocks are 64K entries each).
    const FNameEntryId BadId = FNameEntryId::FromUnstableInt(0x7FFFFFF0u);
    const FName Corrupt(BadId, BadId, 0);
    if (!TestFalse(TEXT("fixture name lies outside the name pool"), Corrupt.IsValid()))
    {
        return false;
    }

    const FString Subdir = TEXT("PinWright/TestRecordingsCorruptName");
    const FString Dir = FPaths::ProjectSavedDir() / Subdir;
    IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists*/ false, /*Tree*/ true);

    FJournalRecorder::BeginSession(TEXT("corrupt-name-test"), /*RetentionCap*/ 5, Subdir);
    if (!TestTrue(TEXT("session opened"), FJournalRecorder::IsRecording()))
    {
        return false;
    }

    AddExpectedMessage(TEXT("carries a corrupt FName"), ELogVerbosity::Warning,
        EAutomationExpectedMessageFlags::Contains, /*Occurrences*/ 4);

    FJournalRecorder::LogEvent(Corrupt, FName(TEXT("test:corrupt_key")), {});
    FJournalRecorder::LogEvent(FName(TEXT("test:corrupt_prop")),
        TArray<TPair<FName, FRecordedValue>>{ { Corrupt, FRecordedValue::From(1) } });
    FJournalRecorder::LogVariable(Corrupt, FName(TEXT("test.corrupt_value")), 1.0f);
    FJournalRecorder::RegisterObject(Corrupt, TEXT("corrupt_object"));

    const FString Marker = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    FJournalRecorder::LogEvent(FName(TEXT("test:valid")),
        TArray<TPair<FName, FRecordedValue>>{ { FName(TEXT("marker")), FRecordedValue::From(Marker) } });

    // The drain the PIE ticker runs once per frame; the unfixed recorder segfaults here.
    FJournalRecorder::DrainAndFlush();

    TArray<FString> Files;
    IFileManager::Get().FindFiles(Files, *(Dir / TEXT("session-*.ndjson")), true, false);
    FString Content;
    const bool bLoaded = Files.Num() == 1 && FFileHelper::LoadFileToString(
        Content, *(Dir / Files[0]), FFileHelper::EHashOptions::None, FILEREAD_AllowWrite);
    TestTrue(TEXT("session file readable mid-session"), bLoaded);
    TestTrue(TEXT("valid event in the same drain is written"), Content.Contains(Marker));
    TestFalse(TEXT("event with a corrupt key is dropped"), Content.Contains(TEXT("test:corrupt_key")));
    TestFalse(TEXT("event with a corrupt prop key is dropped"), Content.Contains(TEXT("test:corrupt_prop")));
    TestFalse(TEXT("value with a corrupt key is dropped"), Content.Contains(TEXT("test.corrupt_value")));
    TestFalse(TEXT("catalog entry with a corrupt key is dropped"), Content.Contains(TEXT("corrupt_object")));

    FJournalRecorder::EndSession();
    IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists*/ false, /*Tree*/ true);
    return true;
}
