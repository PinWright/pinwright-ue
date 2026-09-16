// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Debug/TraceExportCore.h"
#include "Handlers/ErrorCodes.h"
#include "Utils/AtomicFileWriter.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Utils/AtomicFileWriterInternal.h"
#endif

#include "Containers/StringConv.h"
#include <limits>
#include <atomic>

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Compat/EngineVersionCompat.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

#include "TraceServices/ITraceServicesModule.h"
#include "TraceServices/AnalysisService.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Counters.h"
#include "TraceServices/Model/Frames.h"
#include "TraceServices/Model/Threads.h"
#include "TraceServices/Model/TimingProfiler.h"
#include "TraceServices/Containers/Tables.h"
#include "TraceServices/Containers/Timelines.h"

namespace PinWrightRpc::TraceExport
{
struct FTraceAnalysis
{
    TSharedPtr<const TraceServices::IAnalysisSession> Session;
};

namespace
{
    constexpr double MsPerSecond = 1000.0;

#if WITH_DEV_AUTOMATION_TESTS
    std::atomic<bool> GInjectWriteFailureForTests(false);
    std::atomic<bool> GForceNoGameFramesForTests(false);
    std::atomic<bool> GForceSlowAnalysisForTests(false);

    AtomicFileWriter::FResult WriteInjectedCsvFailure(
        const FString& FullPath, FStringView Contents)
    {
        const FTCHARToUTF8 Utf8(Contents.GetData(), Contents.Len());
        const TArrayView<const uint8> Bytes(
            reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());

        AtomicFileWriter::Private::FOperations Operations;
        Operations.Stage = [](const FString& TempPath, TArrayView<const uint8> StageBytes)
        {
            const int32 PartialSize = StageBytes.Num() > 0
                ? FMath::Max(1, StageBytes.Num() / 2)
                : 0;
            FFileHelper::SaveArrayToFile(
                TArrayView64<const uint8>(StageBytes.GetData(), PartialSize), *TempPath);
            return AtomicFileWriter::Private::FStageResult{
                AtomicFileWriter::Private::EStageStatus::IoError,
                TEXT("Injected atomic CSV staging failure (error 5: Access is denied.)")};
        };

        return AtomicFileWriter::Private::WriteBytesWithOperationsForTests(
            FullPath, Bytes, AtomicFileWriter::EExistingFilePolicy::ReplaceExisting, Operations);
    }
#endif

    // CSV-escape a single field. Wraps in double quotes and doubles inner quotes
    // when the value contains a comma, quote, or newline.
    FString CsvEscape(const FString& In)
    {
        const bool bNeedsQuote =
            In.Contains(TEXT(",")) || In.Contains(TEXT("\"")) ||
            In.Contains(TEXT("\n")) || In.Contains(TEXT("\r"));
        if (!bNeedsQuote)
        {
            return In;
        }
        FString Escaped = In;
        Escaped.ReplaceInline(TEXT("\""), TEXT("\"\""));
        return FString::Printf(TEXT("\"%s\""), *Escaped);
    }

    // Sanitizes a window name for use inside a filename (timer_stats__<window>.csv).
    FString SanitizeFileToken(const FString& In)
    {
        if (In.IsEmpty())
        {
            return TEXT("window");
        }
        FString Out;
        Out.Reserve(In.Len());
        const TCHAR* const Invalid = TEXT("\\/:*?\"<>| ");
        for (int32 i = 0; i < In.Len(); ++i)
        {
            const TCHAR Ch = In[i];
            bool bBad = FChar::IsControl(Ch);
            for (const TCHAR* P = Invalid; !bBad && *P != TEXT('\0'); ++P)
            {
                bBad = (*P == Ch);
            }
            Out.AppendChar(bBad ? TEXT('_') : Ch);
        }
        return Out;
    }

    // Writes one CSV atomically and appends the absolute path only after verifying
    // that publication produced a non-empty file.
    bool WriteCsvFile(const FString& OutDir, const FString& FileName,
                      const FString& Contents, TArray<FString>& OutFiles,
                      FString& OutError)
    {
        const FString FullPath = FPaths::Combine(OutDir, FileName);
#if WITH_DEV_AUTOMATION_TESTS
        const AtomicFileWriter::FResult WriteResult = GInjectWriteFailureForTests.load(
            std::memory_order_acquire)
            ? WriteInjectedCsvFailure(FullPath, Contents)
            : AtomicFileWriter::WriteUtf8(
                FullPath, Contents, AtomicFileWriter::EExistingFilePolicy::ReplaceExisting);
#else
        const AtomicFileWriter::FResult WriteResult = AtomicFileWriter::WriteUtf8(
            FullPath, Contents, AtomicFileWriter::EExistingFilePolicy::ReplaceExisting);
#endif
        if (!WriteResult.IsSuccess())
        {
            OutError = FString::Printf(TEXT("Failed to write CSV '%s': %s"),
                *FullPath, *WriteResult.Error);
            return false;
        }

        const FString AbsolutePath = FPaths::ConvertRelativePathToFull(FullPath);
        const int64 FileSize = IFileManager::Get().FileSize(*AbsolutePath);
        if (!IFileManager::Get().FileExists(*AbsolutePath) || FileSize <= 0)
        {
            OutError = FString::Printf(
                TEXT("CSV publication did not produce a non-empty file '%s' (size=%lld)."),
                *AbsolutePath, (long long)FileSize);
            return false;
        }

        OutFiles.Add(AbsolutePath);
        return true;
    }

