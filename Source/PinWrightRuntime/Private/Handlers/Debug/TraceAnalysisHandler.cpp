// Copyright (c) 2026 Alexander Penkin. MIT License.

// RPC handler for insights.export_trace: resolves params on the game thread,
// starts TraceServices analysis at a safe point, then polls and exports on a
// worker thread via a job ticket. The analysis core is UObject-free after start,
// so the worker touches only the analysis session and filesystem.
#include "CoreMinimal.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Debug/TraceExportCore.h"
#include "State/PluginState.h"
#include "Dispatch/SafePoint.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include <atomic>

namespace
{
    using namespace PinWrightRpc::TraceExport;
#if WITH_DEV_AUTOMATION_TESTS
    std::atomic<int32> GTraceExportWorkersInFlight(0);
#endif
    constexpr double kDefaultExportTimeoutSeconds = 300.0;
    constexpr double kMaxExportTimeoutSeconds = 1800.0;
    FCriticalSection GOutputReservationMutex;
    TSet<FString> GReservedOutputDirectories;

    class FOutputReservation
    {
    public:
        explicit FOutputReservation(FString InKey)
            : Key(MoveTemp(InKey))
        {
        }

        bool Acquire()
        {
            FScopeLock Lock(&GOutputReservationMutex);
            if (GReservedOutputDirectories.Contains(Key))
            {
                return false;
            }
            GReservedOutputDirectories.Add(Key);
            bHeld = true;
            return true;
        }

        ~FOutputReservation()
        {
            if (bHeld)
            {
                FScopeLock Lock(&GOutputReservationMutex);
                GReservedOutputDirectories.Remove(Key);
            }
        }

    private:
        FString Key;
        bool bHeld = false;
    };

    void SnapshotOutputFiles(const FString& OutDir, TSet<FString>& OutFiles)
    {
        for (const TCHAR* Pattern : {TEXT("*.csv"), TEXT("*.tmp")})
        {
            TArray<FString> Names;
            IFileManager::Get().FindFiles(Names,
                *FPaths::Combine(OutDir, Pattern), true, false);
            for (const FString& Name : Names)
            {
                OutFiles.Add(FPaths::ConvertRelativePathToFull(
                    FPaths::Combine(OutDir, Name)));
            }
        }
    }

    class FTraceExportJob : public TSharedFromThis<FTraceExportJob>
    {
    public:
        FTraceExportJob(FExportRequest InRequest,
            TSharedPtr<FAsyncRequestLifetimeLease> InRequestLease,
            TSharedRef<std::atomic<bool>> InAbandoned,
            TSharedPtr<FOutputReservation> InReservation)
            : Request(MoveTemp(InRequest))
            , RequestLease(MoveTemp(InRequestLease))
            , Abandoned(MoveTemp(InAbandoned))
            , Reservation(MoveTemp(InReservation))
        {
        }

        void Start(FJobOnComplete InOnComplete)
        {
            OnComplete = MoveTemp(InOnComplete);
            if (IsAbandoned() || IsCancelled())
            {
                Finish(FExportResult());
                return;
            }

            FExportResult StartResult = StartTraceAnalysis(Request, Analysis);
            if (!StartResult.bSuccess)
            {
                Finish(MoveTemp(StartResult));
                return;
            }

            DeadlineSeconds = FPlatformTime::Seconds() + Request.TimeoutSeconds;

            const TSharedRef<FTraceExportJob> Self = AsShared();
#if WITH_DEV_AUTOMATION_TESTS
            GTraceExportWorkersInFlight.fetch_add(1, std::memory_order_acq_rel);
#endif
            AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
                [Self]()
                {
                    while (!Self->IsAbandoned() && !Self->IsCancelled() &&
                           !IsTraceAnalysisComplete(Self->Analysis))
                    {
                        if (FPlatformTime::Seconds() >= Self->DeadlineSeconds)
                        {
                            Self->CleanupOwnedOutputs();
                            Self->ReleaseAnalysisOnWorker();
                            Self->QueueFinish(Self->MakeTimeoutResult(),
                                TEXT("insights.export_trace timeout"));
                            return;
                        }
                        FPlatformProcess::Sleep(0.01f);
                    }

                    if (Self->IsAbandoned() || Self->IsCancelled())
                    {
                        Self->CleanupOwnedOutputs();
                        Self->ReleaseAnalysisOnWorker();
                        Self->QueueFinish(FExportResult(),
                            TEXT("insights.export_trace cancellation"));
                        return;
                    }

                    FExportResult Result = ExportTraceAnalysis(
                        Self->Request, Self->Analysis, Self->Cancelled,
                        Self->DeadlineSeconds);
                    const bool bTimedOut = FPlatformTime::Seconds() >= Self->DeadlineSeconds;
                    if (!Result.bSuccess)
                    {
                        Self->CleanupOwnedOutputs(Result.PublishedFiles);
                    }
                    if (Self->IsAbandoned() || Self->IsCancelled() || bTimedOut)
                    {
                        Self->CleanupOwnedOutputs(Result.PublishedFiles);
                        if (bTimedOut && !Self->IsCancelled())
                        {
                            Result = Self->MakeTimeoutResult();
                        }
                        else
                        {
                            Result = FExportResult();
                        }
                    }
                    Self->ReleaseAnalysisOnWorker();
                    Self->QueueFinish(MoveTemp(Result),
                        TEXT("insights.export_trace completion"));
                });
        }

