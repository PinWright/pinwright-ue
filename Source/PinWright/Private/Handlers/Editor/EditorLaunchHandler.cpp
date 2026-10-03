// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Editor/EditorLaunchHandlerInternal.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformOutputDevices.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

static FString StripObjectSuffix(const FString& InPath)
{
    int32 DotIdx = INDEX_NONE;
    if (InPath.FindLastChar(TEXT('.'), DotIdx))
    {
        return InPath.Left(DotIdx);
    }
    return InPath;
}

FString BuildStandaloneCommandLine(
    const FString& ProjectPath, const FString& Map, int32 InstanceIndex,
    int32 NumClients, bool bListenServer, const FString& ExtraArgs)
{
    const bool bIsClientJoiningListen = bListenServer && InstanceIndex > 0;

    FString MapToken;
    if (bIsClientJoiningListen)
    {
        MapToken = TEXT("127.0.0.1");
    }
    else if (bListenServer && InstanceIndex == 0)
    {
        MapToken = Map + TEXT("?listen");
    }
    else
    {
        MapToken = Map;
    }

    FString CommandLine = FString::Printf(TEXT("\"%s\" %s -game"), *ProjectPath, *MapToken);
    if (!ExtraArgs.IsEmpty())
    {
        CommandLine += TEXT(" ");
        CommandLine += ExtraArgs;
    }
    return CommandLine.TrimStartAndEnd();
}

FString AppendStandaloneLogArg(FString& CommandLine, const FString& ExtraArgs, const FString& DefaultLogFile)
{
    // Mirrors FGenericPlatformOutputDevices::GetAbsoluteLogFilename: LOG= / LogFileName= (relative
    // to the project log dir) win over ABSLOG=, and a name without a .log/.txt extension is
    // replaced by <Project>.log (for ABSLOG=, relative to the process's working dir, BaseDir).
    FString CallerLog;
    const bool bRelative = FParse::Value(*ExtraArgs, TEXT("LOG="), CallerLog, false)
        || FParse::Value(*ExtraArgs, TEXT("LogFileName="), CallerLog, false);
    if (!bRelative && !FParse::Value(*ExtraArgs, TEXT("ABSLOG="), CallerLog, false))
    {
        CommandLine += FString::Printf(TEXT(" -abslog=\"%s\""), *DefaultLogFile);
        return DefaultLogFile;
    }
    const FString Extension = FPaths::GetExtension(CallerLog);
    if (Extension != TEXT("log") && Extension != TEXT("txt"))
    {
        return FPaths::ConvertRelativePathToFull((bRelative ? FPaths::ProjectLogDir() : FString(FPlatformProcess::BaseDir()))
            / FString(FApp::GetProjectName()) + TEXT(".log"));
    }
    return FPaths::ConvertRelativePathToFull(bRelative ? FPaths::ProjectLogDir() / CallerLog : CallerLog);
}

FString StandaloneCallerLogRefusal(const FString& ExtraArgs, int32 NumClients)
{
    FString Probe;
    const FString CallerLog = AppendStandaloneLogArg(Probe, ExtraArgs, FString());
    if (!Probe.IsEmpty())
    {
        return FString(); // No caller log argument: every slot gets its own -abslog.
    }
    const bool bEditorLog = FPaths::IsSamePath(CallerLog,
        FPaths::ConvertRelativePathToFull(FPlatformOutputDevices::GetAbsoluteLogFilename()));
    if (!bEditorLog && NumClients <= 1)
    {
        return FString();
    }
    return FString::Printf(TEXT("The log argument in extraArgs resolves to %s, %s. The engine opens its log "
        "exclusively and a process that finds it held writes <name>_2.log, _3.log... instead, so the reported "
        "logPath would be another process's log. Omit the log argument for one log per slot."),
        *CallerLog, bEditorLog ? TEXT("the editor's own log") : TEXT("shared by every one of the numClients processes"));
}