    double FrameEndClamped(const TraceServices::FFrame& Frame, double Duration)
    {
        // The final frame's EndTime can be +inf; clamp to the trace duration.
        return FMath::IsFinite(Frame.EndTime) ? Frame.EndTime : Duration;
    }

    // Resolves requested thread names/ids to a concrete id set, plus an ordered
    // list of (id,name) for stable column ordering in frame_series. When the
    // request selects no threads, all threads are included.
    struct FResolvedThreads
    {
        TSet<uint32> Ids;
        TArray<TPair<uint32, FString>> Ordered; // selection order preserved
        bool bAll = false;
    };

    FResolvedThreads ResolveThreads(const TArray<TPair<uint32, FString>>& AllThreads,
                                    const TArray<FString>& Names,
                                    const TArray<uint32>& Ids)
    {
        FResolvedThreads Result;
        if (Names.Num() == 0 && Ids.Num() == 0)
        {
            Result.bAll = true;
            Result.Ordered = AllThreads;
            for (const TPair<uint32, FString>& T : AllThreads)
            {
                Result.Ids.Add(T.Key);
            }
            return Result;
        }

        for (uint32 Id : Ids)
        {
            for (const TPair<uint32, FString>& T : AllThreads)
            {
                if (T.Key == Id && !Result.Ids.Contains(T.Key))
                {
                    Result.Ids.Add(T.Key);
                    Result.Ordered.Add(T);
                }
            }
        }
        for (const FString& Name : Names)
        {
            for (const TPair<uint32, FString>& T : AllThreads)
            {
                const bool bMatch =
                    T.Value.Equals(Name, ESearchCase::IgnoreCase) ||
                    T.Value.Contains(Name, ESearchCase::IgnoreCase);
                if (bMatch && !Result.Ids.Contains(T.Key))
                {
                    Result.Ids.Add(T.Key);
                    Result.Ordered.Add(T);
                }
            }
        }
        return Result;
    }

    // FTimingProfilerTimer carries its kind per-timer. Mirror the engine's own
    // precedence: Gpu, then Verse, else Cpu (see TraceInsights STimerTreeView /
    // STimersView). UE 5.7 replaced the IsGpuTimer/IsVerseTimer bitfields with a
    // single Type field of the now-public ETimingProfilerTimerType enum.
    const TCHAR* TimerTypeToString(const TraceServices::FTimingProfilerTimer& Timer)
    {
#if UE_VERSION_OLDER_THAN(5, 7, 0)
        if (Timer.IsGpuTimer)
        {
            return TEXT("Gpu");
        }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        // The IsVerseTimer bitfield was added in UE 5.6 (and replaced by the Type enum in 5.7).
        if (Timer.IsVerseTimer)
        {
            return TEXT("Verse");
        }
#endif
        return TEXT("Cpu");
#else
        switch (Timer.Type)
        {
        case TraceServices::ETimingProfilerTimerType::GpuScope:
            return TEXT("Gpu");
        case TraceServices::ETimingProfilerTimerType::VerseSampling:
            return TEXT("Verse");
        default:
            return TEXT("Cpu");
        }
#endif
    }