        void Abandon()
        {
            Abandoned->store(true, std::memory_order_release);
        }

        void Cancel()
        {
            Cancelled->store(true, std::memory_order_release);
        }

    private:
        void QueueFinish(FExportResult Result, const TCHAR* Reason)
        {
#if WITH_DEV_AUTOMATION_TESTS
            GTraceExportWorkersInFlight.fetch_sub(1, std::memory_order_acq_rel);
#endif
            const TSharedRef<FTraceExportJob> Self = AsShared();
            PinWrightSafePoint::DeferToSafePoint(
                [Self, Result = MoveTemp(Result)]() mutable
                {
                    Self->Finish(MoveTemp(Result));
                }, Reason);
        }

        FExportResult MakeTimeoutResult() const
        {
            FExportResult Result;
            Result.Error = ErrorCodes::ERR_EXPORT_TIMED_OUT;
            Result.Message = FString::Printf(
                TEXT("insights.export_trace exceeded timeoutSeconds=%.3f for trace '%s'; output '%s' was not retained."),
                Request.TimeoutSeconds, *Request.TracePath, *Request.OutDir);
            return Result;
        }

        void CleanupOwnedOutputs(const TArray<FString>& PublishedFiles = {}) const
        {
            for (const FString& Path : PublishedFiles)
            {
                const FString Absolute = FPaths::ConvertRelativePathToFull(Path);
                if (!Request.PreExistingOutputFiles.Contains(Absolute))
                {
                    IFileManager::Get().Delete(*Absolute, false, true, true);
                }
            }

        }

        void ReleaseAnalysisOnWorker()
        {
            bool bExpectedReleased = false;
            if (bAnalysisReleased.compare_exchange_strong(
                    bExpectedReleased, true, std::memory_order_acq_rel))
            {
                ReleaseTraceAnalysisOnWorker(Analysis);
            }
        }

        bool IsAbandoned() const
        {
            return Abandoned->load(std::memory_order_acquire);
        }

        bool IsCancelled() const
        {
            return Cancelled->load(std::memory_order_acquire);
        }

        void Finish(FExportResult Result)
        {
            bool bExpectedFinished = false;
            if (!bFinished.compare_exchange_strong(
                    bExpectedFinished, true, std::memory_order_acq_rel))
            {
                return;
            }

            TSharedPtr<FJsonObject> JobResult = MoveTemp(Result.ResultJson);
            if (!Result.bSuccess && !JobResult.IsValid())
            {
                JobResult = MakeShared<FJsonObject>();
                JobResult->SetStringField(TEXT("message"), Result.Message);
            }

            FJobOnComplete Completion = MoveTemp(OnComplete);
            if (!IsAbandoned() && Completion)
            {
                Completion(Result.bSuccess, MoveTemp(JobResult), MoveTemp(Result.Error));
            }
            RequestLease.Reset();
        }

