// Copyright (c) 2026 Alexander Penkin. MIT License.

// UObject-free TraceServices analysis core for insights.export_trace.
// Starts a .utrace analysis at a safe point, then lets a worker poll the
// analysis session and write CSV data products (timer_stats, frame_series,
// counters, timers/threads listings). All provider reads happen under a single
// FAnalysisSessionReadScope; this header carries only request/result structs
// plus the analysis-phase free functions.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include <atomic>

namespace PinWrightRpc::TraceExport
{
    // Which data products the caller wants written.
    struct FExportKinds
    {
        bool bTimerStats = false;
        bool bFrameSeries = false;
        bool bCounters = false;
        bool bTimers = false;
    };

    // A named time window (seconds) for timer-stat aggregation / counter scoping.
    struct FExportWindow
    {
        FString Name;
        double StartTime = 0.0;
        double EndTime = 0.0;
    };

    // Worker-thread request. All inputs are resolved on the game thread before
    // dispatch so the worker never touches UObject or asset state.
    struct FExportRequest
    {
        FString TracePath;       // absolute path to a .utrace file
        FString OutDir;          // absolute output directory (created if missing)
        FExportKinds Kinds;
        TArray<FExportWindow> Windows;

        // Thread selection for timer_stats. Empty Names + Ids => all threads.
        // Names match exact or substring against the runtime thread name;
        // Ids match verbatim.
        TArray<FString> ThreadNames;
        TArray<uint32> ThreadIds;

        // Thread selection for frame_series specifically. When the caller passes
        // no `threads`, the handler seeds this with GameThread/RenderThread/RHIThread
        // while leaving the timer_stats selection as "all". Empty => fall back to
        // the shared ThreadNames/ThreadIds selection.
        TArray<FString> FrameSeriesThreadNames;
        TArray<uint32> FrameSeriesThreadIds;

        // timer_stats selection: explicit timer names AND/OR a top-N cap by
        // total inclusive time. TopN<=0 disables the cap.
        TArray<FString> TimerNames;
        int32 TopN = 20;

        int32 TableEntryLimit = 5000;
        double CounterDownsampleHz = 0.0; // 0 => emit every value
        double TimeoutSeconds = 300.0; // validated by the handler, max 1800 seconds
        TSet<FString> PreExistingOutputFiles; // destinations rejected before mutation
    };

    // One truncation event: appended whenever a configured limit drops output.
    struct FTruncationEvent
    {
        FString Kind;     // "timer_stats" | "counters" | ...
        FString Window;   // window name, or "*"
        FString Reason;   // human-readable cause
        int64 Kept = 0;
        int64 Dropped = 0;
        int64 Limit = 0;
    };

    // Resolved window echoed back in the summary (EndTime clamped to duration).
    struct FResolvedWindow
    {
        FString Name;
        double StartTime = 0.0;
        double EndTime = 0.0;
        // True only for the synthesized whole-trace default window (no caller
        // windows[]). Drives the unbounded counter scan; a caller-supplied window
        // literally named "all" must NOT trigger it.
        bool bIsDefaultAllWindow = false;
    };

    // Worker-thread result. ResultJson is built on the worker (TraceServices and
    // FJsonObject are UObject-free) and handed straight to FJobRegistry::Complete.
    struct FExportResult
    {
        bool bSuccess = false;
        FString Error;       // uppercase domain code on failure (empty on success)
        FString Message;     // human-readable detail
        TSharedPtr<::FJsonObject> ResultJson;
        TArray<FString> PublishedFiles; // successfully verified paths owned by this request
    };

    struct FTraceAnalysis;

    // Must run at a game-thread safe point. StartAnalysis begins the engine
    // analysis without waiting for the trace to finish parsing.
    FExportResult StartTraceAnalysis(const FExportRequest& Request,
        TSharedPtr<FTraceAnalysis>& OutAnalysis);

    // Worker-side completion poll. Provider reads and CSV writes must wait until
    // this returns true.
    bool IsTraceAnalysisComplete(const TSharedPtr<FTraceAnalysis>& Analysis);

    // Worker-side provider reads and CSV publication after analysis completes.
    FExportResult ExportTraceAnalysis(const FExportRequest& Request,
        const TSharedPtr<FTraceAnalysis>& Analysis,
        const TSharedRef<std::atomic<bool>>& CancelRequested,
        double DeadlineSeconds);

    // Worker-only teardown. Stops/waits for TraceServices and releases the
    // session before the game-thread completion continuation is queued.
    void ReleaseTraceAnalysisOnWorker(TSharedPtr<FTraceAnalysis>& Analysis);

#if WITH_DEV_AUTOMATION_TESTS
    // Test-only seam: make the production atomic writer stage and discard partial
    // bytes so handler tests can verify typed failure and temporary-file cleanup.
    PINWRIGHT_API void SetWriteFailureInjectionForTests(bool bEnabled);

    // Test-only seam: make the trace's Game-frame provider appear empty so the
    // handler test can verify that frame_series cannot report false success.
    PINWRIGHT_API void SetForceNoGameFramesForTests(bool bEnabled);

    PINWRIGHT_API void SetForceSlowAnalysisForTests(bool bEnabled);
#endif
}