    // ---- timer_stats ----------------------------------------------------------
    // One aggregation per window (engine sorts by TotalInclusiveTime). When the
    // caller passes explicit timer names we keep only those rows; otherwise the
    // TopN/TableEntryLimit caps bound the output.
    bool DumpTimerStats(const TraceServices::ITimingProfilerProvider& Timing,
                        const FExportRequest& Req,
                        const TArray<FResolvedWindow>& Windows,
                        const FResolvedThreads& Threads,
                        const FString& OutDir,
                        TArray<FString>& OutFiles,
                        TArray<FTruncationEvent>& OutTrunc,
                        FString& OutError,
                        const TFunctionRef<bool()>& ShouldStop)
    {
        // Build a lowercase lookup for explicit timer-name filtering.
        TSet<FString> WantedTimers;
        for (const FString& Name : Req.TimerNames)
        {
            WantedTimers.Add(Name.ToLower());
        }
        const bool bThreadSplit = !Threads.bAll || Req.ThreadNames.Num() > 0 || Req.ThreadIds.Num() > 0;

        for (const FResolvedWindow& Window : Windows)
        {
            if (ShouldStop())
            {
                OutError = TEXT("Export cancelled or timed out during timer_stats enumeration.");
                return false;
            }
            // UE 5.6 renamed the misspelled FCreateAggreationParams to FCreateAggregationParams
            // and added the SortBy / SortOrder / TableEntryLimit fields. On 5.4/5.5 the engine
            // sorts by TotalInclusiveTime descending by default (which the row-emit loop below
            // already assumes), and the TableEntryLimit cap is approximated by the TopN row cap.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
            TraceServices::FCreateAggregationParams Params;
            Params.IntervalStart = Window.StartTime;
            Params.IntervalEnd = Window.EndTime;
            Params.SortBy = TraceServices::FCreateAggregationParams::ESortBy::TotalInclusiveTime;
            Params.SortOrder = TraceServices::FCreateAggregationParams::ESortOrder::Descending;
            Params.TableEntryLimit = Req.TableEntryLimit > 0 ? Req.TableEntryLimit : 0;
#else
            TraceServices::FCreateAggreationParams Params;
            Params.IntervalStart = Window.StartTime;
            Params.IntervalEnd = Window.EndTime;
            Params.IncludeGpu = true;
#endif
            // CreateAggregation includes CPU timelines ONLY when CpuThreadFilter is
            // set (TimingProfiler.cpp:907 — a null filter skips all CPU threads), so
            // always provide one: accept-all for the "all threads" case, else the id set.
            if (Threads.bAll)
            {
                Params.CpuThreadFilter = [](uint32) { return true; };
            }
            else
            {
                const TSet<uint32>& IdSet = Threads.Ids;
                Params.CpuThreadFilter = [IdSet](uint32 ThreadId) { return IdSet.Contains(ThreadId); };
            }

            // Caller owns the returned table.
            TUniquePtr<TraceServices::ITable<TraceServices::FTimingProfilerAggregatedStats>> Table(
                Timing.CreateAggregation(Params));
            if (!Table.IsValid())
            {
                continue;
            }

            FString Csv = TEXT("window,thread,timer,count,incl_total_ms,incl_avg_ms,incl_min_ms,incl_max_ms,incl_median_ms,excl_total_ms,excl_avg_ms\n");
            // thread column: "*" when no thread filter; the single thread name when one
            // thread is selected; otherwise a '+'-joined list of the selected names
            // (rows are aggregated across all filtered threads, so this is the set).
            FString ThreadCol;
            if (!bThreadSplit)
            {
                ThreadCol = TEXT("*");
            }
            else if (Threads.Ordered.Num() == 1)
            {
                ThreadCol = Threads.Ordered[0].Value;
            }
            else
            {
                TArray<FString> SelectedNames;
                for (const TPair<uint32, FString>& T : Threads.Ordered)
                {
                    SelectedNames.Add(T.Value);
                }
                ThreadCol = FString::Join(SelectedNames, TEXT("+"));
            }

            // CreateAggregation gives no dropped count when it saturates TableEntryLimit;
            // detect saturation (rows == limit) and report it as truncation.
            if (Req.TableEntryLimit > 0 && (int64)Table->GetRowCount() >= Req.TableEntryLimit)
            {
                FTruncationEvent Ev;
                Ev.Kind = TEXT("timer_stats");
                Ev.Window = Window.Name;
                Ev.Reason = TEXT(">= tableEntryLimit rows aggregated; additional timers MAY have been dropped (engine does not surface the total)");
                Ev.Kept = (int64)Table->GetRowCount();
                Ev.Dropped = -1; // unknown: engine does not surface the dropped count
                Ev.Limit = Req.TableEntryLimit;
                OutTrunc.Add(Ev);
                UE_LOG(LogTemp, Warning,
                    TEXT("insights.export_trace: timer_stats window '%s' hit tableEntryLimit=%d (dropped count unknown)"),
                    *Window.Name, Req.TableEntryLimit);
            }

            int64 Kept = 0;
            const TUniquePtr<TraceServices::ITableReader<TraceServices::FTimingProfilerAggregatedStats>> Reader(
                Table->CreateReader());
            for (; Reader.IsValid() && Reader->IsValid(); Reader->NextRow())
            {
                if (ShouldStop())
                {
                    OutError = TEXT("Export cancelled or timed out during timer_stats enumeration.");
                    return false;
                }
                const TraceServices::FTimingProfilerAggregatedStats* Row = Reader->GetCurrentRow();
                if (!Row || !Row->Timer)
                {
                    continue;
                }
                const FString TimerName = Row->Timer->Name ? FString(Row->Timer->Name) : FString();
                if (WantedTimers.Num() > 0 && !WantedTimers.Contains(TimerName.ToLower()))
                {
                    continue;
                }
                Csv += FString::Printf(TEXT("%s,%s,%s,%llu,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n"),
                    *CsvEscape(Window.Name),
                    *CsvEscape(ThreadCol),
                    *CsvEscape(TimerName),
                    (unsigned long long)Row->InstanceCount,
                    Row->TotalInclusiveTime * MsPerSecond,
                    Row->AverageInclusiveTime * MsPerSecond,
                    Row->MinInclusiveTime * MsPerSecond,
                    Row->MaxInclusiveTime * MsPerSecond,
                    Row->MedianInclusiveTime * MsPerSecond,
                    Row->TotalExclusiveTime * MsPerSecond,
                    Row->AverageExclusiveTime * MsPerSecond);
                ++Kept;

                // Apply TopN as a hard cap on emitted rows (engine sorted descending).
                if (WantedTimers.Num() == 0 && Req.TopN > 0 && Kept >= Req.TopN)
                {
                    Reader->NextRow();
                    if (Reader->IsValid())
                    {
                        // Count how many remain to report honest truncation.
                        int64 Dropped = 0;
                        for (; Reader->IsValid(); Reader->NextRow())
                        {
                            if (ShouldStop())
                            {
                                OutError = TEXT("Export cancelled or timed out during timer_stats truncation scan.");
                                return false;
                            }
                            ++Dropped;
                        }
                        FTruncationEvent Ev;
                        Ev.Kind = TEXT("timer_stats");
                        Ev.Window = Window.Name;
                        Ev.Reason = TEXT("topN cap on rows");
                        Ev.Kept = Kept;
                        Ev.Dropped = Dropped;
                        Ev.Limit = Req.TopN;
                        OutTrunc.Add(Ev);
                        UE_LOG(LogTemp, Warning,
                            TEXT("insights.export_trace: timer_stats window '%s' truncated by topN=%d (kept %lld, dropped %lld)"),
                            *Window.Name, Req.TopN, (long long)Kept, (long long)Dropped);
                    }
                    break;
                }
            }

            const FString FileName = FString::Printf(TEXT("timer_stats__%s.csv"), *SanitizeFileToken(Window.Name));
            if (!WriteCsvFile(OutDir, FileName, Csv, OutFiles, OutError))
            {
                return false;
            }
        }
        return true;
    }

