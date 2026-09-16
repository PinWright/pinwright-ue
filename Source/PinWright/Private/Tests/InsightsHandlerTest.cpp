// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Handlers/Debug/TraceExportCore.h"
#include "Handlers/Debug/InsightsHandlerInternal.h"
#include "Handlers/ErrorCodes.h"
#include "State/JobRegistry.h"
#include "State/PluginState.h"
#include "ProfilingDebugging/TraceAuxiliary.h"
#include "Tests/TestUtils.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Containers/Ticker.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestSkipReporting.h"

// Verifies ResolveActiveTracePath returns an empty string when no trace is
// connected. Counterfactual: if the helper hardcoded a sentinel path (or
// returned a stale value from a previous session), .IsEmpty() would be false
// and this assertion would fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInsightsResolveActiveTracePathNotConnectedReturnsEmptyTest,
    "PinWright.insights.resolve_path.NotConnectedReturnsEmpty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInsightsResolveActiveTracePathNotConnectedReturnsEmptyTest::RunTest(const FString& Parameters)
{
    // Ensure no trace is active before measuring the disconnected state.
    if (FTraceAuxiliary::IsConnected())
    {
        FTraceAuxiliary::Stop();
    }

    TestTrue(TEXT("ResolveActiveTracePath is empty when not connected"),
        PinWrightRpc::Insights::ResolveActiveTracePath().IsEmpty());

    return true;
}

// Verifies ResolveActiveTracePath returns a non-empty destination string that
// reflects the file trace target after a successful Start. Skips with a
// warning if Start returns false (trace subsystem unavailable on this build).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInsightsResolveActiveTracePathAfterFileTraceReturnsPathTest,
    "PinWright.insights.resolve_path.AfterFileTraceReturnsPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInsightsResolveActiveTracePathAfterFileTraceReturnsPathTest::RunTest(const FString& Parameters)
{
    const bool bStarted = FTraceAuxiliary::Start(
        FTraceAuxiliary::EConnectionType::File,
        TEXT("InsightsHandlerTest"),
        nullptr,
        nullptr);

    if (!bStarted)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("trace-unavailable"),
            TEXT("Trace start unavailable, skipping"));
        return true;
    }

    const FString Path = PinWrightRpc::Insights::ResolveActiveTracePath();
    TestFalse(TEXT("ResolveActiveTracePath is non-empty after file trace start"), Path.IsEmpty());
    TestTrue(TEXT("Path contains the requested trace target name"),
        Path.Contains(TEXT("InsightsHandlerTest"), ESearchCase::IgnoreCase));

    FTraceAuxiliary::Stop();
    return true;
}

// Regression test for E-insights-snapshot-empty-filepath: when filePath is
// omitted, the snapshot helper must resolve and return the actual auto-generated
// .utrace path the engine wrote — not echo back the empty input. Counterfactual:
// the pre-fix handler set filePath to the empty input string, so OutResolvedPath
// would be empty here and both assertions below would fail. Skips with a warning
// if WriteSnapshot is unavailable on this build (e.g. trace subsystem disabled).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInsightsWriteSnapshotResolvesAutoPathTest,
    "PinWright.insights.snapshot.OmittedPathResolvesAutoName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInsightsWriteSnapshotResolvesAutoPathTest::RunTest(const FString& Parameters)
{
    // Snapshot the in-memory trace buffer with NO requested path (the
    // auto-generate case the ticket flagged). The engine picks the name; the
    // helper must report it back via the OnSnapshotSaved delegate.
    FString ResolvedPath;
    bool bResolvedFromEngine = false;
    const bool bOk = PinWrightRpc::Insights::WriteSnapshotResolvingPath(FString(), ResolvedPath, bResolvedFromEngine);

    if (!bOk)
    {
        // WriteSnapshot can fail when the trace subsystem is not running on this
        // build/config; that is not the defect under test.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("trace-unavailable"),
            TEXT("WriteSnapshot unavailable, skipping auto-name resolution check"));
        return true;
    }

    // The core regression assertion: an omitted filePath must NOT round-trip as
    // empty — the engine-resolved auto-generated path is reported instead.
    TestFalse(TEXT("Resolved snapshot path is non-empty for an omitted filePath"), ResolvedPath.IsEmpty());
    TestTrue(TEXT("Resolved snapshot path is a .utrace file"),
        ResolvedPath.EndsWith(TEXT(".utrace"), ESearchCase::IgnoreCase));
    // An omitted filePath has no echo to fall back to, so a non-empty result can
    // only have come from the engine's OnSnapshotSaved delegate.
    TestTrue(TEXT("Omitted-path resolution came from the engine"), bResolvedFromEngine);

    // Clean up the snapshot artifact so the test leaves no residue.
    if (!ResolvedPath.IsEmpty())
    {
        IFileManager::Get().Delete(*ResolvedPath, /*RequireExists*/ false, /*EvenReadOnly*/ true);
    }

    return true;
}

