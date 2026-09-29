// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Debug/InsightsHandlerInternal.h"
#include "Handlers/ErrorCodes.h"
#include "PinWrightHelpers.h"
#include "Compat/EngineVersionCompat.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Misc/StringBuilder.h"
#include "ProfilingDebugging/TraceAuxiliary.h"
#include "Trace/Trace.h"

namespace PinWrightRpc::Insights
{
    FString ResolveActiveTracePath()
    {
        // FTraceAuxiliary returns an empty string when no trace is active.
        return FTraceAuxiliary::GetTraceDestinationString();
    }

    bool WriteSnapshotResolvingPath(const FString& RequestedFilePath, FString& OutResolvedPath, bool& bOutResolvedFromEngine)
    {
        // WriteSnapshot returns only a bool, but the engine broadcasts
        // OnSnapshotSaved synchronously inside the call with the fully-resolved
        // native path (the auto-generated name when the input is empty). Bind a
        // temporary lambda to capture it, then unbind — deterministic and
        // race-free, unlike mtime-sorting the profiling directory.
        FString CapturedPath;
        const FDelegateHandle Handle = FTraceAuxiliary::OnSnapshotSaved.AddLambda(
            [&CapturedPath](FTraceAuxiliary::EConnectionType /*TraceType*/, const FString& TraceDestination)
            {
                CapturedPath = TraceDestination;
            });

        const bool bOk = FTraceAuxiliary::WriteSnapshot(
            RequestedFilePath.IsEmpty() ? nullptr : *RequestedFilePath);

        FTraceAuxiliary::OnSnapshotSaved.Remove(Handle);

        // The delegate fired iff CapturedPath is non-empty; that is the only case
        // where the engine itself resolved the destination (vs. us echoing the
        // requested path). Absolutize the result to match the path shape the rest
        // of the insights/performance handlers return (FPaths::ConvertRelativePathToFull).
        bOutResolvedFromEngine = !CapturedPath.IsEmpty();
        const FString& ChosenPath = bOutResolvedFromEngine ? CapturedPath : RequestedFilePath;
        OutResolvedPath = ChosenPath.IsEmpty() ? ChosenPath : FPaths::ConvertRelativePathToFull(ChosenPath);
        return bOk;
    }

    bool RequestTraceStop(double TimeoutSeconds)
    {
        const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
        for (;;)
        {
            if (!FTraceAuxiliary::IsConnected())
            {
                return false;
            }
            if (FTraceAuxiliary::Stop())
            {
                return true;
            }
            // Refused with the connection type already None: an earlier stop's close is pending.
            if (FTraceAuxiliary::GetConnectionType() == FTraceAuxiliary::EConnectionType::None)
            {
                return true;
            }
            // Refused while the start is still pending; retry once the worker adopts the connection.
            if (FPlatformTime::Seconds() >= Deadline)
            {
                return false;
            }
            UE::Trace::Update();
            FPlatformProcess::Sleep(0.005f);
        }
    }

    bool WaitForTraceClose(double TimeoutSeconds)
    {
        const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
        while (FTraceAuxiliary::IsConnected() && FPlatformTime::Seconds() < Deadline)
        {
            // No-op while TraceLog's worker thread runs (the editor default); drives the writer
            // when it does not (-notracethreading), where the close otherwise waits for end of frame.
            UE::Trace::Update();
            FPlatformProcess::Sleep(0.005f);
        }
        return !FTraceAuxiliary::IsConnected();
    }
}

namespace
{
    // Maps FTraceAuxiliary::EConnectionType to a stable string for RPC results.
    const TCHAR* ConnectionTypeToString(FTraceAuxiliary::EConnectionType Type)
    {
        switch (Type)
        {
        case FTraceAuxiliary::EConnectionType::Network: return TEXT("Network");
        case FTraceAuxiliary::EConnectionType::File:    return TEXT("File");
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        // EConnectionType::Relay was added in UE 5.6
        case FTraceAuxiliary::EConnectionType::Relay:   return TEXT("Relay");
#endif
        case FTraceAuxiliary::EConnectionType::None:    return TEXT("None");
        default:                                        return TEXT("Unknown");
        }
    }

    FString GetActiveChannels()
    {
        TStringBuilder<512> Channels;
        FTraceAuxiliary::GetActiveChannelsString(Channels);
        return FString(Channels.ToString());
    }