    // ---- frame_series ---------------------------------------------------------
    // Per-frame, per-thread busy_ms and span_ms. ONE depth-0 EnumerateEvents pass
    // per thread over the whole trace, boundary-split into the frame each event
    // overlaps (mirrors TraceInsights FrameStatsHelper, reimplemented here).
    bool DumpFrameSeries(const TraceServices::ITimingProfilerProvider& Timing,
                         const TraceServices::IFrameProvider& Frames,
                         const FExportRequest& Req,
                         double Duration,
                         const FResolvedThreads& Threads,
                         const FString& OutDir,
                         TArray<FString>& OutFiles,
                         FString& OutError,
                         const TFunctionRef<bool()>& ShouldStop)
    {
        // Collect the Game-frame list once; clamp the last frame's end.
        TArray<TraceServices::FFrame> FrameList;
        bool bAbort = false;
        const uint64 FrameCount = Frames.GetFrameCount(TraceFrameType_Game);
        FrameList.Reserve(FrameCount);
        Frames.EnumerateFrames(TraceFrameType_Game, 0, FrameCount,
            [&FrameList, &bAbort, &ShouldStop](const TraceServices::FFrame& Frame)
            {
                if (ShouldStop())
                {
                    bAbort = true;
                    return;
                }
                FrameList.Add(Frame);
            });
        if (bAbort)
        {
            OutError = TEXT("Export cancelled or timed out during frame enumeration.");
            return false;
        }
#if WITH_DEV_AUTOMATION_TESTS
        if (GForceNoGameFramesForTests.load(std::memory_order_acquire))
        {
            FrameList.Reset();
        }
#endif
        if (FrameList.Num() == 0)
        {
            const FString ExpectedPath = FPaths::ConvertRelativePathToFull(
                FPaths::Combine(OutDir, TEXT("frame_series.csv")));
            OutError = FString::Printf(
                TEXT("Requested frame_series artifact '%s' could not be produced: trace has no Game frames."),
                *ExpectedPath);
            return false;
        }

        const int32 NumFrames = FrameList.Num();
        const int32 NumThreads = Threads.Ordered.Num();

        // Per (thread, frame): busy accumulator + span min/max (init invalid).
        TArray<double> Busy;
        TArray<double> SpanMin;
        TArray<double> SpanMax;
        Busy.Init(0.0, NumThreads * NumFrames);
        SpanMin.Init(DBL_MAX, NumThreads * NumFrames);
        SpanMax.Init(-DBL_MAX, NumThreads * NumFrames);

        for (int32 ThreadIdx = 0; ThreadIdx < NumThreads; ++ThreadIdx)
        {
            if (ShouldStop())
            {
                OutError = TEXT("Export cancelled or timed out during frame_series enumeration.");
                return false;
            }
            uint32 TimelineIndex = 0;
            if (!Timing.GetCpuThreadTimelineIndex(Threads.Ordered[ThreadIdx].Key, TimelineIndex))
            {
                continue; // thread has no CPU timeline
            }

            Timing.ReadTimeline(TimelineIndex,
                [&](const TraceServices::ITimingProfilerProvider::Timeline& Timeline)
                {
                    Timeline.EnumerateEvents(0.0, Duration,
                        [&](double EventStart, double EventEnd, uint32 Depth,
                            const TraceServices::FTimingProfilerEvent& /*Event*/)
                        {
                            if (ShouldStop())
                            {
                                return TraceServices::EEventEnumerate::Stop;
                            }
                            if (Depth != 0)
                            {
                                return TraceServices::EEventEnumerate::Continue;
                            }
                            const double EvEnd = FMath::IsFinite(EventEnd) ? EventEnd : Duration;

                            // Find the first/last frame this event overlaps via timestamps.
                            const uint32 FirstFrame = Frames.GetFrameNumberForTimestamp(TraceFrameType_Game, EventStart);
                            const uint32 LastFrame = Frames.GetFrameNumberForTimestamp(TraceFrameType_Game, EvEnd);
                            const int32 Lo = FMath::Clamp((int32)FirstFrame, 0, NumFrames - 1);
                            const int32 Hi = FMath::Clamp((int32)LastFrame, 0, NumFrames - 1);

                            for (int32 F = Lo; F <= Hi; ++F)
                            {
                                if (ShouldStop())
                                {
                                    return TraceServices::EEventEnumerate::Stop;
                                }
                                const TraceServices::FFrame& Frame = FrameList[F];
                                const double FrameEnd = FrameEndClamped(Frame, Duration);
                                const double OverlapStart = FMath::Max(EventStart, Frame.StartTime);
                                const double OverlapEnd = FMath::Min(EvEnd, FrameEnd);
                                if (OverlapEnd <= OverlapStart)
                                {
                                    continue;
                                }
                                const int32 Slot = ThreadIdx * NumFrames + F;
                                Busy[Slot] += (OverlapEnd - OverlapStart);
                                SpanMin[Slot] = FMath::Min(SpanMin[Slot], OverlapStart);
                                SpanMax[Slot] = FMath::Max(SpanMax[Slot], OverlapEnd);
                            }
                            return TraceServices::EEventEnumerate::Continue;
                        });
                });
            if (ShouldStop())
            {
                OutError = TEXT("Export cancelled or timed out during frame_series enumeration.");
                return false;
            }
        }

        // Header: frame_index,start_time,end_time,wall_ms, then per-thread pair.
        // CSV-escape the full "<thread>_busy_ms" / "<thread>_span_ms" tokens so a
        // thread name with an embedded comma/quote (e.g. "Foreground Worker #3")
        // cannot corrupt the header row.
        FString Csv = TEXT("frame_index,start_time,end_time,wall_ms");
        for (const TPair<uint32, FString>& T : Threads.Ordered)
        {
            Csv += FString::Printf(TEXT(",%s,%s"),
                *CsvEscape(T.Value + TEXT("_busy_ms")),
                *CsvEscape(T.Value + TEXT("_span_ms")));
        }
        Csv += TEXT("\n");

        for (int32 F = 0; F < NumFrames; ++F)
        {
            if (ShouldStop())
            {
                OutError = TEXT("Export cancelled or timed out before frame_series publication.");
                return false;
            }
            const TraceServices::FFrame& Frame = FrameList[F];
            const double FrameEnd = FrameEndClamped(Frame, Duration);
            Csv += FString::Printf(TEXT("%llu,%.6f,%.6f,%.6f"),
                (unsigned long long)Frame.Index,
                Frame.StartTime,
                FrameEnd,
                (FrameEnd - Frame.StartTime) * MsPerSecond);

            for (int32 ThreadIdx = 0; ThreadIdx < NumThreads; ++ThreadIdx)
            {
                const int32 Slot = ThreadIdx * NumFrames + F;
                const double BusyMs = Busy[Slot] * MsPerSecond;
                const double SpanMs = (SpanMax[Slot] >= SpanMin[Slot])
                    ? (SpanMax[Slot] - SpanMin[Slot]) * MsPerSecond
                    : 0.0;
                Csv += FString::Printf(TEXT(",%.6f,%.6f"), BusyMs, SpanMs);
            }
            Csv += TEXT("\n");
        }

        return WriteCsvFile(OutDir, TEXT("frame_series.csv"), Csv, OutFiles, OutError);
    }

