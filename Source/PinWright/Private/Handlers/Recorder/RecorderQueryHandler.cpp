// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "RecorderResolver.h"
#include "State/PluginState.h"
#include "Dispatch/SafePoint.h"

#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "IPythonScriptPlugin.h"
#include "Compat/EngineVersionCompat.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include <atomic>

namespace
{
    // Sentinel pair bracketing the JSON the wrapper prints, so we can scrape it
    // out of the child process output.
    const TCHAR* kResultBegin = TEXT("__PW_RECORDER_QUERY_BEGIN__");
    const TCHAR* kResultEnd = TEXT("__PW_RECORDER_QUERY_END__");
    constexpr double kDefaultQueryTimeoutSeconds = 30.0;
    constexpr double kMinQueryTimeoutSeconds = 0.1;
    constexpr double kMaxQueryTimeoutSeconds = 60.0;
    constexpr double kQueryTerminationHardMaxSeconds = 5.0;
    constexpr float kQueryPollIntervalSeconds = 0.05f;
    constexpr int32 kMaxQueryOutputCharacters = 1024 * 1024;

    enum class ESessionResolution : uint8
    {
        NotFound,
        Resolved,
        Ambiguous
    };

    enum class EQueryTerminationReason : uint8
    {
        None,
        Cancelled,
        TimedOut
    };

    struct FSessionResolution
    {
        ESessionResolution Status = ESessionResolution::NotFound;
        FString Path;
        TArray<FString> Candidates;
    };

    // Exact path/filename/id resolution wins. The legacy substring convenience is retained only
    // when it identifies one file; returning the first filesystem enumeration result is not a
    // stable session selection policy.
    FSessionResolution ResolveSessionPath(const FString& Session)
    {
        FSessionResolution Result;
        const FString ExactPath = RecorderResolver::ResolvePath(Session);
        if (!ExactPath.IsEmpty())
        {
            Result.Status = ESessionResolution::Resolved;
            Result.Path = ExactPath;
            return Result;
        }

        IFileManager& FM = IFileManager::Get();
        const FString RecordingsDir = RecorderResolver::RecordingsDir();
        TArray<FString> Files;
        FM.FindFiles(Files, *FPaths::Combine(RecordingsDir, TEXT("*.ndjson")), /*Files=*/true, /*Directories=*/false);

        TArray<FString> Matches;
        for (const FString& File : Files)
        {
            if (File.Contains(Session, ESearchCase::IgnoreCase))
            {
                Matches.Add(File);
            }
        }

        if (Matches.Num() == 1)
        {
            Result.Status = ESessionResolution::Resolved;
            Result.Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(RecordingsDir, Matches[0]));
            return Result;
        }