// Drives the real handler/job path with an injected AtomicFileWriter stage failure.
// The injected stage writes bytes to the temporary sibling and then returns an OS-style
// error; AtomicFileWriter must clean that temporary file, leave the final destination absent,
// and the job must surface EXPORT_FAILED rather than report success. The injection does not
// emit a UE Error log, so no AddExpectedError declaration is needed for this deliberate failure.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInsightsExportWriteFailureIsTypedAndAtomicTest,
    "PinWright.insights.export_trace.WriteFailureIsTypedAndLeavesNoPartialFile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInsightsExportWriteFailureIsTypedAndAtomicTest::RunTest(const FString& Parameters)
{
    if (FTraceAuxiliary::IsConnected())
    {
        FTraceAuxiliary::Stop();
    }

    const bool bStarted = FTraceAuxiliary::Start(
        FTraceAuxiliary::EConnectionType::File,
        TEXT("InsightsExportWriteFailureTest"),
        nullptr,
        nullptr);
    if (!bStarted)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("trace-unavailable"),
            TEXT("Trace start unavailable, skipping export write-failure check"));
        return true;
    }

    FString TracePath = PinWrightRpc::Insights::ResolveActiveTracePath();
    FTraceAuxiliary::Stop();

    const FString OutDir = FPaths::ConvertRelativePathToFull(
        FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("InsightsExportFailureTests"),
            FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    const FString CsvPath = FPaths::Combine(OutDir, TEXT("counters.csv"));
    IFileManager& FileManager = IFileManager::Get();
    bool bCleanupSafe = false;
    ON_SCOPE_EXIT
    {
        if (bCleanupSafe)
        {
            PinWrightRpc::TraceExport::SetWriteFailureInjectionForTests(false);
            FileManager.DeleteDirectory(*OutDir, /*RequireExists=*/false, /*Tree=*/true);
        }
    };

    if (TracePath.IsEmpty() || !FileManager.FileExists(*TracePath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("trace-path-unavailable"),
            TEXT("File trace did not produce an analyzable path, skipping export check"));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("tracePath"), TracePath);
    Payload->SetStringField(TEXT("outDir"), OutDir);
    TArray<TSharedPtr<FJsonValue>> Kinds;
    Kinds.Add(MakeShared<FJsonValueString>(TEXT("counters")));
    Payload->SetArrayField(TEXT("kind"), Kinds);

    PinWrightRpc::TraceExport::SetWriteFailureInjectionForTests(true);
    FTestResponseCapture Capture;
    TestTrue(TEXT("export handler is registered"),
        InvokeHandlerWithCapture(TEXT("insights.export_trace"), Payload, Capture));
    TestTrue(TEXT("export handler returns the running ticket"), Capture.bWasCalled && Capture.bSuccess);

    FString TicketId;
    if (!Capture.Result.IsValid() ||
        !Capture.Result->TryGetStringField(TEXT("ticket_id"), TicketId) || TicketId.IsEmpty())
    {
        TestTrue(TEXT("running response contains a ticket id"), false);
        PinWrightRpc::TraceExport::SetWriteFailureInjectionForTests(false);
        bCleanupSafe = true;
        return true;
    }

    FJobTicket Ticket;
    TArray<FString> TempFiles;
    const double Deadline = FPlatformTime::Seconds() + 30.0;
    while (FPluginState::Get().GetJobRegistry().Get(TicketId, Ticket)
        && Ticket.Status == TEXT("running")
        && FPlatformTime::Seconds() < Deadline)
    {
        FTSTicker::GetCoreTicker().Tick(0.01f);
        FPlatformProcess::Sleep(0.001f);
    }

    if (Ticket.Status == TEXT("running"))
    {
        FPluginState::Get().GetJobRegistry().Cancel(TicketId);
        const double CleanupDeadline = FPlatformTime::Seconds() + 30.0;
        while (FPluginState::Get().GetJobRegistry().Get(TicketId, Ticket)
            && (Ticket.Status == TEXT("running") || TempFiles.Num() > 0
                || !PinWrightRpc::Insights::IsTraceExportWorkerQuiescentForTests())
            && FPlatformTime::Seconds() < CleanupDeadline)
        {
            FTSTicker::GetCoreTicker().Tick(0.01f);
            TempFiles.Reset();
            FileManager.FindFiles(TempFiles, *FPaths::Combine(OutDir, TEXT("*.tmp")), true, false);
            FPlatformProcess::Sleep(0.001f);
        }
    }
    TempFiles.Reset();
    FileManager.FindFiles(TempFiles, *FPaths::Combine(OutDir, TEXT("*.tmp")), true, false);
    bCleanupSafe = Ticket.Status != TEXT("running") && !FileManager.FileExists(*CsvPath)
        && TempFiles.Num() == 0
        && PinWrightRpc::Insights::IsTraceExportWorkerQuiescentForTests();
    TestTrue(TEXT("export cleanup completed before test injection reset"), bCleanupSafe);

    TestEqual(TEXT("export job reaches a terminal state"), Ticket.Status, FString(TEXT("failed")));
    TestEqual(TEXT("write failure uses EXPORT_FAILED"), Ticket.Error,
        FString(ErrorCodes::ERR_EXPORT_FAILED));

    FString FailureMessage;
    const bool bHasFailureMessage = Ticket.Result.IsValid()
        && Ticket.Result->TryGetStringField(TEXT("message"), FailureMessage);
    TestTrue(TEXT("failed job carries the writer message"), bHasFailureMessage);
    if (bHasFailureMessage)
    {
        TestTrue(TEXT("writer message includes the attempted CSV path"),
            FailureMessage.Contains(CsvPath));
        TestTrue(TEXT("writer message includes the OS error"),
            FailureMessage.Contains(TEXT("(error 5:")));
    }

    TestFalse(TEXT("failed atomic publication leaves no final CSV"),
        FileManager.FileExists(*CsvPath));
    FileManager.FindFiles(TempFiles, *FPaths::Combine(OutDir, TEXT("*.tmp")),
        /*Files=*/true, /*Directories=*/false);
    TestTrue(TEXT("failed atomic publication removes the partial temporary file"),
        TempFiles.Num() == 0);
    return true;
}