    // ---- counters -------------------------------------------------------------
    // Whole-trace (or windowed) counter value series. Dispatch on IsFloatingPoint
    // to use the lossless callback. Optional time-based downsample.
    bool DumpCounters(const TraceServices::ICounterProvider& Counters,
                      const FExportRequest& Req,
                      const TArray<FResolvedWindow>& Windows,
                      const FString& OutDir,
                      TArray<FString>& OutFiles,
                      FString& OutError,
                      const TFunctionRef<bool()>& ShouldStop)
    {
        FString Csv = TEXT("counter,time,value,type\n");
        bool bAbort = false;

        // The synthesized default window => whole-trace unbounded scan. Otherwise scan
        // each window with external bounds to capture the left-edge sample. Keyed on the
        // default-window flag, not the name, so a caller window literally named "all"
        // with real bounds is honored rather than silently scanned unbounded.
        const bool bWholeTrace = (Windows.Num() == 1 && Windows[0].bIsDefaultAllWindow);
        const double MinInterval = (Req.CounterDownsampleHz > 0.0) ? (1.0 / Req.CounterDownsampleHz) : 0.0;

        Counters.EnumerateCounters(
            [&](uint32 /*Id*/, const TraceServices::ICounter& Counter)
            {
                if (ShouldStop())
                {
                    bAbort = true;
                    return;
                }
                const FString Name = Counter.GetName() ? FString(Counter.GetName()) : FString();
                const bool bFloat = Counter.IsFloatingPoint();
                const TCHAR* TypeStr = bFloat ? TEXT("double") : TEXT("int");

                auto EmitRows = [&](double Start, double End, bool bExternal)
                {
                    double LastEmitted = -DBL_MAX;
                    if (bFloat)
                    {
                        Counter.EnumerateFloatValues(Start, End, bExternal,
                            [&](double Time, double Value)
                            {
                                if (ShouldStop())
                                {
                                    bAbort = true;
                                    return;
                                }
                                if (MinInterval > 0.0 && Time - LastEmitted < MinInterval)
                                {
                                    return;
                                }
                                LastEmitted = Time;
                                Csv += FString::Printf(TEXT("%s,%.6f,%.6f,%s\n"),
                                    *CsvEscape(Name), Time, Value, TypeStr);
                            });
                    }
                    else
                    {
                        Counter.EnumerateValues(Start, End, bExternal,
                            [&](double Time, int64 Value)
                            {
                                if (ShouldStop())
                                {
                                    bAbort = true;
                                    return;
                                }
                                if (MinInterval > 0.0 && Time - LastEmitted < MinInterval)
                                {
                                    return;
                                }
                                LastEmitted = Time;
                                Csv += FString::Printf(TEXT("%s,%.6f,%lld,%s\n"),
                                    *CsvEscape(Name), Time, (long long)Value, TypeStr);
                            });
                    }
                };

                if (bWholeTrace)
                {
                    constexpr double Inf = std::numeric_limits<double>::infinity();
                    EmitRows(-Inf, Inf, /*bExternal=*/false);
                }
                else
                {
                    for (const FResolvedWindow& Window : Windows)
                    {
                        if (ShouldStop())
                        {
                            bAbort = true;
                            return;
                        }
                        EmitRows(Window.StartTime, Window.EndTime, /*bExternal=*/true);
                    }
                }
            });

        if (bAbort || ShouldStop())
        {
            OutError = TEXT("Export cancelled or timed out during counters enumeration.");
            return false;
        }
        return WriteCsvFile(OutDir, TEXT("counters.csv"), Csv, OutFiles, OutError);
    }