        if (Matches.Num() > 1)
        {
            Matches.Sort();
            Result.Status = ESessionResolution::Ambiguous;
            for (const FString& Match : Matches)
            {
                Result.Candidates.Add(FPaths::GetBaseFilename(Match));
            }
        }
        return Result;
    }

    FString MakeTempScriptPath()
    {
        const FString TempDir = FPaths::ProjectIntermediateDir() / TEXT("PinWright/Python");
        IFileManager::Get().MakeDirectory(*TempDir, /*Tree=*/true);
        return TempDir / FString::Printf(TEXT("RecorderQuery_%s.py"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // Wrap the user's return-style snippet in a function, load the session, run it,
    // and print the capped JSON result between sentinels. The snippet body is
    // indented one level so it nests inside `def __pw_query(q):`.
    FString ComposeWrapper(const FString& SessionAbsPath, const FString& PythonContentDir, const FString& UserQuery, int32 MaxRows)
    {
        // Indent every line of the user snippet by four spaces for the function body.
        TArray<FString> Lines;
        UserQuery.ParseIntoArrayLines(Lines, /*InCullEmpty=*/false);
        FString Indented;
        for (const FString& Line : Lines)
        {
            Indented += TEXT("    ") + Line + TEXT("\n");
        }

        // Python literals: forward-slash paths sidestep backslash-escaping entirely.
        const FString SessionLiteral = SessionAbsPath.Replace(TEXT("\\"), TEXT("/"));
        const FString ContentDirLiteral = PythonContentDir.Replace(TEXT("\\"), TEXT("/"));

        FString Script;
        Script += TEXT("import sys, json, traceback\n");
        Script += FString::Printf(TEXT("sys.path.insert(0, r\"%s\")\n"), *ContentDirLiteral);
        Script += TEXT("import recorder_query\n");
        Script += FString::Printf(TEXT("def __pw_query(q):\n%s"), *Indented);
        // Guard against an empty/no-return body.
        Script += TEXT("    return None\n");
        Script += TEXT("try:\n");
        Script += FString::Printf(TEXT("    q = recorder_query.load(r\"%s\")\n"), *SessionLiteral);
        Script += TEXT("    __result = recorder_query.cap(__pw_query(q), ");
        Script += FString::Printf(TEXT("%d)\n"), MaxRows);
        Script += FString::Printf(TEXT("    print(\"%s\" + json.dumps(__result) + \"%s\")\n"), kResultBegin, kResultEnd);
        Script += TEXT("except Exception:\n");
        Script += FString::Printf(TEXT("    print(\"%s\" + json.dumps({\"error\": \"runtime_error\", \"diagnostics\": traceback.format_exc().splitlines()}) + \"%s\")\n"),
            kResultBegin, kResultEnd);
        return Script;
    }

    // Pull the sentinel-delimited JSON blob out of the child process output.
    bool ExtractResultJson(const FString& Output, FString& OutJson)
    {
        int32 Begin = Output.Find(kResultBegin, ESearchCase::CaseSensitive);
        if (Begin == INDEX_NONE)
        {
            return false;
        }
        Begin += FCString::Strlen(kResultBegin);
        const int32 End = Output.Find(kResultEnd, ESearchCase::CaseSensitive, ESearchDir::FromStart, Begin);
        if (End == INDEX_NONE)
        {
            return false;
        }
        OutJson = Output.Mid(Begin, End - Begin).TrimStartAndEnd();
        return true;
    }

    class FRecorderQueryRun : public TSharedFromThis<FRecorderQueryRun>
    {
    public:
        FRecorderQueryRun(const FString& InInterpreterPath, const FString& InSessionAbsPath,
            const double InTimeoutSeconds, const FString& InTempScriptPath)
            : InterpreterPath(InInterpreterPath)
            , SessionAbsPath(InSessionAbsPath)
            , TimeoutSeconds(InTimeoutSeconds)
            , TempScriptPath(InTempScriptPath)
        {
        }

        void Start(FJobOnComplete InOnComplete)
        {
            OnComplete = MoveTemp(InOnComplete);
            StartedSeconds = FPlatformTime::Seconds();
            DeadlineSeconds = StartedSeconds + TimeoutSeconds;

            if (!FPlatformProcess::CreatePipe(ReadPipe, WritePipe, /*bWritePipeLocal=*/false))
            {
                FinishLaunchFailure(TEXT("CREATEPIPE_FAILED"));
                return;
            }

            const FString Arguments = FString::Printf(TEXT("-I -u \"%s\""), *TempScriptPath);
            ProcessId = 0;
            Process = FPlatformProcess::CreateProc(
                *InterpreterPath,
                *Arguments,
                /*bLaunchDetached=*/false,
                /*bLaunchHidden=*/true,
                /*bLaunchReallyHidden=*/true,
                &ProcessId,
                0,
                *FPaths::GetPath(InterpreterPath),
                WritePipe);

            // The child owns the inherited write end. Keeping the editor's copy open would
            // prevent EOF and make the output drain look like an active process forever.
            FPlatformProcess::ClosePipe(nullptr, WritePipe);
            WritePipe = nullptr;

            if (!Process.IsValid())
            {
                FinishLaunchFailure(TEXT("CREATEPROC_FAILED"));
                return;
            }

            TSharedRef<FRecorderQueryRun> Self = AsShared();
            FTSTicker::GetCoreTicker().AddTicker(
                FTickerDelegate::CreateLambda(
                    [Self](float)
                    {
                        return Self->Tick();
                    }),
                kQueryPollIntervalSeconds);
        }

        void Cancel()
        {
            BeginTermination(EQueryTerminationReason::Cancelled);
        }

    private:
        void DrainOutput()
        {
            if (ReadPipe == nullptr)
            {
                return;
            }

            for (;;)
            {
                const FString Chunk = FPlatformProcess::ReadPipe(ReadPipe);
                if (Chunk.IsEmpty())
                {
                    break;
                }

                if (Output.Len() >= kMaxQueryOutputCharacters)
                {
                    bOutputTruncated = true;
                    break;
                }

                const int32 Remaining = kMaxQueryOutputCharacters - Output.Len();
                Output += Chunk.Left(Remaining);
                if (Chunk.Len() >= Remaining)
                {
                    bOutputTruncated = true;
                    // Do not drain an unbounded producer forever on the game-thread ticker. The
                    // deadline/cancel path must retain a chance to run even for noisy scripts.
                    break;
                }
            }
        }

        void TerminateProcess()
        {
            if (!Process.IsValid() || ProcessId == 0 || !FPlatformProcess::IsProcRunning(Process))
            {
                return;
            }
            if (bTerminationAttempted)
            {
                return;
            }

            // Open the target handle before queueing the worker. The worker owns this handle for
            // the whole recursive tree walk, so it cannot race PID reuse or the editor-side
            // cleanup of the CreateProc handle.
            const TSharedRef<FProcHandle> TerminationHandle = MakeShared<FProcHandle>(
                FPlatformProcess::OpenProcess(ProcessId));
            const TSharedRef<std::atomic<bool>> TaskDone = MakeShared<std::atomic<bool>>(false);
            bTerminationAttempted = true;
            TerminationTaskDone = TaskDone;
            const TSharedRef<FRecorderQueryRun> Self = AsShared();
            AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
                [Self, TerminationHandle, TaskDone]()
                {
                    if (TerminationHandle->IsValid())
                    {
                        FPlatformProcess::TerminateProc(*TerminationHandle, /*KillTree=*/true);
                        FPlatformProcess::CloseProc(*TerminationHandle);
                    }
                    TaskDone->store(true, std::memory_order_release);
                    PinWrightSafePoint::DeferToSafePoint(
                        [Self]()
                        {
                            Self->OnTerminationTaskFinished();
                        },
                        TEXT("recorder.query termination cleanup"));
                });
        }

        bool TryGetProcessExitCode(int32& OutReturnCode)
        {
            if (FPlatformProcess::GetProcReturnCode(Process, &OutReturnCode))
            {
                return true;
            }
            if (!FPlatformProcess::IsProcRunning(Process))
            {
                OutReturnCode = -1;
                return true;
            }
            return false;
        }

        void BeginTermination(const EQueryTerminationReason Reason)
        {
            if (bCompleted || bTerminationRequested)
            {
                return;
            }

            bTerminationRequested = true;
            TerminationReason = Reason;
            const double NowSeconds = FPlatformTime::Seconds();
            TerminationHardDeadlineSeconds = NowSeconds + kQueryTerminationHardMaxSeconds;
            TerminateProcess();
        }

        void Cleanup()
        {
            if (bCleanupFinalized)
            {
                return;
            }
            if (TerminationTaskDone.IsValid() &&
                !TerminationTaskDone->load(std::memory_order_acquire))
            {
                bCleanupIncomplete = true;
                return;
            }
            if (Process.IsValid())
            {
                FPlatformProcess::CloseProc(Process);
            }
            if (ReadPipe != nullptr || WritePipe != nullptr)
            {
                FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
            }
            ReadPipe = nullptr;
            WritePipe = nullptr;
            if (!TempScriptPath.IsEmpty())
            {
                IFileManager::Get().Delete(*TempScriptPath, /*RequireExists=*/false, /*EvenReadOnly=*/true);
            }
            bCleanupFinalized = true;
        }

        void OnTerminationTaskFinished()
        {
            // Keep the polling handle alive until the ticker observes exit or the hard cutoff
            // classifies cleanup as incomplete. After a hard-cutoff terminal result, this
            // callback is the safe point for the deferred best-effort cleanup.
            if (bCompletionReported)
            {
                Cleanup();
            }
        }

        TSharedPtr<FJsonObject> MakeMeta(const double NowSeconds, const bool bTimedOut,
            const bool bTerminationConfirmed, const bool bIncludeCleanupIncomplete = false) const
        {
            TSharedPtr<FJsonObject> Meta = MakeShared<FJsonObject>();
            Meta->SetStringField(TEXT("timeDomain"), TEXT("time"));
            Meta->SetStringField(TEXT("session"), FPaths::GetCleanFilename(SessionAbsPath));
            Meta->SetNumberField(TEXT("schemaVersion"), 1);
            Meta->SetNumberField(TEXT("timeoutSeconds"), TimeoutSeconds);
            Meta->SetNumberField(TEXT("elapsedSeconds"), FMath::Max(0.0, NowSeconds - StartedSeconds));
            Meta->SetBoolField(TEXT("timedOut"), bTimedOut);
            if (bTimedOut)
            {
                Meta->SetBoolField(TEXT("terminationConfirmed"), bTerminationConfirmed);
                if (bIncludeCleanupIncomplete)
                {
                    Meta->SetBoolField(TEXT("cleanupIncomplete"), true);
                }
            }
            return Meta;
        }

        TSharedPtr<FJsonObject> MakeFailureResponse(const FString& Failure, const FString& Diagnostics,
            const double NowSeconds, const bool bTimedOut, const bool bTerminationConfirmed = true,
            const bool bIncludeCleanupIncomplete = false) const
        {
            TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
            Response->SetStringField(TEXT("error"), Failure);
            Response->SetBoolField(TEXT("timedOut"), bTimedOut);
            Response->SetNumberField(TEXT("timeoutSeconds"), TimeoutSeconds);
            if (bTimedOut)
            {
                Response->SetBoolField(TEXT("terminationConfirmed"), bTerminationConfirmed);
                if (bIncludeCleanupIncomplete)
                {
                    Response->SetBoolField(TEXT("cleanupIncomplete"), true);
                }
            }
            if (!Diagnostics.IsEmpty())
            {
                Response->SetStringField(TEXT("diagnostics"), Diagnostics);
            }
            Response->SetObjectField(TEXT("meta"), MakeMeta(
                NowSeconds, bTimedOut, bTerminationConfirmed, bIncludeCleanupIncomplete));
            return Response;
        }

        FString DiagnosticsTail() const
        {
            FString Diagnostics = Output.Right(8192);
            if (bOutputTruncated)
            {
                Diagnostics = FString(TEXT("[child output truncated]\n")) + Diagnostics;
            }
            return Diagnostics;
        }

        void FinishLaunchFailure(const FString& LaunchError)
        {
            if (bCompleted)
            {
                return;
            }
            bCompleted = true;
            DrainOutput();
            const double NowSeconds = FPlatformTime::Seconds();
            TSharedPtr<FJsonObject> Response = MakeFailureResponse(
                TEXT("execution_failed"), LaunchError + TEXT("\n") + DiagnosticsTail(), NowSeconds, false);
            Cleanup();
            FJobOnComplete Completion = MoveTemp(OnComplete);
            Completion(false, Response, ErrorCodes::ERR_QUERY_FAILED);
        }

        void ReportTermination(const double NowSeconds, const bool bTerminationConfirmed,
            const bool bTerminationCleanupIncomplete)
        {
            if (bCompletionReported || (!bCleanupFinalized && !bTerminationCleanupIncomplete))
            {
                return;
            }

            bCompletionReported = true;
            FJobOnComplete Completion = MoveTemp(OnComplete);
            if (TerminationReason == EQueryTerminationReason::Cancelled)
            {
                Completion(false, nullptr, FString());
                return;
            }

            TSharedPtr<FJsonObject> Response = MakeFailureResponse(
                TEXT("timeout"), DiagnosticsTail(), NowSeconds, true, bTerminationConfirmed,
                bTerminationCleanupIncomplete);
            Completion(false, Response, ErrorCodes::ERR_TIMEOUT);
        }

        void FinishProcess(const int32 ReturnCode)
        {
            if (bCompleted)
            {
                return;
            }
            bCompleted = true;
            DrainOutput();
            const double NowSeconds = FPlatformTime::Seconds();

            FString ResultJson;
            TSharedPtr<FJsonObject> Response;
            bool bSuccess = false;
            if (ReturnCode != 0 || !ExtractResultJson(Output, ResultJson))
            {
                FString Failure;
                if (ReturnCode == 0)
                {
                    Failure = TEXT("Python snippet did not produce a result.");
                }
                else
                {
                    Failure = FString::Printf(TEXT("Python child exited with code %d."), ReturnCode);
                }
                Response = MakeFailureResponse(TEXT("execution_failed"), Failure + TEXT("\n") + DiagnosticsTail(),
                    NowSeconds, false);
            }
            else
            {
                TSharedPtr<FJsonObject> Parsed;
                const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResultJson);
                if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid())
                {
                    Response = MakeFailureResponse(TEXT("bad_result_json"), ResultJson.Left(2000), NowSeconds, false);
                }
                else
                {
                    FString SnippetError;
                    if (Parsed->TryGetStringField(TEXT("error"), SnippetError))
                    {
                        Response = MakeShared<FJsonObject>();
                        Response->SetStringField(TEXT("error"), SnippetError);
                        Response->SetBoolField(TEXT("timedOut"), false);
                        Response->SetNumberField(TEXT("timeoutSeconds"), TimeoutSeconds);
                        Response->SetObjectField(TEXT("meta"), MakeMeta(NowSeconds, false, true));
                        const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
                        if (Parsed->TryGetArrayField(TEXT("diagnostics"), Diagnostics) && Diagnostics)
                        {
                            Response->SetArrayField(TEXT("diagnostics"), *Diagnostics);
                        }
                        else
                        {
                            Response->SetStringField(TEXT("diagnostics"), ResultJson.Left(8192));
                        }
                    }
                    else
                    {
                        Response = MakeShared<FJsonObject>();
                        if (TSharedPtr<FJsonValue> ValueField = Parsed->TryGetField(TEXT("value")))
                        {
                            Response->SetField(TEXT("value"), ValueField);
                        }

                        TSharedPtr<FJsonObject> Meta = MakeMeta(NowSeconds, false, true);
                        const TSharedPtr<FJsonObject>* CapMeta = nullptr;
                        if (Parsed->TryGetObjectField(TEXT("meta"), CapMeta) && CapMeta && CapMeta->IsValid())
                        {
                            bool bTruncated = false;
                            (*CapMeta)->TryGetBoolField(TEXT("truncated"), bTruncated);
                            Meta->SetBoolField(TEXT("truncated"), bTruncated);

                            double TotalCount = 0;
                            if ((*CapMeta)->TryGetNumberField(TEXT("totalCount"), TotalCount))
                            {
                                Meta->SetNumberField(TEXT("totalCount"), TotalCount);
                            }
                            double Elided = 0;
                            (*CapMeta)->TryGetNumberField(TEXT("elided"), Elided);
                            Meta->SetNumberField(TEXT("elided"), Elided);
                        }
                        Response->SetObjectField(TEXT("meta"), Meta);
                        Response->SetBoolField(TEXT("timedOut"), false);
                        Response->SetNumberField(TEXT("timeoutSeconds"), TimeoutSeconds);
                        bSuccess = true;
                    }
                }
            }

            Cleanup();
            FJobOnComplete Completion = MoveTemp(OnComplete);
            const FString Error = bSuccess ? FString() : FString(ErrorCodes::ERR_QUERY_FAILED);
            Completion(bSuccess, Response, Error);
        }

        bool Tick()
        {
            if (bCompleted)
            {
                return false;
            }

            if (bTerminationRequested)
            {
                DrainOutput();
                const double TerminationNowSeconds = FPlatformTime::Seconds();
                if (TerminationNowSeconds >= TerminationHardDeadlineSeconds)
                {
                    const bool bTerminationTaskComplete = !TerminationTaskDone.IsValid() ||
                        TerminationTaskDone->load(std::memory_order_acquire);
                    bCleanupIncomplete = !bTerminationTaskComplete || !bProcessExitObserved;
                    Cleanup();
                    bCompleted = true;
                    ReportTermination(TerminationNowSeconds, bProcessExitObserved && bTerminationTaskComplete,
                        bCleanupIncomplete);
                    return false;
                }

                int32 ReturnCode = 0;
                if (TryGetProcessExitCode(ReturnCode))
                {
                    bProcessExitObserved = true;
                }

                // An already-exited child does not need a termination worker. Treating the
                // absent task as complete avoids waiting until the hard cutoff in that race.
                const bool bTerminationTaskComplete = !TerminationTaskDone.IsValid() ||
                    TerminationTaskDone->load(std::memory_order_acquire);
                if (bProcessExitObserved && bTerminationTaskComplete)
                {
                    const double ExitSeconds = FPlatformTime::Seconds();
                    Cleanup();
                    bCompleted = true;
                    ReportTermination(ExitSeconds, true, false);
                    return false;
                }

                TerminateProcess();
                return true;
            }

            const double BeforeWorkSeconds = FPlatformTime::Seconds();
            if (BeforeWorkSeconds >= DeadlineSeconds)
            {
                BeginTermination(EQueryTerminationReason::TimedOut);
                return true;
            }

            DrainOutput();
            // A slow drain can cross the deadline. Classify conservatively before inspecting
            // the child exit code so a late observation cannot become timedOut=false.
            if (FPlatformTime::Seconds() >= DeadlineSeconds)
            {
                BeginTermination(EQueryTerminationReason::TimedOut);
                return true;
            }

            int32 ReturnCode = 0;
            if (TryGetProcessExitCode(ReturnCode))
            {
                FinishProcess(ReturnCode);
                return false;
            }
            return true;
        }

        FString InterpreterPath;
        FString SessionAbsPath;
        double TimeoutSeconds = kDefaultQueryTimeoutSeconds;
        FString TempScriptPath;
        FProcHandle Process;
        uint32 ProcessId = 0;
        void* ReadPipe = nullptr;
        void* WritePipe = nullptr;
        FString Output;
        FJobOnComplete OnComplete;
        double StartedSeconds = 0.0;
        double DeadlineSeconds = 0.0;
        double TerminationHardDeadlineSeconds = 0.0;
        bool bOutputTruncated = false;
        EQueryTerminationReason TerminationReason = EQueryTerminationReason::None;
        bool bTerminationRequested = false;
        bool bCompletionReported = false;
        bool bCleanupFinalized = false;
        bool bCleanupIncomplete = false;
        bool bProcessExitObserved = false;
        bool bTerminationAttempted = false;
        TSharedPtr<std::atomic<bool>> TerminationTaskDone;
        bool bCompleted = false;
    };
}

