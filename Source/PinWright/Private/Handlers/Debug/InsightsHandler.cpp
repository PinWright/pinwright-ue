// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Debug/InsightsHandlerInternal.h"
#include "PinWrightHelpers.h"
#include "Compat/EngineVersionCompat.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/TraceAuxiliary.h"

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
}

REGISTER_RPC_HANDLER("insights.start_session", "insights", "Start an Unreal Insights trace by invoking the 'Trace.Start' console command. Captured trace data can later be opened in the Unreal Insights tool to inspect frame timings, CPU/GPU usage, etc.",
    RPC_PARAMS(
        RPC_PARAM_OPT("channels", "string", "Comma-separated channel list passed to Trace.Start (e.g. 'cpu,gpu,frame', 'audio'). Omit to use the default channel set.")
    ))
{
    FString Channels = Ctx.GetString(TEXT("channels"));
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

    if (!Channels.IsEmpty())
    {
        GEngine->Exec(nullptr, *FString::Printf(TEXT("Trace.Start %s"), *Channels));
        Result->SetStringField(TEXT("channels"), Channels);
    }
    else
    {
        GEngine->Exec(nullptr, TEXT("Trace.Start"));
    }

    Result->SetStringField(TEXT("action"), TEXT("start_trace"));
    Result->SetStringField(TEXT("status"), TEXT("started"));

    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("insights.stop_session", "insights", "Stop the active Unreal Insights trace via FTraceAuxiliary::Stop and report the resolved trace path. The path is captured BEFORE Stop runs so callers can still retrieve the .utrace location after the connection is torn down.",
    RPC_NO_PARAMS)
{
    // Capture the destination before Stop() clears it.
    const FString Path = PinWrightRpc::Insights::ResolveActiveTracePath();
    const bool bWasRunning = FTraceAuxiliary::Stop();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("status"), bWasRunning ? TEXT("stopped") : TEXT("not_running"));
    Result->SetStringField(TEXT("tracePath"), Path);

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

REGISTER_RPC_HANDLER("insights.set_channels", "insights", "Mutate the active Unreal Insights channel set mid-trace by forwarding to the 'Trace.Enable' / 'Trace.Disable' console commands.",
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

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("enabled"), EnableList);
    Result->SetStringField(TEXT("disabled"), DisableList);

    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("insights.snapshot", "insights", "Flush the in-memory trace ring buffer to a .utrace file via FTraceAuxiliary::WriteSnapshot without stopping the active session. Returns the resolved filePath the engine actually wrote, including the auto-generated name when filePath is omitted.",
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
    Result->SetStringField(TEXT("status"), bOk ? TEXT("snapshot_written") : TEXT("snapshot_failed"));
    Result->SetStringField(TEXT("filePath"), ResolvedFilePath);
    if (bOk && !bResolvedFromEngine)
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
