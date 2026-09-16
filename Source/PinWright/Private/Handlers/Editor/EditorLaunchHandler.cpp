// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Editor/EditorLaunchHandlerInternal.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
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

REGISTER_RPC_HANDLER("editor.launch_standalone", "editor",
    "Launch one or more separate -game processes against the current project. "
    "Use for multiplayer test loops (set listenServer=true, numClients>1) or "
    "clean-baseline perf probes where editor tick overhead would distort the measurement. "
    "Each spawned process is detached and the editor only retains a transient handle. "
    "Returns the resolved command line and OS PID for every slot; per-slot failures "
    "are reported with error=CREATEPROC_FAILED and pid=0 instead of aborting the batch.",
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

    const FString ProjectPath = FPaths::GetProjectFilePath();
    const TCHAR* ExecutablePath = FPlatformProcess::ExecutablePath();

    TArray<TSharedPtr<FJsonValue>> Processes;
    Processes.Reserve(NumClients);

    for (int32 i = 0; i < NumClients; ++i)
    {
        const FString CmdLine = BuildStandaloneCommandLine(
            ProjectPath, Map, i, NumClients, bListenServer, ExtraArgs);

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
            // The handle is editor-side; the spawned process keeps running after we drop it.
            FPlatformProcess::CloseProc(Handle);
        }
        Processes.Add(MakeShared<FJsonValueObject>(ProcEntry));
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetArrayField(TEXT("processes"), Processes);
    Ctx.SendSuccess(Resp);
    return true;
}