// ---- recorder.query ----
REGISTER_RPC_HANDLER("recorder.query", "recorder",
    "Run a small Python snippet over a recorded journal session — the escape hatch for "
    "ad-hoc questions the structured verbs (get_state, summarize_change, get_series, find_events) "
    "can't express. CALL recorder.describe_session FIRST to learn the object keys and tags. "
    "Your 'query' is a Python function body that must `return` a value, with `q` (the read-only "
    "session) in scope. The q surface: q.objects (key->record; a record is a plain dict — index "
    "with rec['field'], attribute access raises AttributeError), q.variables (tag->manifest), "
    "q.events (list), q.series_keys, q.series(key,tag) -> list[Point], q.value_at(key,tag,ts) -> "
    "Point|None (as-of), q.magnitude(point) -> float, q.min_ts, q.max_ts, q.session_id. "
    "A Point has .t (wall-clock seconds), .kind, .v (scalar or [x,y,z..]), .s (string/enum), "
    ".dt/.df (domain time/frame). Return a primitive, a Point, or a list of dicts/Points — "
    "the result is capped at 'maxRows'. The body runs in an isolated bundled-Python child "
    "process, so the editor stays responsive even when a caller loop misbehaves. The job is "
    "tracked and killed when timeoutSeconds expires; a hard cleanup cutoff may also report "
    "cleanupIncomplete=true. Use stdlib only (json, bisect, math), no "
    "pandas/numpy. Exact session paths/filenames/ids win; substring matching must be unique.",
    RPC_PARAMS(
        RPC_PARAM_REQ("session", "string", "Recording file path, filename, or session id under Saved/PinWright/Recordings."),
        RPC_PARAM_REQ("query", "string", "A Python function body that returns a value, with `q` (the read-only session) in scope."),
        RPC_PARAM_DEF("maxRows", "number", "Max rows for an enumerable result (hard cap 2000). Default 200.", "200"),
        RPC_PARAM_DEF("timeoutSeconds", "number", "Wall-clock child-process budget in seconds. Clamped to 0.1-60.0; default 30.0. A killed child reports timedOut=true; cleanupIncomplete=true means the hard cleanup cutoff was reached before termination cleanup finished.", "30")
    ))
{
    FString Session;
    if (!Ctx.RequireString(TEXT("session"), Session))
    {
        return true;
    }

    FString Query;
    if (!Ctx.RequireString(TEXT("query"), Query))
    {
        return true;
    }

    int32 MaxRows = Ctx.GetInt(TEXT("maxRows"), 200);
    if (MaxRows <= 0)
    {
        MaxRows = 200;
    }
    MaxRows = FMath::Min(MaxRows, 2000);

    double RequestedTimeoutSeconds = kDefaultQueryTimeoutSeconds;
    if (Ctx.GetRawPayload().IsValid() && Ctx.GetRawPayload()->HasField(TEXT("timeoutSeconds")) &&
        !Ctx.RequireNumber(TEXT("timeoutSeconds"), RequestedTimeoutSeconds))
    {
        return true;
    }
    const double TimeoutSeconds = FMath::Clamp(
        RequestedTimeoutSeconds,
        kMinQueryTimeoutSeconds,
        kMaxQueryTimeoutSeconds);

    const FSessionResolution Resolution = ResolveSessionPath(Session);
    if (Resolution.Status == ESessionResolution::Ambiguous)
    {
        TSharedPtr<FJsonObject> Ambiguous = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Candidates;
        for (const FString& Candidate : Resolution.Candidates)
        {
            Candidates.Add(MakeShared<FJsonValueString>(Candidate));
        }
        Ambiguous->SetArrayField(TEXT("candidates"), Candidates);
        Ctx.SendError(ErrorCodes::ERR_AMBIGUOUS_SESSION,
            FString::Printf(TEXT("Session '%s' matched multiple recordings; pass an exact id, filename, or path."), *Session),
            Ambiguous);
        return true;
    }
    if (Resolution.Status != ESessionResolution::Resolved)
    {
        Ctx.SendError(ErrorCodes::ERR_SESSION_NOT_FOUND,
            FString::Printf(TEXT("Could not resolve session '%s' to a file under Saved/PinWright/Recordings."), *Session));
        return true;
    }
    const FString SessionAbsPath = Resolution.Path;

    IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
    if (!Python)
    {
        Ctx.SendError(ErrorCodes::ERR_PYTHON_NOT_AVAILABLE,
            TEXT("PythonScriptPlugin is not loaded. Enable it in Edit > Plugins."));
        return true;
    }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    // IsPythonInitialized / ForceEnablePythonAtRuntime were added in UE 5.6.
    if (!Python->IsPythonInitialized())
    {
        Python->ForceEnablePythonAtRuntime();
        if (!Python->IsPythonInitialized())
        {
            Ctx.SendError(ErrorCodes::ERR_PYTHON_INIT_FAILED,
                TEXT("Python could not be initialized. Check Output Log for details."));
            return true;
        }
    }
#else
    // UE 5.4/5.5: no runtime-enable hook; the plugin is initialized eagerly when available.
    if (!Python->IsPythonAvailable())
    {
        Ctx.SendError(ErrorCodes::ERR_PYTHON_INIT_FAILED,
            TEXT("Python is not available. Check Output Log for details."));
        return true;
    }
#endif

    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
    if (!Plugin.IsValid())
    {
        Ctx.SendError(ErrorCodes::ERR_PLUGIN_NOT_FOUND, TEXT("PinWright plugin not found."));
        return true;
    }
    const FString PythonContentDir = Plugin->GetContentDir() / TEXT("Python");
    const FString InterpreterPath = FPaths::ConvertRelativePathToFull(Python->GetInterpreterExecutablePath());
    if (!FPaths::FileExists(InterpreterPath))
    {
        Ctx.SendError(ErrorCodes::ERR_PYTHON_NOT_AVAILABLE,
            TEXT("The PythonScriptPlugin interpreter executable could not be resolved."));
        return true;
    }

    const FString Script = ComposeWrapper(SessionAbsPath, PythonContentDir, Query, MaxRows);

    // Run via a temporary file in an isolated child process. This is the killable boundary:
    // arbitrary caller Python is never executed by the editor's embedded interpreter.
    const FString TempScriptPath = MakeTempScriptPath();
    if (!FFileHelper::SaveStringToFile(Script, *TempScriptPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        Ctx.SendError(ErrorCodes::ERR_TEMP_FILE_WRITE_FAILED,
            FString::Printf(TEXT("Could not write temporary Python script '%s'"), *TempScriptPath));
        return true;
    }

    TSharedRef<FRecorderQueryRun> State = MakeShared<FRecorderQueryRun>(
        InterpreterPath,
        SessionAbsPath,
        TimeoutSeconds,
        TempScriptPath);

    FJobBindArgs Args;
    Args.Method = Ctx.GetMethod();
    Args.StartedPayload = MakeShared<FJsonObject>();
    Args.StartedPayload->SetStringField(TEXT("session"), FPaths::GetCleanFilename(SessionAbsPath));
    Args.StartedPayload->SetNumberField(TEXT("maxRows"), MaxRows);
    Args.StartedPayload->SetNumberField(TEXT("timeoutSeconds"), TimeoutSeconds);
    Args.StartedPayload->SetStringField(TEXT("message"), TEXT("recorder.query is running in an isolated Python child process"));
    Args.BindNativeDelegate = [State](FJobOnComplete OnComplete)
    {
        State->Start(MoveTemp(OnComplete));
    };

    const FString TicketId = Ctx.StartJob(Args);
    TWeakPtr<FRecorderQueryRun> WeakState(State);
    FPluginState::Get().GetJobRegistry().SetCancelCallback(
        TicketId,
        [WeakState]()
        {
            if (const TSharedPtr<FRecorderQueryRun> Pinned = WeakState.Pin())
            {
                Pinned->Cancel();
            }
        });
    return true;
}