    // ---- timers + threads listings -------------------------------------------
    bool DumpTimers(const TraceServices::ITimingProfilerProvider& Timing,
                    const TraceServices::IThreadProvider& ThreadProvider,
                    const FString& OutDir,
                    TArray<FString>& OutFiles,
                    FString& OutError,
                    const TFunctionRef<bool()>& ShouldStop)
    {
        FString TimersCsv = TEXT("timer_id,name,type,file,line\n");
        const auto AppendTimerRows =
            [&TimersCsv, &ShouldStop](const TraceServices::ITimingProfilerTimerReader& Reader)
            {
                const uint32 Count = Reader.GetTimerCount();
                for (uint32 Index = 0; Index < Count; ++Index)
                {
                    if (ShouldStop())
                    {
                        return;
                    }
                    const TraceServices::FTimingProfilerTimer* Timer = Reader.GetTimer(Index);
                    if (!Timer)
                    {
                        continue;
                    }
                    const FString Name = Timer->Name ? FString(Timer->Name) : FString();
                    const FString File = Timer->File ? FString(Timer->File) : FString();
                    TimersCsv += FString::Printf(TEXT("%u,%s,%s,%s,%u\n"),
                        Timer->Id,
                        *CsvEscape(Name),
                        TimerTypeToString(*Timer),
                        *CsvEscape(File),
                        Timer->Line);
                }
            };
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        // 5.8 deprecated ReadTimers(Callback) in favor of the direct reader accessor.
        // Callers are already inside a session read scope (see the analysis entry points).
        AppendTimerRows(Timing.GetTimerReader());
#else
        Timing.ReadTimers(AppendTimerRows);
#endif
        if (ShouldStop())
        {
            OutError = TEXT("Export cancelled or timed out during timers enumeration.");
            return false;
        }
        if (!WriteCsvFile(OutDir, TEXT("timers.csv"), TimersCsv, OutFiles, OutError))
        {
            return false;
        }

        FString ThreadsCsv = TEXT("thread_id,name,group\n");
        ThreadProvider.EnumerateThreads(
            [&ThreadsCsv, &ShouldStop](const TraceServices::FThreadInfo& Info)
            {
                if (ShouldStop())
                {
                    return;
                }
                const FString Name = Info.Name ? FString(Info.Name) : FString();
                const FString Group = Info.GroupName ? FString(Info.GroupName) : FString();
                ThreadsCsv += FString::Printf(TEXT("%u,%s,%s\n"),
                    Info.Id, *CsvEscape(Name), *CsvEscape(Group));
            });
        if (ShouldStop())
        {
            OutError = TEXT("Export cancelled or timed out during threads enumeration.");
            return false;
        }
        return WriteCsvFile(OutDir, TEXT("threads.csv"), ThreadsCsv, OutFiles, OutError);
    }

    FExportResult MakeFailure(const FString& Code, const FString& Message)
    {
        FExportResult R;
        R.bSuccess = false;
        R.Error = Code;
        R.Message = Message;
        return R;
    }