    // Bounded wait for a stop's queued close; the handler runs synchronously on the game thread.
    constexpr double TraceCloseTimeoutSeconds = 5.0;
}

REGISTER_RPC_HANDLER("insights.start_session", "insights", "Start an Unreal Insights file trace (FTraceAuxiliary::Start, auto-named .utrace in the profiling directory) and report the destination the engine actually connected to. Refuses with TRACE_ALREADY_ACTIVE, naming the active destination, when a trace is already open; a previous stop that is still closing is waited out (up to 5 s) first. TRACE_START_FAILED when the engine opened no connection.",
    RPC_PARAMS(
        RPC_PARAM_OPT("channels", "string", "Comma-separated channel list (e.g. 'cpu,gpu,frame', 'audio'). Omit to keep the current channel set.")
    ))
{
    const FString Channels = Ctx.GetString(TEXT("channels"));

    // A stop still closing reads connected with connection type None; wait it out so Start does not
    // hit the engine's "already tracing" refusal.
    if (FTraceAuxiliary::IsConnected()
        && FTraceAuxiliary::GetConnectionType() == FTraceAuxiliary::EConnectionType::None)
    {
        PinWrightRpc::Insights::WaitForTraceClose(TraceCloseTimeoutSeconds);
    }
    if (FTraceAuxiliary::IsConnected())
    {
        const FString ActiveDestination = PinWrightRpc::Insights::ResolveActiveTracePath();
        TSharedPtr<FJsonObject> Refusal = MakeShared<FJsonObject>();
        Refusal->SetStringField(TEXT("activeDestination"), ActiveDestination);
        Refusal->SetStringField(TEXT("connectionType"), ConnectionTypeToString(FTraceAuxiliary::GetConnectionType()));
        Refusal->SetBoolField(TEXT("connected"), true);
        Ctx.SendError(ErrorCodes::ERR_TRACE_ALREADY_ACTIVE,
            FString::Printf(TEXT("A trace is already active (destination '%s'); stop it with insights.stop_session first."),
                *ActiveDestination),
            Refusal);
        return true;
    }

    const bool bStarted = FTraceAuxiliary::Start(FTraceAuxiliary::EConnectionType::File, nullptr,
        Channels.IsEmpty() ? nullptr : *Channels);
    const FString TracePath = PinWrightRpc::Insights::ResolveActiveTracePath();
    const bool bConnected = FTraceAuxiliary::IsConnected();
    if (!bStarted || !bConnected || TracePath.IsEmpty())
    {
        TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
        Failure->SetBoolField(TEXT("started"), bStarted);
        Failure->SetBoolField(TEXT("connected"), bConnected);
        Failure->SetStringField(TEXT("tracePath"), TracePath);
        Ctx.SendError(ErrorCodes::ERR_TRACE_START_FAILED,
            TEXT("FTraceAuxiliary::Start opened no file trace; the editor log carries the engine's reason."),
            Failure);
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("status"), TEXT("started"));
    Result->SetStringField(TEXT("tracePath"), TracePath);
    Result->SetBoolField(TEXT("connected"), true);
    Result->SetStringField(TEXT("connectionType"), ConnectionTypeToString(FTraceAuxiliary::GetConnectionType()));
    Result->SetStringField(TEXT("activeChannels"), GetActiveChannels());
    if (!Channels.IsEmpty())
    {
        Result->SetStringField(TEXT("channels"), Channels);
    }

    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("insights.stop_session", "insights", "Stop the active Unreal Insights trace via FTraceAuxiliary::Stop (retried while a just-started connection is still pending, which the engine refuses to stop), wait (up to 5 s) for the engine to close the connection, and report the trace path captured before the stop plus whether the file is closed. status: stopped (closed), stop_pending (stop accepted, still closing), not_running (nothing to stop).",
    RPC_NO_PARAMS)
{
    // Capture the destination before Stop() clears it.
    const FString Path = PinWrightRpc::Insights::ResolveActiveTracePath();
    const bool bStopAccepted = PinWrightRpc::Insights::RequestTraceStop(TraceCloseTimeoutSeconds);
    const bool bClosed = PinWrightRpc::Insights::WaitForTraceClose(TraceCloseTimeoutSeconds);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("status"),
        !bStopAccepted ? TEXT("not_running") : (bClosed ? TEXT("stopped") : TEXT("stop_pending")));
    Result->SetStringField(TEXT("tracePath"), Path);
    Result->SetBoolField(TEXT("closed"), bClosed);
    if (bStopAccepted && !bClosed)
    {
        TArray<TSharedPtr<FJsonValue>> Warnings;
        Warnings.Add(MakeShared<FJsonValueString>(
            TEXT("The trace connection was still open after 5 s, so the .utrace may still be being written; poll insights.get_trace_path until connected is false before reading it.")));
        Result->SetArrayField(TEXT("warnings"), Warnings);
    }

    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("insights.get_trace_path", "insights", "Read-only probe that returns the current Unreal Insights trace destination and connection state without altering the trace session.",
    RPC_NO_PARAMS)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("tracePath"), PinWrightRpc::Insights::ResolveActiveTracePath());
    Result->SetBoolField(TEXT("connected"), FTraceAuxiliary::IsConnected());
    Result->SetStringField(TEXT("connectionType"), ConnectionTypeToString(FTraceAuxiliary::GetConnectionType()));

    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("insights.set_channels", "insights", "Mutate the active Unreal Insights channel set mid-trace by forwarding to the 'Trace.Enable' / 'Trace.Disable' console commands. enabled/disabled echo the request; activeChannels is the engine's measured channel set afterwards (an unknown channel name is absent from it).",
    RPC_PARAMS(
        RPC_PARAM_OPT("enable", "string", "Comma-separated channel names to enable"),
        RPC_PARAM_OPT("disable", "string", "Comma-separated channel names to disable")
    ))
{
    const FString EnableList = Ctx.GetString(TEXT("enable"));
    const FString DisableList = Ctx.GetString(TEXT("disable"));

    if (!EnableList.IsEmpty())
    {
        GEngine->Exec(nullptr, *FString::Printf(TEXT("Trace.Enable %s"), *EnableList));
    }
    if (!DisableList.IsEmpty())
    {
        GEngine->Exec(nullptr, *FString::Printf(TEXT("Trace.Disable %s"), *DisableList));
    }

    // enabled/disabled echo the request; activeChannels is the engine's set after the change.
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("enabled"), EnableList);
    Result->SetStringField(TEXT("disabled"), DisableList);
    Result->SetStringField(TEXT("activeChannels"), GetActiveChannels());

    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("insights.snapshot", "insights", "Flush the in-memory trace ring buffer to a .utrace file via FTraceAuxiliary::WriteSnapshot without stopping the active session. Returns the resolved filePath the engine actually wrote, including the auto-generated name when filePath is omitted. SNAPSHOT_FAILED when the engine wrote no snapshot.",
    RPC_PARAMS(
        RPC_PARAM_OPT("filePath", "filepath", "Output path; if omitted, the engine auto-generates one. The resolved path is reported back in the response's filePath field either way.")
    ))
{
    const FString RequestedFilePath = Ctx.GetString(TEXT("filePath"));

    // Resolve the actual written path via the OnSnapshotSaved delegate so an
    // omitted (auto-generated) path is reported back instead of an empty string.
    FString ResolvedFilePath;
    bool bResolvedFromEngine = false;
    const bool bOk = PinWrightRpc::Insights::WriteSnapshotResolvingPath(RequestedFilePath, ResolvedFilePath, bResolvedFromEngine);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("filePath"), ResolvedFilePath);
    if (!bOk)
    {
        Ctx.SendError(ErrorCodes::ERR_SNAPSHOT_FAILED,
            TEXT("FTraceAuxiliary::WriteSnapshot wrote no snapshot; the editor log carries the engine's reason."),
            Result);
        return true;
    }
    Result->SetStringField(TEXT("status"), TEXT("snapshot_written"));
    if (!bResolvedFromEngine)
    {
        // The snapshot wrote, but the OnSnapshotSaved delegate did not fire, so
        // filePath is the echoed (caller-supplied) path rather than an engine-
        // resolved destination. Flag that the precise path is unverified — same
        // shape as performance.generate_memory_report's pathResolved:false branch.
        Result->SetBoolField(TEXT("pathResolved"), false);
    }

    Ctx.SendSuccess(Result);
    return true;
}
