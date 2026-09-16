// Copyright (c) 2026 Alexander Penkin. MIT License.

// End-to-end write-through test for the journal session writer, driven via the public
// FJournalRecorder facade (the writer itself is private to the PinWrightRecorder module).
// Pins the per-drain flush contract behind the "journal must never device-flush" fix:
// after one game-thread DrainAndFlush, the drained lines are already readable from the
// session NDJSON file while the session is still OPEN — the writer hands bytes to the OS
// every drain (plain write(), shared-read handle) instead of holding them until close.
// A regression that buffers lines until EndSession, or one that drops them entirely,
// fails the mid-session read below.
#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "JournalRecorder.h"
#include "JournalTypes.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Tests/TestSkipReporting.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJournalWriterFlushWriteThroughTest,
    "PinWright.recorder.writer.FlushWriteThroughMidSession",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FJournalWriterFlushWriteThroughTest::RunTest(const FString& Parameters)
{
    // A live session (e.g. a PIE journal) owns the global recorder; don't fight it.
    if (FJournalRecorder::IsRecording())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("journal-session-already-open"),
            TEXT("a journal session is already open; skipping JournalWriterFlush."));
        return true;
    }

    // Dedicated subdir so session creation / retention pruning never touches real recordings.
    // Pre-clean so leftovers from an interrupted earlier run can't break the single-file check.
    const FString Subdir = TEXT("PinWright/TestRecordings");
    const FString Dir = FPaths::ProjectSavedDir() / Subdir;
    IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists*/ false, /*Tree*/ true);

    FJournalRecorder::BeginSession(TEXT("writer-flush-test"), /*RetentionCap*/ 5, Subdir);
    if (!TestTrue(TEXT("session opened"), FJournalRecorder::IsRecording()))
    {
        return false;
    }

    // A unique marker makes the on-disk assertion independent of any other line content.
    const FString Marker = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    FJournalRecorder::LogEvent(FName(TEXT("writer:flush_test")),
        TArray<TPair<FName, FRecordedValue>>{ { FName(TEXT("marker")), FRecordedValue::From(Marker) } });

    // One game-thread drain — exactly what the PIE drain ticker does once per frame.
    FJournalRecorder::DrainAndFlush();

    // Locate the session file this test created (the subdir is exclusive to this test).
    TArray<FString> Files;
    IFileManager::Get().FindFiles(Files, *(Dir / TEXT("session-*.ndjson")), true, false);
    TestEqual(TEXT("exactly one session file in the test subdir"), Files.Num(), 1);

    bool bMarkerOnDisk = false;
    if (Files.Num() > 0)
    {
        // The session is still open; the writer holds the file with shared read access,
        // so the reader must tolerate the concurrent writer (FILEREAD_AllowWrite).
        FString Content;
        const bool bLoaded = FFileHelper::LoadFileToString(
            Content, *(Dir / Files[0]), FFileHelper::EHashOptions::None, FILEREAD_AllowWrite);
        TestTrue(TEXT("session file readable mid-session"), bLoaded);
        bMarkerOnDisk = bLoaded && Content.Contains(Marker);
    }
    TestTrue(TEXT("drained event is on disk before the session closes"), bMarkerOnDisk);

    FJournalRecorder::EndSession();
    TestFalse(TEXT("session closed"), FJournalRecorder::IsRecording());

    // Best-effort cleanup of the isolated test directory.
    IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists*/ false, /*Tree*/ true);

    return true;
}