    FExportResult MakeFailureWithFiles(const FString& Code, const FString& Message,
        TArray<FString>& Files)
    {
        FExportResult R = MakeFailure(Code, Message);
        R.PublishedFiles = MoveTemp(Files);
        return R;
    }
}

FExportResult StartTraceAnalysis(const FExportRequest& Request,
    TSharedPtr<FTraceAnalysis>& OutAnalysis)
{
    OutAnalysis.Reset();
    if (!IFileManager::Get().FileExists(*Request.TracePath))
    {
        return MakeFailure(ErrorCodes::ERR_TRACE_NOT_FOUND,
            FString::Printf(TEXT("Trace file does not exist: %s"), *Request.TracePath));
    }

    ITraceServicesModule& TraceModule =
        FModuleManager::LoadModuleChecked<ITraceServicesModule>("TraceServices");
    TSharedPtr<TraceServices::IAnalysisService> AnalysisService = TraceModule.GetAnalysisService();
    if (!AnalysisService.IsValid())
    {
        return MakeFailure(ErrorCodes::ERR_ANALYSIS_FAILED,
            TEXT("TraceServices analysis service unavailable"));
    }

    // StartAnalysis schedules the engine parser and returns immediately. The
    // handler polls the returned session from its worker before reading providers.
    TSharedPtr<const TraceServices::IAnalysisSession> Session =
        AnalysisService->StartAnalysis(*Request.TracePath);
    if (!Session.IsValid())
    {
        return MakeFailure(ErrorCodes::ERR_ANALYSIS_FAILED,
            FString::Printf(TEXT("Failed to analyze trace: %s"), *Request.TracePath));
    }

    OutAnalysis = MakeShared<FTraceAnalysis>();
    OutAnalysis->Session = MoveTemp(Session);
    FExportResult Started;
    Started.bSuccess = true;
    return Started;
}

bool IsTraceAnalysisComplete(const TSharedPtr<FTraceAnalysis>& Analysis)
{
#if WITH_DEV_AUTOMATION_TESTS
    if (GForceSlowAnalysisForTests.load(std::memory_order_acquire))
    {
        FPlatformProcess::Sleep(0.01f);
        return false;
    }
#endif
    return Analysis.IsValid() && Analysis->Session.IsValid() &&
        Analysis->Session->IsAnalysisComplete();
}

void ReleaseTraceAnalysisOnWorker(TSharedPtr<FTraceAnalysis>& Analysis)
{
    if (!Analysis.IsValid())
    {
        return;
    }

    TSharedPtr<const TraceServices::IAnalysisSession> Session =
        MoveTemp(Analysis->Session);
    Analysis.Reset();
    if (Session.IsValid())
    {
        // bAndWait keeps parser/provider teardown on this worker. The game-thread
        // continuation must only publish the already-built result and release its
        // dispatcher lease.
        Session->Stop(/*bAndWait=*/true);
    }
}

FExportResult ExportTraceAnalysis(const FExportRequest& Request,
    const TSharedPtr<FTraceAnalysis>& Analysis,
    const TSharedRef<std::atomic<bool>>& CancelRequested,
    const double DeadlineSeconds)
{
    if (!Analysis.IsValid() || !Analysis->Session.IsValid())
    {
        return MakeFailure(ErrorCodes::ERR_ANALYSIS_FAILED,
            TEXT("TraceServices analysis session is unavailable"));
    }
    if (!Analysis->Session->IsAnalysisComplete())
    {
        return MakeFailure(ErrorCodes::ERR_ANALYSIS_FAILED,
            TEXT("TraceServices analysis was not complete before export"));
    }

    const TSharedPtr<const TraceServices::IAnalysisSession>& Session = Analysis->Session;
    const auto ShouldStop = [&CancelRequested, DeadlineSeconds]()
    {
        return CancelRequested->load(std::memory_order_acquire) ||
            FPlatformTime::Seconds() >= DeadlineSeconds;
    };
    TArray<FString> Files;
    TArray<FString> ExpectedFiles;
    TArray<FTruncationEvent> Truncations;
    TArray<FResolvedWindow> ResolvedWindows;
    double Duration = 0.0;
    uint64 GameFrameCount = 0;
    int32 ThreadCount = 0;

    {
        // Single read scope guarding ALL provider access for the whole dump.
        TraceServices::FAnalysisSessionReadScope ReadScope(*Session);

        Duration = Session->GetDurationSeconds();

        // Resolve windows: clamp ends to the trace duration; default span the trace.
        if (Request.Windows.Num() == 0)
        {
            FResolvedWindow W;
            W.Name = TEXT("all");
            W.StartTime = 0.0;
            W.EndTime = Duration;
            W.bIsDefaultAllWindow = true;
            ResolvedWindows.Add(W);
        }
        else
        {
            for (const FExportWindow& In : Request.Windows)
            {
                FResolvedWindow W;
                W.Name = In.Name;
                W.StartTime = FMath::Max(0.0, In.StartTime);
                W.EndTime = (In.EndTime <= 0.0) ? Duration : FMath::Min(In.EndTime, Duration);
                ResolvedWindows.Add(W);
            }
        }

        if (Request.Kinds.bTimerStats)
        {
            for (const FResolvedWindow& Window : ResolvedWindows)
            {
                ExpectedFiles.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(
                    Request.OutDir,
                    FString::Printf(TEXT("timer_stats__%s.csv"), *SanitizeFileToken(Window.Name)))));
            }
        }
        if (Request.Kinds.bFrameSeries)
        {
            ExpectedFiles.Add(FPaths::ConvertRelativePathToFull(
                FPaths::Combine(Request.OutDir, TEXT("frame_series.csv"))));
        }
        if (Request.Kinds.bCounters)
        {
            ExpectedFiles.Add(FPaths::ConvertRelativePathToFull(
                FPaths::Combine(Request.OutDir, TEXT("counters.csv"))));
        }
        if (Request.Kinds.bTimers)
        {
            ExpectedFiles.Add(FPaths::ConvertRelativePathToFull(
                FPaths::Combine(Request.OutDir, TEXT("timers.csv"))));
            ExpectedFiles.Add(FPaths::ConvertRelativePathToFull(
                FPaths::Combine(Request.OutDir, TEXT("threads.csv"))));
        }

        const TraceServices::IThreadProvider& ThreadProvider = TraceServices::ReadThreadProvider(*Session);
        const TraceServices::IFrameProvider& FrameProvider = TraceServices::ReadFrameProvider(*Session);
        GameFrameCount = FrameProvider.GetFrameCount(TraceFrameType_Game);

        // Snapshot all threads once (pointers are scope-bound; copy names now).
        TArray<TPair<uint32, FString>> AllThreads;
        ThreadProvider.EnumerateThreads(
            [&AllThreads](const TraceServices::FThreadInfo& Info)
            {
                AllThreads.Emplace(Info.Id, Info.Name ? FString(Info.Name) : FString());
            });

        // Shared (timer_stats) selection — "all" when the caller left threads unset.
        FResolvedThreads Threads = ResolveThreads(AllThreads, Request.ThreadNames, Request.ThreadIds);
        ThreadCount = AllThreads.Num();

        // frame_series uses its own selection: the handler seeds GT/RT/RHI here when
        // the caller passed no `threads`, leaving timer_stats unconstrained.
        const TArray<FString>& FrameNames =
            Request.FrameSeriesThreadNames.Num() > 0 ? Request.FrameSeriesThreadNames : Request.ThreadNames;
        const TArray<uint32>& FrameIds =
            Request.FrameSeriesThreadIds.Num() > 0 ? Request.FrameSeriesThreadIds : Request.ThreadIds;
        FResolvedThreads FrameThreads = ResolveThreads(AllThreads, FrameNames, FrameIds);

        const TraceServices::ITimingProfilerProvider* Timing = TraceServices::ReadTimingProfilerProvider(*Session);

        const bool bNeedsTiming = Request.Kinds.bTimerStats || Request.Kinds.bFrameSeries || Request.Kinds.bTimers;
        if (bNeedsTiming && !Timing)
        {
            // Early return mid-scope: the ReadScope destructor releases the read lock here.
            return MakeFailure(ErrorCodes::ERR_NO_TIMING_DATA,
                TEXT("Trace has no CPU timing data (timer_stats/frame_series/timers require the 'cpu' channel)"));
        }