        FExportRequest Request;
        FJobOnComplete OnComplete;
        TSharedPtr<FAsyncRequestLifetimeLease> RequestLease;
        TSharedRef<std::atomic<bool>> Abandoned;
        TSharedPtr<FOutputReservation> Reservation;
        TSharedRef<std::atomic<bool>> Cancelled = MakeShared<std::atomic<bool>>(false);
        TSharedPtr<FTraceAnalysis> Analysis;
        double DeadlineSeconds = 0.0;
        std::atomic<bool> bAnalysisReleased{false};
        std::atomic<bool> bFinished{false};
    };

    // Reads the first present value among a set of alias keys as a string.
    FString GetStringAlias(const FHandlerContext& Ctx, std::initializer_list<const TCHAR*> Keys)
    {
        TArray<FString> KeyList;
        for (const TCHAR* K : Keys)
        {
            KeyList.Add(FString(K));
        }
        return Ctx.GetStringFirstOf(KeyList);
    }

    // Reads the first present numeric value among alias keys; Default if none set.
    double GetNumberAlias(const FHandlerContext& Ctx, std::initializer_list<const TCHAR*> Keys, double Default)
    {
        const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
        if (Payload.IsValid())
        {
            for (const TCHAR* K : Keys)
            {
                if (Payload->HasTypedField<EJson::Number>(K))
                {
                    return Payload->GetNumberField(K);
                }
            }
        }
        return Default;
    }

    // Returns the named array under the first matching alias key, or nullptr.
    const TArray<TSharedPtr<FJsonValue>>* GetArrayAlias(const FHandlerContext& Ctx, std::initializer_list<const TCHAR*> Keys)
    {
        for (const TCHAR* K : Keys)
        {
            if (const TArray<TSharedPtr<FJsonValue>>* Arr = Ctx.GetArray(FString(K)))
            {
                return Arr;
            }
        }
        return nullptr;
    }

    // The default UnrealTrace store directory on the local machine.
    // FPlatformProcess::UserSettingsDir() resolves to %LOCALAPPDATA%/ on Windows.
    FString GetDefaultTraceStoreDir()
    {
        const FString UserSettings = FPlatformProcess::UserSettingsDir();
        if (UserSettings.IsEmpty())
        {
            return FString();
        }
        return FPaths::Combine(UserSettings, TEXT("UnrealEngine"), TEXT("Common"),
            TEXT("UnrealTrace"), TEXT("Store"), TEXT("001"));
    }

    // Scans a directory for .utrace files and returns the newest by mtime.
    void CollectNewestTrace(const FString& Dir, FString& OutPath, FDateTime& OutNewest)
    {
        if (Dir.IsEmpty() || !IFileManager::Get().DirectoryExists(*Dir))
        {
            return;
        }
        TArray<FString> Found;
        IFileManager::Get().FindFiles(Found, *(Dir / TEXT("*.utrace")), /*Files=*/true, /*Directories=*/false);
        for (const FString& File : Found)
        {
            const FString Full = Dir / File;
            const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Full);
            if (Stamp > OutNewest)
            {
                OutNewest = Stamp;
                OutPath = FPaths::ConvertRelativePathToFull(Full);
            }
        }
    }

    // Resolves the default trace path: newest .utrace across the UnrealTrace store
    // and the project's ProfilingDir. Empty if none found.
    FString ResolveDefaultTracePath()
    {
        FString Best;
        FDateTime Newest = FDateTime::MinValue();
        CollectNewestTrace(GetDefaultTraceStoreDir(), Best, Newest);
        CollectNewestTrace(FPaths::ProfilingDir(), Best, Newest);
        return Best;
    }

    // Parses a windows[] entry ({name,startTime/start,endTime/end}) into FExportWindow.
    bool ParseWindow(const TSharedPtr<FJsonObject>& Obj, FExportWindow& Out)
    {
        if (!Obj.IsValid())
        {
            return false;
        }
        Obj->TryGetStringField(TEXT("name"), Out.Name);
        if (Out.Name.IsEmpty())
        {
            Out.Name = TEXT("window");
        }
        double Start = 0.0;
        if (!Obj->TryGetNumberField(TEXT("startTime"), Start))
        {
            Obj->TryGetNumberField(TEXT("start"), Start);
        }
        double End = 0.0;
        if (!Obj->TryGetNumberField(TEXT("endTime"), End))
        {
            Obj->TryGetNumberField(TEXT("end"), End);
        }
        Out.StartTime = Start;
        Out.EndTime = End;
        return true;
    }
}