// Drives the real handler/job path with an empty Game-frame provider. A requested
// frame_series must fail with the attempted artifact path instead of completing
// with an empty files array. This deliberate failure logs no UE Error, so no
// AddExpectedError declaration is needed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInsightsExportNoGameFramesIsTypedFailureTest,
    "PinWright.insights.export_trace.NoGameFramesAreTypedFailure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInsightsExportNoGameFramesIsTypedFailureTest::RunTest(const FString& Parameters)
{
    if (FTraceAuxiliary::IsConnected())
    {
        FTraceAuxiliary::Stop();
    }

    const bool bStarted = FTraceAuxiliary::Start(
        FTraceAuxiliary::EConnectionType::File,
        TEXT("InsightsExportNoFramesTest"),
        nullptr,
        nullptr);
    if (!bStarted)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("trace-unavailable"),
            TEXT("Trace start unavailable, skipping no-frame export check"));
        return true;
    }

    FString TracePath = PinWrightRpc::Insights::ResolveActiveTracePath();
    FTraceAuxiliary::Stop();

    const FString OutDir = FPaths::ConvertRelativePathToFull(
        FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("InsightsExportNoFrameTests"),
            FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    const FString CsvPath = FPaths::Combine(OutDir, TEXT("frame_series.csv"));
    IFileManager& FileManager = IFileManager::Get();
    bool bCleanupSafe = false;
    ON_SCOPE_EXIT
    {
        if (bCleanupSafe)
        {
            PinWrightRpc::TraceExport::SetForceNoGameFramesForTests(false);
            FileManager.DeleteDirectory(*OutDir, /*RequireExists=*/false, /*Tree=*/true);
        }
    };

    if (TracePath.IsEmpty() || !FileManager.FileExists(*TracePath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("trace-path-unavailable"),
            TEXT("File trace did not produce an analyzable path, skipping no-frame export check"));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("tracePath"), TracePath);
    Payload->SetStringField(TEXT("outDir"), OutDir);
    TArray<TSharedPtr<FJsonValue>> Kinds;
    Kinds.Add(MakeShared<FJsonValueString>(TEXT("frame_series")));
    Payload->SetArrayField(TEXT("kind"), Kinds);

    PinWrightRpc::TraceExport::SetForceNoGameFramesForTests(true);
    FTestResponseCapture Capture;
    TestTrue(TEXT("export handler is registered"),
        InvokeHandlerWithCapture(TEXT("insights.export_trace"), Payload, Capture));
    TestTrue(TEXT("export handler returns the running ticket"), Capture.bWasCalled && Capture.bSuccess);

    FString TicketId;
    if (!Capture.Result.IsValid() ||
        !Capture.Result->TryGetStringField(TEXT("ticket_id"), TicketId) || TicketId.IsEmpty())
    {
        TestTrue(TEXT("running response contains a ticket id"), false);
        PinWrightRpc::TraceExport::SetForceNoGameFramesForTests(false);
        bCleanupSafe = true;
        return true;
    }

    FJobTicket Ticket;
    TArray<FString> TempFiles;
    const double Deadline = FPlatformTime::Seconds() + 30.0;
    while (FPluginState::Get().GetJobRegistry().Get(TicketId, Ticket)
        && Ticket.Status == TEXT("running")
        && FPlatformTime::Seconds() < Deadline)
    {
        FTSTicker::GetCoreTicker().Tick(0.01f);
        FPlatformProcess::Sleep(0.001f);
    }

    if (Ticket.Status == TEXT("running"))
    {
        FPluginState::Get().GetJobRegistry().Cancel(TicketId);
        const double CleanupDeadline = FPlatformTime::Seconds() + 30.0;
        while (FPluginState::Get().GetJobRegistry().Get(TicketId, Ticket)
            && (Ticket.Status == TEXT("running") || TempFiles.Num() > 0
                || !PinWrightRpc::Insights::IsTraceExportWorkerQuiescentForTests())
            && FPlatformTime::Seconds() < CleanupDeadline)
        {
            FTSTicker::GetCoreTicker().Tick(0.01f);
            TempFiles.Reset();
            FileManager.FindFiles(TempFiles, *FPaths::Combine(OutDir, TEXT("*.tmp")), true, false);
            FPlatformProcess::Sleep(0.001f);
        }
    }
    TempFiles.Reset();
    FileManager.FindFiles(TempFiles, *FPaths::Combine(OutDir, TEXT("*.tmp")), true, false);
    bCleanupSafe = Ticket.Status != TEXT("running") && !FileManager.FileExists(*CsvPath)
        && TempFiles.Num() == 0
        && PinWrightRpc::Insights::IsTraceExportWorkerQuiescentForTests();
    TestTrue(TEXT("no-frame cleanup completed before test injection reset"), bCleanupSafe);

    TestEqual(TEXT("no-frame export reaches a terminal state"), Ticket.Status, FString(TEXT("failed")));
    TestEqual(TEXT("no-frame export uses EXPORT_FAILED"), Ticket.Error,
        FString(ErrorCodes::ERR_EXPORT_FAILED));

    FString FailureMessage;
    const bool bHasFailureMessage = Ticket.Result.IsValid()
        && Ticket.Result->TryGetStringField(TEXT("message"), FailureMessage);
    TestTrue(TEXT("no-frame failure carries an artifact message"), bHasFailureMessage);
    if (bHasFailureMessage)
    {
        TestTrue(TEXT("no-frame message includes the attempted CSV path"),
            FailureMessage.Contains(CsvPath));
        TestTrue(TEXT("no-frame message explains the missing Game frames"),
            FailureMessage.Contains(TEXT("no Game frames")));
    }

    TestFalse(TEXT("no-frame failure leaves no final CSV"),
        FileManager.FileExists(*CsvPath));
    FileManager.FindFiles(TempFiles, *FPaths::Combine(OutDir, TEXT("*.tmp")),
        /*Files=*/true, /*Directories=*/false);
    TestTrue(TEXT("no-frame failure leaves no temporary CSV"), TempFiles.Num() == 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInsightsExportTimeoutIsTypedAndCleansArtifactsTest,
    "PinWright.insights.export_trace.TimeoutIsTypedAndLeavesNoArtifacts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInsightsExportTimeoutIsTypedAndCleansArtifactsTest::RunTest(const FString& Parameters)
{
    if (FTraceAuxiliary::IsConnected())
    {
        FTraceAuxiliary::Stop();
    }
    const bool bStarted = FTraceAuxiliary::Start(
        FTraceAuxiliary::EConnectionType::File,
        TEXT("InsightsExportTimeoutTest"), nullptr, nullptr);
    if (!bStarted)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("trace-unavailable"),
            TEXT("Trace start unavailable, skipping timeout check"));
        return true;
    }

    const FString TracePath = PinWrightRpc::Insights::ResolveActiveTracePath();
    FTraceAuxiliary::Stop();
    const FString OutDir = FPaths::ConvertRelativePathToFull(
        FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("InsightsExportTimeoutTests"),
            FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    const FString CsvPath = FPaths::Combine(OutDir, TEXT("frame_series.csv"));
    IFileManager& FileManager = IFileManager::Get();
    bool bCleanupSafe = false;
    ON_SCOPE_EXIT
    {
        if (bCleanupSafe)
        {
            PinWrightRpc::TraceExport::SetForceSlowAnalysisForTests(false);
            FileManager.DeleteDirectory(*OutDir, false, true);
        }
    };
    if (TracePath.IsEmpty() || !FileManager.FileExists(*TracePath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("trace-path-unavailable"),
            TEXT("File trace did not produce an analyzable path, skipping timeout check"));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("tracePath"), TracePath);
    Payload->SetStringField(TEXT("outDir"), OutDir);
    Payload->SetNumberField(TEXT("timeoutSeconds"), 0.1);
    TArray<TSharedPtr<FJsonValue>> Kinds;
    Kinds.Add(MakeShared<FJsonValueString>(TEXT("frame_series")));
    Payload->SetArrayField(TEXT("kind"), Kinds);

    PinWrightRpc::TraceExport::SetForceSlowAnalysisForTests(true);
    FTestResponseCapture Capture;
    TestTrue(TEXT("timeout test invokes the export handler"),
        InvokeHandlerWithCapture(TEXT("insights.export_trace"), Payload, Capture));
    FString TicketId;
    TestTrue(TEXT("timeout test receives a ticket"), Capture.Result.IsValid() &&
        Capture.Result->TryGetStringField(TEXT("ticket_id"), TicketId));
    if (TicketId.IsEmpty())
    {
        PinWrightRpc::TraceExport::SetForceSlowAnalysisForTests(false);
        bCleanupSafe = true;
        return true;
    }

    FJobTicket Ticket;
    const double Deadline = FPlatformTime::Seconds() + 30.0;
    while (FPluginState::Get().GetJobRegistry().Get(TicketId, Ticket)
        && Ticket.Status == TEXT("running") && FPlatformTime::Seconds() < Deadline)
    {
        FTSTicker::GetCoreTicker().Tick(0.01f);
        FPlatformProcess::Sleep(0.001f);
    }
    if (Ticket.Status == TEXT("running"))
    {
        FPluginState::Get().GetJobRegistry().Cancel(TicketId);
    }
    const double CleanupDeadline = FPlatformTime::Seconds() + 30.0;
    while (FPlatformTime::Seconds() < CleanupDeadline)
    {
        TArray<FString> TempFiles;
        FileManager.FindFiles(TempFiles, *FPaths::Combine(OutDir, TEXT("*.tmp")), true, false);
        if (Ticket.Status != TEXT("running") && !FileManager.FileExists(*CsvPath)
            && TempFiles.Num() == 0
            && PinWrightRpc::Insights::IsTraceExportWorkerQuiescentForTests())
        {
            bCleanupSafe = true;
            break;
        }
        FPluginState::Get().GetJobRegistry().Get(TicketId, Ticket);
        FTSTicker::GetCoreTicker().Tick(0.01f);
        FPlatformProcess::Sleep(0.001f);
    }
    TestTrue(TEXT("timeout job reaches a terminal state before cleanup"), bCleanupSafe);
    TestEqual(TEXT("timeout uses EXPORT_TIMED_OUT"), Ticket.Error,
        FString(ErrorCodes::ERR_EXPORT_TIMED_OUT));
    TestFalse(TEXT("timeout leaves no final CSV"), FileManager.FileExists(*CsvPath));
    return true;
}