        FString ExportError;
        if (ShouldStop())
        {
            return MakeFailureWithFiles(ErrorCodes::ERR_EXPORT_FAILED,
                TEXT("Export cancelled or timed out before CSV publication."), Files);
        }
        if (Request.Kinds.bTimerStats && Timing)
        {
            if (!DumpTimerStats(*Timing, Request, ResolvedWindows, Threads, Request.OutDir,
                    Files, Truncations, ExportError, ShouldStop))
            {
                return MakeFailureWithFiles(ErrorCodes::ERR_EXPORT_FAILED, ExportError, Files);
            }
        }
        if (ShouldStop())
        {
            return MakeFailureWithFiles(ErrorCodes::ERR_EXPORT_FAILED,
                TEXT("Export cancelled or timed out during CSV publication."), Files);
        }
        if (Request.Kinds.bFrameSeries && Timing)
        {
            if (!DumpFrameSeries(*Timing, FrameProvider, Request, Duration, FrameThreads,
                    Request.OutDir, Files, ExportError, ShouldStop))
            {
                return MakeFailureWithFiles(ErrorCodes::ERR_EXPORT_FAILED, ExportError, Files);
            }
        }
        if (ShouldStop())
        {
            return MakeFailureWithFiles(ErrorCodes::ERR_EXPORT_FAILED,
                TEXT("Export cancelled or timed out during CSV publication."), Files);
        }
        if (Request.Kinds.bCounters)
        {
            const TraceServices::ICounterProvider& CounterProvider = TraceServices::ReadCounterProvider(*Session);
            if (!DumpCounters(CounterProvider, Request, ResolvedWindows, Request.OutDir,
                    Files, ExportError, ShouldStop))
            {
                return MakeFailureWithFiles(ErrorCodes::ERR_EXPORT_FAILED, ExportError, Files);
            }
        }
        if (ShouldStop())
        {
            return MakeFailureWithFiles(ErrorCodes::ERR_EXPORT_FAILED,
                TEXT("Export cancelled or timed out during CSV publication."), Files);
        }
        if (Request.Kinds.bTimers && Timing)
        {
            if (!DumpTimers(*Timing, ThreadProvider, Request.OutDir, Files, ExportError, ShouldStop))
            {
                return MakeFailureWithFiles(ErrorCodes::ERR_EXPORT_FAILED, ExportError, Files);
            }
        }

        for (const FString& ExpectedFile : ExpectedFiles)
        {
            if (!Files.Contains(ExpectedFile))
            {
                return MakeFailureWithFiles(ErrorCodes::ERR_EXPORT_FAILED,
                    FString::Printf(TEXT("Requested export artifact was not produced: '%s'."),
                        *ExpectedFile), Files);
            }
        }
    }

    // Build the result JSON (UObject-free; safe on the worker thread).
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

    TArray<TSharedPtr<FJsonValue>> FilesArr;
    for (const FString& File : Files)
    {
        FilesArr.Add(MakeShared<FJsonValueString>(File));
    }
    Result->SetArrayField(TEXT("files"), FilesArr);

    TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
    Summary->SetStringField(TEXT("tracePath"), Request.TracePath);
    Summary->SetNumberField(TEXT("durationSeconds"), Duration);
    Summary->SetNumberField(TEXT("frameCount"), (double)GameFrameCount);
    Summary->SetNumberField(TEXT("threadCount"), ThreadCount);
    TArray<TSharedPtr<FJsonValue>> WindowsArr;
    for (const FResolvedWindow& W : ResolvedWindows)
    {
        TSharedPtr<FJsonObject> WObj = MakeShared<FJsonObject>();
        WObj->SetStringField(TEXT("name"), W.Name);
        WObj->SetNumberField(TEXT("start"), W.StartTime);
        WObj->SetNumberField(TEXT("end"), W.EndTime);
        WindowsArr.Add(MakeShared<FJsonValueObject>(WObj));
    }
    Summary->SetArrayField(TEXT("windows"), WindowsArr);
    Result->SetObjectField(TEXT("summary"), Summary);

    TSharedPtr<FJsonObject> Trunc = MakeShared<FJsonObject>();
    Trunc->SetBoolField(TEXT("truncated"), Truncations.Num() > 0);
    TArray<TSharedPtr<FJsonValue>> TruncArr;
    for (const FTruncationEvent& Ev : Truncations)
    {
        TSharedPtr<FJsonObject> EObj = MakeShared<FJsonObject>();
        EObj->SetStringField(TEXT("kind"), Ev.Kind);
        EObj->SetStringField(TEXT("window"), Ev.Window);
        EObj->SetStringField(TEXT("reason"), Ev.Reason);
        EObj->SetNumberField(TEXT("kept"), (double)Ev.Kept);
        EObj->SetNumberField(TEXT("dropped"), (double)Ev.Dropped);
        EObj->SetNumberField(TEXT("limit"), (double)Ev.Limit);
        TruncArr.Add(MakeShared<FJsonValueObject>(EObj));
    }
    Trunc->SetArrayField(TEXT("events"), TruncArr);
    Result->SetObjectField(TEXT("truncation"), Trunc);

    FExportResult Out;
    Out.bSuccess = true;
    Out.ResultJson = Result;
    Out.PublishedFiles = Files;
    return Out;
}

#if WITH_DEV_AUTOMATION_TESTS
void SetWriteFailureInjectionForTests(const bool bEnabled)
{
    GInjectWriteFailureForTests.store(bEnabled, std::memory_order_release);
}

void SetForceNoGameFramesForTests(const bool bEnabled)
{
    GForceNoGameFramesForTests.store(bEnabled, std::memory_order_release);
}

void SetForceSlowAnalysisForTests(const bool bEnabled)
{
    GForceSlowAnalysisForTests.store(bEnabled, std::memory_order_release);
}
#endif

} // namespace PinWrightRpc::TraceExport