REGISTER_RPC_HANDLER("insights.export_trace", "insights",
    "Extract an Unreal Insights .utrace into CSV data products (timer_stats, frame_series, counters, timers/threads listings) for external pandas analysis. Starts TraceServices analysis at a safe point, then polls and writes CSVs on a worker thread. Long-running — returns a job ticket; poll via system.job_status.",
    RPC_PARAMS(
        RPC_PARAM_OPT("tracePath", "string", "Path to a .utrace file. Default: newest .utrace in the UnrealTrace store and the project ProfilingDir."),
        RPC_PARAM_OPT("kind", "array", "Subset of timer_stats|frame_series|counters|timers. Default: [\"frame_series\"]."),
        RPC_PARAM_OPT("windows", "array", "Array of {name,startTime,endTime} (seconds). Default: a single window 'all' spanning the trace."),
        RPC_PARAM_OPT("threads", "array", "Thread names or numeric ids. Default: all threads (frame_series defaults to GameThread,RenderThread,RHIThread)."),
        RPC_PARAM_OPT("timers", "array", "Explicit timer names for timer_stats (overrides topN)."),
        RPC_PARAM_OPT("topN", "number", "Top-N timers by total inclusive time for timer_stats. Default 20."),
        RPC_PARAM_OPT("outDir", "string", "Output directory. Default: <ProjectSavedDir>/PinWright/insights/<trace-stem>/<runId>/."),
        RPC_PARAM_OPT("format", "string", "Output format. Default 'csv' (only supported value in v1)."),
        RPC_PARAM_OPT("tableEntryLimit", "number", "Max aggregated rows per timer_stats window. Default 5000."),
        RPC_PARAM_OPT("counterDownsampleHz", "number", "Max counter samples/sec (0 = all values). Default 0."),
        RPC_PARAM_OPT("timeoutSeconds", "number", "Wall-clock worker deadline in seconds. Default 300; hard maximum 1800.")
    ))
{
    // ---- format gate (CSV only in v1) ----
    const FString Format = GetStringAlias(Ctx, {TEXT("format")});
    if (!Format.IsEmpty() && !Format.Equals(TEXT("csv"), ESearchCase::IgnoreCase))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("Unsupported format '%s'; only 'csv' is supported in v1"), *Format));
        return true;
    }

    double TimeoutSeconds = kDefaultExportTimeoutSeconds;
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    if (Payload.IsValid() && Payload->HasField(TEXT("timeoutSeconds")) &&
        (!Ctx.RequireNumber(TEXT("timeoutSeconds"), TimeoutSeconds) ||
        !FMath::IsFinite(TimeoutSeconds) || TimeoutSeconds <= 0.0 ||
        TimeoutSeconds > kMaxExportTimeoutSeconds))
    {
        if (FMath::IsFinite(TimeoutSeconds) && TimeoutSeconds > 0.0 &&
            TimeoutSeconds <= kMaxExportTimeoutSeconds)
        {
            return true;
        }
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("timeoutSeconds must be finite, > 0, and <= %.0f seconds"),
                kMaxExportTimeoutSeconds));
        return true;
    }

    // ---- resolve trace path ----
    FString TracePath = GetStringAlias(Ctx, {TEXT("tracePath"), TEXT("trace_path")});
    if (TracePath.IsEmpty())
    {
        TracePath = ResolveDefaultTracePath();
        if (TracePath.IsEmpty())
        {
            Ctx.SendError(ErrorCodes::ERR_TRACE_NOT_FOUND,
                TEXT("No tracePath given and no .utrace found in the UnrealTrace store or project ProfilingDir"));
            return true;
        }
    }
    else
    {
        TracePath = FPaths::ConvertRelativePathToFull(TracePath);
    }
    if (!IFileManager::Get().FileExists(*TracePath))
    {
        Ctx.SendError(ErrorCodes::ERR_TRACE_NOT_FOUND,
            FString::Printf(TEXT("Trace file does not exist: %s"), *TracePath));
        return true;
    }

    // ---- kinds ----
    FExportKinds Kinds;
    if (const TArray<TSharedPtr<FJsonValue>>* KindArr = GetArrayAlias(Ctx, {TEXT("kind"), TEXT("kinds")}))
    {
        for (const TSharedPtr<FJsonValue>& V : *KindArr)
        {
            const FString K = V.IsValid() ? V->AsString().ToLower() : FString();
            if (K == TEXT("timer_stats"))      { Kinds.bTimerStats = true; }
            else if (K == TEXT("frame_series")) { Kinds.bFrameSeries = true; }
            else if (K == TEXT("counters"))     { Kinds.bCounters = true; }
            else if (K == TEXT("timers"))       { Kinds.bTimers = true; }
            else
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                    FString::Printf(TEXT("Unknown kind '%s' (expected timer_stats|frame_series|counters|timers)"), *K));
                return true;
            }
        }
    }
    if (!Kinds.bTimerStats && !Kinds.bFrameSeries && !Kinds.bCounters && !Kinds.bTimers)
    {
        Kinds.bFrameSeries = true; // default
    }

    // ---- windows ----
    TArray<FExportWindow> Windows;
    if (const TArray<TSharedPtr<FJsonValue>>* WinArr = GetArrayAlias(Ctx, {TEXT("windows")}))
    {
        for (const TSharedPtr<FJsonValue>& V : *WinArr)
        {
            const TSharedPtr<FJsonObject>* Obj = nullptr;
            if (V.IsValid() && V->TryGetObject(Obj) && Obj)
            {
                FExportWindow W;
                if (ParseWindow(*Obj, W))
                {
                    Windows.Add(W);
                }
            }
        }
    }
    // Empty => the core resolves a single 'all' window spanning the trace.

    // ---- threads ----
    TArray<FString> ThreadNames;
    TArray<uint32> ThreadIds;
    if (const TArray<TSharedPtr<FJsonValue>>* ThrArr = GetArrayAlias(Ctx, {TEXT("threads")}))
    {
        for (const TSharedPtr<FJsonValue>& V : *ThrArr)
        {
            if (!V.IsValid()) { continue; }
            double NumVal = 0.0;
            if (V->TryGetNumber(NumVal))
            {
                ThreadIds.Add((uint32)NumVal);
            }
            else
            {
                const FString Name = V->AsString();
                if (!Name.IsEmpty())
                {
                    ThreadNames.Add(Name);
                }
            }
        }
    }
    // frame_series-only default thread set: applied when the caller passed no
    // `threads`, and only to frame_series (timer_stats stays "all threads").
    TArray<FString> FrameSeriesThreadNames;
    if (ThreadNames.Num() == 0 && ThreadIds.Num() == 0)
    {
        FrameSeriesThreadNames.Add(TEXT("GameThread"));
        FrameSeriesThreadNames.Add(TEXT("RenderThread"));
        FrameSeriesThreadNames.Add(TEXT("RHIThread"));
    }

    // ---- timer selection ----
    TArray<FString> TimerNames;
    if (const TArray<TSharedPtr<FJsonValue>>* TimArr = GetArrayAlias(Ctx, {TEXT("timers")}))
    {
        for (const TSharedPtr<FJsonValue>& V : *TimArr)
        {
            const FString Name = V.IsValid() ? V->AsString() : FString();
            if (!Name.IsEmpty())
            {
                TimerNames.Add(Name);
            }
        }
    }

    // ---- numeric params ----
    const int32 TopN = (int32)GetNumberAlias(Ctx, {TEXT("topN"), TEXT("top_n")}, 20.0);
    const int32 TableEntryLimit = (int32)GetNumberAlias(Ctx, {TEXT("tableEntryLimit"), TEXT("table_entry_limit")}, 5000.0);
    const double CounterDownsampleHz = GetNumberAlias(Ctx, {TEXT("counterDownsampleHz"), TEXT("counter_downsample_hz")}, 0.0);

    // ---- output directory ----
    FString OutDir = GetStringAlias(Ctx, {TEXT("outDir"), TEXT("out_dir")});
    if (OutDir.IsEmpty())
    {
        const FString Stem = FPaths::GetBaseFilename(TracePath);
        const FString RunId = FString::Printf(TEXT("%s_%s"), *Stem, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        OutDir = FPaths::Combine(FPaths::ProjectSavedDir(),
            TEXT("PinWright"), TEXT("insights"), Stem, RunId);
    }
    OutDir = FPaths::ConvertRelativePathToFull(OutDir);

    // ---- assemble worker request ----
    FExportRequest Req;
    Req.TracePath = TracePath;
    Req.OutDir = OutDir;
    Req.Kinds = Kinds;
    Req.Windows = MoveTemp(Windows);
    Req.ThreadNames = MoveTemp(ThreadNames);
    Req.ThreadIds = MoveTemp(ThreadIds);
    Req.FrameSeriesThreadNames = MoveTemp(FrameSeriesThreadNames);
    Req.TimerNames = MoveTemp(TimerNames);
    Req.TopN = TopN;
    Req.TableEntryLimit = TableEntryLimit;
    Req.CounterDownsampleHz = CounterDownsampleHz;
    Req.TimeoutSeconds = TimeoutSeconds;
    SnapshotOutputFiles(OutDir, Req.PreExistingOutputFiles);
    if (Req.PreExistingOutputFiles.Num() > 0)
    {
        const FString ExistingPath = *Req.PreExistingOutputFiles.CreateConstIterator();
        Ctx.SendError(ErrorCodes::ERR_EXPORT_FAILED,
            FString::Printf(TEXT("Output directory contains a pre-existing export artifact; refusing to overwrite '%s'."),
                *ExistingPath));
        return true;
    }

    const TSharedPtr<FOutputReservation> Reservation = MakeShared<FOutputReservation>(
        FPaths::ConvertRelativePathToFull(OutDir).ToLower());
    if (!Reservation->Acquire())
    {
        Ctx.SendError(ErrorCodes::ERR_EXPORT_FAILED,
            FString::Printf(TEXT("An insights export already owns output directory '%s' in this process."),
                *OutDir));
        return true;
    }

    // Retain only the dispatcher lifetime while the worker is active. The worker
    // never touches the handler context; its completion re-enters through the
    // safe-point ticker before StartJob updates the ticket. Dispatcher teardown
    // marks the shared state abandoned so a late worker completion is dropped.
    const TSharedRef<std::atomic<bool>> Abandoned =
        MakeShared<std::atomic<bool>>(false);
    TSharedPtr<FAsyncRequestLifetimeLease> RequestLease =
        Ctx.RetainAsyncRequestLifetime(
            [Abandoned]()
            {
                Abandoned->store(true, std::memory_order_release);
            });
    const TSharedRef<FTraceExportJob> State = MakeShared<FTraceExportJob>(
        MoveTemp(Req), MoveTemp(RequestLease), Abandoned, Reservation);

    FJobBindArgs Args;
    Args.Method = Ctx.GetMethod();
    Args.StartedPayload = MakeShared<FJsonObject>();
    Args.StartedPayload->SetStringField(
        TEXT("message"), TEXT("insights.export_trace is running on a background worker"));
    Args.BindNativeDelegate = [Ctx, State](FJobOnComplete OnComplete)
    {
        PinWrightSafePoint::DeferJobToSafePoint(Ctx, TEXT("insights.export_trace"),
            [State, OnComplete]() mutable
            {
                State->Start(MoveTemp(OnComplete));
            },
            [State]()
            {
                State->Abandon();
            });
    };
    const FString TicketId = Ctx.StartJob(Args);
    TWeakPtr<FTraceExportJob> WeakState(State);
    FPluginState::Get().GetJobRegistry().SetCancelCallback(
        TicketId,
        [WeakState]()
        {
            if (const TSharedPtr<FTraceExportJob> Pinned = WeakState.Pin())
            {
                Pinned->Cancel();
            }
        });
    return true;
}

#if WITH_DEV_AUTOMATION_TESTS
namespace PinWrightRpc::Insights
{
    bool IsTraceExportWorkerQuiescentForTests()
    {
        return GTraceExportWorkersInFlight.load(std::memory_order_acquire) == 0;
    }
}
#endif