REGISTER_RPC_HANDLER("editor.launch_standalone", "editor",
    "Launch one or more separate -game processes against the current project. "
    "Use for multiplayer test loops (set listenServer=true, numClients>1) or "
    "clean-baseline perf probes where editor tick overhead would distort the measurement. "
    "Each spawned process is detached; the editor keeps its handle for editor.standalone_status. "
    "Returns the resolved command line, OS PID and logPath for every slot; per-slot failures "
    "are reported with error=CREATEPROC_FAILED and pid=0 instead of aborting the batch. "
    "Each process logs to its own file (-abslog; a -log=/-LogFileName=/-abslog= in extraArgs is honoured and "
    "its resolved path reported, but refused INVALID_ARGUMENT with numClients>1 or when it resolves to the editor's own log); "
    "observe it with editor.standalone_status (exit code, log tail, Linux/X11 window capture).",
    RPC_PARAMS(
        RPC_PARAM_OPT("map", "path", "Package path to a level (e.g. /Game/Maps/M_Test). Defaults to the current editor world."),
        RPC_PARAM_OPT("extraArgs", "string", "Extra command-line arguments forwarded verbatim (e.g. \"-windowed -resx=1280\")."),
        RPC_PARAM_DEF("numClients", "number", "Number of processes to spawn. Clamped to [1,8].", "1"),
        RPC_PARAM_DEF("listenServer", "boolean", "When true, instance 0 launches with ?listen and instances 1..N-1 connect to 127.0.0.1.", "false")
    ))
{
    if (!GEditor)
    {
        Ctx.SendError(TEXT("EDITOR_NOT_AVAILABLE"), TEXT("Editor not available"));
        return true;
    }

    FString Map = Ctx.GetString(TEXT("map"), TEXT(""));
    if (Map.IsEmpty())
    {
        UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
        if (EditorWorld)
        {
            Map = StripObjectSuffix(EditorWorld->GetPathName());
        }
        if (Map.IsEmpty())
        {
            Ctx.SendError(TEXT("NO_EDITOR_WORLD"), TEXT("No map provided and no editor world is loaded"));
            return true;
        }
    }

    const FString ExtraArgs = Ctx.GetString(TEXT("extraArgs"), TEXT(""));
    int32 NumClients = Ctx.GetInt(TEXT("numClients"), 1);
    const bool bListenServer = Ctx.GetBool(TEXT("listenServer"), false);

    if (NumClients < 1 || NumClients > 8)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("numClients must be in [1,8], got %d"), NumClients));
        return true;
    }
    const FString LogRefusal = StandaloneCallerLogRefusal(ExtraArgs, NumClients);
    if (!LogRefusal.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), LogRefusal);
        return true;
    }

    const FString ProjectPath = FPaths::GetProjectFilePath();
    const TCHAR* ExecutablePath = FPlatformProcess::ExecutablePath();

    TArray<TSharedPtr<FJsonValue>> Processes;
    Processes.Reserve(NumClients);
    const FString LogDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("PinWright/standalone"));
    IFileManager::Get().MakeDirectory(*LogDir, /*Tree*/ true);
    const FString LogStem = LogDir / FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S-%s"));

    for (int32 i = 0; i < NumClients; ++i)
    {
        FString CmdLine = BuildStandaloneCommandLine(
            ProjectPath, Map, i, NumClients, bListenServer, ExtraArgs);
        const FString LogFile = AppendStandaloneLogArg(
            CmdLine, ExtraArgs, FString::Printf(TEXT("%s-%d.log"), *LogStem, i));

        uint32 PID = 0;
        FProcHandle Handle = FPlatformProcess::CreateProc(
            ExecutablePath, *CmdLine,
            /*bLaunchDetached*/ true,
            /*bLaunchHidden*/ false,
            /*bLaunchReallyHidden*/ false,
            &PID, 0, nullptr, nullptr);

        TSharedPtr<FJsonObject> ProcEntry = MakeShared<FJsonObject>();
        ProcEntry->SetStringField(TEXT("commandLine"), CmdLine);
        if (!Handle.IsValid())
        {
            ProcEntry->SetNumberField(TEXT("pid"), 0);
            ProcEntry->SetStringField(TEXT("error"), TEXT("CREATEPROC_FAILED"));
        }
        else
        {
            ProcEntry->SetNumberField(TEXT("pid"), static_cast<double>(PID));
            ProcEntry->SetStringField(TEXT("logPath"), LogFile);
            // The registry keeps the handle so editor.standalone_status can report the exit code.
            TrackStandaloneProcess(PID, Handle, CmdLine, LogFile);
        }
        Processes.Add(MakeShared<FJsonValueObject>(ProcEntry));
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetArrayField(TEXT("processes"), Processes);
    Ctx.SendSuccess(Resp);
    return true;
}
