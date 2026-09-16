// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "HAL/FileManager.h"
#include "IPythonScriptPlugin.h"
#include "Compat/EngineVersionCompat.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "PinWrightSubsystem.h"
#include "PythonScriptTypes.h"
#include "State/ActiveProgressSink.h"
#include "State/JobRegistry.h"
#include "State/PluginState.h"
#include "Utils/PieState.h"
#include "Utils/PythonCallbackRegistry.h"

namespace
{
    bool IsPythonFileCommand(const FString& Command)
    {
        const FString Trimmed = Command.TrimStartAndEnd();
        FString FilePath;

        if (Trimmed.StartsWith(TEXT("\"")))
        {
            int32 ClosingQuotePos = INDEX_NONE;
            for (int32 Index = 1; Index < Trimmed.Len(); ++Index)
            {
                if (Trimmed[Index] == TEXT('"'))
                {
                    ClosingQuotePos = Index;
                    break;
                }
            }
            if (ClosingQuotePos == INDEX_NONE)
            {
                return false;
            }
            FilePath = Trimmed.Mid(1, ClosingQuotePos - 1);
        }
        else
        {
            int32 EndPathPos = Trimmed.Len();
            for (int32 Index = 0; Index < Trimmed.Len(); ++Index)
            {
                if (FChar::IsWhitespace(Trimmed[Index]))
                {
                    EndPathPos = Index;
                    break;
                }
            }
            FilePath = Trimmed.Left(EndPathPos);
        }

        return FilePath.EndsWith(TEXT(".py"), ESearchCase::IgnoreCase);
    }

    FString MakePrivateInlineScriptPath()
    {
        const FString TempDir = FPaths::ProjectIntermediateDir() / TEXT("PinWright/Python");
        IFileManager::Get().MakeDirectory(*TempDir, /*Tree=*/true);
        return TempDir / FString::Printf(TEXT("InlinePython_%s.py"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // WHY THESE TWO SCRIPTS EXIST. EPythonFileExecutionScope::Private isolates the globals
    // dict handed to the file and nothing else. sys.modules belongs to the embedded
    // interpreter and is process-global, so every module a private script imports stays
    // cached for the life of the editor: edit that helper on disk, call python.execute
    // again, and the NEW entry script runs against the OLD module while the call still
    // reports success. Worse, the traceback then quotes the current file's source text
    // under the cached code object's line numbers, so it names the wrong function and
    // reads as file corruption rather than a stale import
    // (board: B-python-execute-private-scope-leaks-sys-modules).
    //
    // Snapshotting the module table around the call is what makes "private" mean what the
    // parameter says: anything the script imported is dropped afterwards, and anything it
    // replaced or deleted is put back. The cost is that a private call re-imports what it
    // imports every time; `scope: "public"` is the escape hatch for a script that wants
    // the cache (documented on the python overlay page).
    //
    // The stack, rather than a single slot, is because a snapshot must survive whatever
    // the script itself does - including another private execution nested inside it.
    const TCHAR* const SnapshotModulesScript =
        TEXT("import sys\n")
        TEXT("sys.__dict__.setdefault('_pinwright_module_snapshots', []).append(dict(sys.modules))\n");

    // Deletion runs through a comprehension so its loop variable stays inside the
    // comprehension's own scope: these scripts execute in the UE console globals (see
    // RunModuleTableScript below), and the only name they may leave behind is `sys`.
    const TCHAR* const RestoreModulesScript =
        TEXT("import sys\n")
        TEXT("_pinwright_snapshots = sys.__dict__.get('_pinwright_module_snapshots') or []\n")
        TEXT("if _pinwright_snapshots:\n")
        TEXT("    _pinwright_before = _pinwright_snapshots.pop()\n")
        TEXT("    [sys.modules.pop(_n, None) for _n in list(sys.modules) if _n not in _pinwright_before]\n")
        TEXT("    sys.modules.update(_pinwright_before)\n")
        TEXT("    del _pinwright_before\n")
        TEXT("del _pinwright_snapshots\n");

    // Runs one of the two scripts above on its own FPythonCommandEx, so neither one's
    // output can reach the caller's log array. ExecuteFile with literal source (never a
    // path - the engine only takes the file route when it finds a ".py" in the command)
    // is the mode that accepts more than a single statement; it evaluates in the console
    // globals dict, which is why both scripts are written to bind nothing that outlives
    // them.
    bool RunModuleTableScript(IPythonScriptPlugin& Python, const TCHAR* Script)
    {
        FPythonCommandEx ScriptCmd;
        ScriptCmd.Command = Script;
        ScriptCmd.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
        return Python.ExecPythonCommandEx(ScriptCmd);
    }
}

// ---- python.execute ----
REGISTER_RPC_HANDLER("python.execute", "python", "Execute Python code or a .py file via UE's built-in Python interpreter",
    RPC_PARAMS(
        RPC_PARAM_REQ("code", "string", "Python code string or path to a .py file"),
        RPC_PARAM_DEF("mode", "string", "Execution mode: execute_file (default), execute_statement, or evaluate_statement", "execute_file"),
        RPC_PARAM_DEF("scope", "string", "Execution scope: private (default; fresh globals, and sys.modules is restored afterwards so an edited helper module is re-imported next call) or public (shared console globals, imports stay cached)", "private")
    ))
{
    FString Code;
    if (!Ctx.RequireString(TEXT("code"), Code))
    {
        return true;
    }

    const FString Mode = Ctx.GetString(TEXT("mode"), TEXT("execute_file"));
    const FString Scope = Ctx.GetString(TEXT("scope"), TEXT("private"));

    EPythonCommandExecutionMode ExecMode;
    if (Mode == TEXT("execute_file"))
    {
        ExecMode = EPythonCommandExecutionMode::ExecuteFile;
    }
    else if (Mode == TEXT("execute_statement"))
    {
        ExecMode = EPythonCommandExecutionMode::ExecuteStatement;
    }
    else if (Mode == TEXT("evaluate_statement"))
    {
        ExecMode = EPythonCommandExecutionMode::EvaluateStatement;
    }
    else
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            FString::Printf(TEXT("Invalid mode '%s'. Expected: execute_file, execute_statement, evaluate_statement"), *Mode));
        return true;
    }

    EPythonFileExecutionScope FileScope;
    if (Scope == TEXT("private"))
    {
        FileScope = EPythonFileExecutionScope::Private;
    }
    else if (Scope == TEXT("public"))
    {
        FileScope = EPythonFileExecutionScope::Public;
    }
    else
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            FString::Printf(TEXT("Invalid scope '%s'. Expected: private, public"), *Scope));
        return true;
    }

    IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
    if (!Python)
    {
        Ctx.SendError(TEXT("PYTHON_NOT_AVAILABLE"),
            TEXT("PythonScriptPlugin is not loaded. Enable it in Edit > Plugins."));
        return true;
    }

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    // IsPythonInitialized() and ForceEnablePythonAtRuntime() were added in UE 5.6;
    // on 5.4/5.5 the plugin is considered initialized once loaded (IsPythonAvailable() is
    // the only readiness check available, and the plugin is loaded by the time Get() succeeds).
    if (!Python->IsPythonInitialized())
    {
        Python->ForceEnablePythonAtRuntime();

        if (!Python->IsPythonInitialized())
        {
            Ctx.SendError(TEXT("PYTHON_INIT_FAILED"),
                TEXT("Python could not be initialized. Check Output Log for details."));
            return true;
        }
    }
#else
    // UE 5.4/5.5: fall back to IsPythonAvailable() — the plugin is ready if it is loaded
    if (!Python->IsPythonAvailable())
    {
        Ctx.SendError(TEXT("PYTHON_INIT_FAILED"),
            TEXT("Python is not available. Ensure the plugin was built with Python support."));
        return true;
    }
#endif

    // Installed here rather than inside RunPython because the shim module has to be in
    // sys.modules BEFORE the snapshot below, or the restore would drop it after every
    // private call and the next call would re-import it into a fresh, empty registry.
    const bool bCallbackTrackingReady =
        PinWright::PythonCallbacks::EnsureTrackingReady()
            == PinWright::PythonCallbacks::EReadyStatus::Ready;
    const FString RequestId = Ctx.GetRequestId();

    FPythonCommandEx Cmd;
    FString CommandToExecute = Code;
    FString TempScriptPath;

    if (ExecMode == EPythonCommandExecutionMode::ExecuteFile &&
        FileScope == EPythonFileExecutionScope::Private &&
        !IsPythonFileCommand(Code))
    {
        // UE ignores FileExecutionScope for inline strings; a temp script forces
        // the private RunFile path so PIE object wrappers do not persist globally.
        TempScriptPath = MakePrivateInlineScriptPath();
        if (!FFileHelper::SaveStringToFile(Code, *TempScriptPath))
        {
            Ctx.SendError(TEXT("TEMP_FILE_WRITE_FAILED"),
                FString::Printf(TEXT("Could not write temporary Python script '%s'"), *TempScriptPath));
            return true;
        }
        CommandToExecute = TempScriptPath;
    }

    Cmd.Command = CommandToExecute;
    Cmd.ExecutionMode = ExecMode;
    Cmd.FileExecutionScope = FileScope;

    // The module-table scrub applies exactly where private scope applies: the file path.
    // UE ignores FileExecutionScope for the two statement modes, so claiming isolation
    // there would be a second false promise rather than a fix for the first.
    const bool bIsolateModules =
        ExecMode == EPythonCommandExecutionMode::ExecuteFile &&
        FileScope == EPythonFileExecutionScope::Private;

    // Runs the interpreter and builds the response body. Identical on both paths below,
    // so a streaming caller and a plain-JSON caller cannot receive different answers for
    // the same script.
    auto RunPython = [&Python, &Cmd, &TempScriptPath, bIsolateModules,
                      bCallbackTrackingReady, &RequestId]() -> TSharedPtr<FJsonObject>
    {
        const bool bPieActiveBeforeExecution = PinWrightPieState::IsPlayInEditorActive();
        const bool bSnapshotted = bIsolateModules && RunModuleTableScript(*Python, SnapshotModulesScript);

        // Anything the script registers on this stack is attributed to this call, so the
        // response can say what the call left behind rather than leaving it to be found
        // later by whoever the leaked callback starts throwing at.
        PinWright::PythonCallbacks::FScopedRequestSlot RegisteringSlot(RequestId);
        const bool bSuccess = Python->ExecPythonCommandEx(Cmd);
        const bool bPieActiveDuringExecution =
            bPieActiveBeforeExecution || PinWrightPieState::IsPlayInEditorActive();

        // Unconditional on the script's own verdict: a script that raised has still
        // imported whatever it imported before it raised.
        const bool bRestored = !bSnapshotted || RunModuleTableScript(*Python, RestoreModulesScript);

        // Read BEFORE the temp file is dealt with, because the answer decides its fate.
        const int32 LeakedCallbacks =
            bCallbackTrackingReady ? RegisteringSlot.CountSurviving() : 0;

        // A callback that outlived this call names the temp script as its source, and
        // deleting the script is what turned that name into a dead end: the log line quoted
        // InlinePython_<guid>.py for 93 minutes with the file already gone. Keep it exactly
        // when something survived to point at it; every other call still cleans up.
        const bool bRetainScript = !TempScriptPath.IsEmpty() && LeakedCallbacks > 0;
        if (!TempScriptPath.IsEmpty() && !bRetainScript)
        {
            IFileManager::Get().Delete(*TempScriptPath, /*RequireExists=*/false, /*EvenReadOnly=*/true);
        }

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("success"), bSuccess);
        Result->SetStringField(TEXT("result"), Cmd.CommandResult);

        TArray<TSharedPtr<FJsonValue>> LogArray;
        for (const FPythonLogOutputEntry& Entry : Cmd.LogOutput)
        {
            TSharedPtr<FJsonObject> LogEntry = MakeShared<FJsonObject>();
            LogEntry->SetStringField(TEXT("type"), LexToString(Entry.Type));
            LogEntry->SetStringField(TEXT("output"), Entry.Output);
            LogArray.Add(MakeShared<FJsonValueObject>(LogEntry));
        }

        // The scrub is the only thing standing between this call and another call's
        // cached imports, so a scrub that did not run is reported rather than assumed.
        // Reporting it is the whole point of the fix: the defect it repairs was silent.
        if (bIsolateModules && !(bSnapshotted && bRestored))
        {
            TSharedPtr<FJsonObject> LogEntry = MakeShared<FJsonObject>();
            LogEntry->SetStringField(TEXT("type"), LexToString(EPythonLogOutputType::Warning));
            LogEntry->SetStringField(TEXT("output"),
                TEXT("PinWright could not isolate sys.modules for this private call, so modules ")
                TEXT("imported by earlier calls are still cached. Call importlib.reload on any ")
                TEXT("helper module you edited on disk."));
            LogArray.Add(MakeShared<FJsonValueObject>(LogEntry));
        }

        if (bPieActiveDuringExecution)
        {
            Result->SetBoolField(TEXT("pieActive"), true);

            TSharedPtr<FJsonObject> LogEntry = MakeShared<FJsonObject>();
            LogEntry->SetStringField(TEXT("type"), LexToString(EPythonLogOutputType::Warning));
            LogEntry->SetStringField(TEXT("output"),
                TEXT("PIE is active. Unreal editor scripting calls can return failure sentinels without failing this script; ")
                TEXT("StaticMeshEditorSubsystem.get_lod_count returns -1 and get_num_uv_channels returns 0 in this state. ")
                TEXT("Do not treat those values as measurements; use a typed PinWright verb or retry after PIE ends."));
            LogArray.Add(MakeShared<FJsonValueObject>(LogEntry));
        }

        // A tick or shutdown callback registered here outlives this call, PIE, and every
        // later request: the handle the script needed to remove it died with the script's
        // namespace. Reporting the count is what turns that from a silent leak into a fact
        // the caller can act on with python.callbacks.
        if (bCallbackTrackingReady)
        {
            Result->SetNumberField(TEXT("leakedCallbacks"), LeakedCallbacks);
            if (bRetainScript)
            {
                Result->SetStringField(TEXT("retainedScript"), TempScriptPath);
            }

            if (LeakedCallbacks > 0)
            {
                FString Output = FString::Printf(
                    TEXT("This script left %d editor callback(s) registered. They keep running after ")
                    TEXT("this call and after PIE ends, and the handle needed to remove them is gone ")
                    TEXT("with the script's namespace. List them with python.callbacks {\"action\": ")
                    TEXT("\"list\"} and remove them with {\"action\": \"clear\", \"ids\": [...]}."),
                    LeakedCallbacks);
                if (bRetainScript)
                {
                    Output += FString::Printf(
                        TEXT(" The script was kept at '%s' so the source line those callbacks ")
                        TEXT("report stays readable; delete it once they are cleared."),
                        *TempScriptPath);
                }

                TSharedPtr<FJsonObject> LogEntry = MakeShared<FJsonObject>();
                LogEntry->SetStringField(TEXT("type"), LexToString(EPythonLogOutputType::Warning));
                LogEntry->SetStringField(TEXT("output"), Output);
                LogArray.Add(MakeShared<FJsonValueObject>(LogEntry));
            }
        }
        else
        {
            TSharedPtr<FJsonObject> LogEntry = MakeShared<FJsonObject>();
            LogEntry->SetStringField(TEXT("type"), LexToString(EPythonLogOutputType::Warning));
            LogEntry->SetStringField(TEXT("output"),
                TEXT("PinWright could not install its callback tracker for this call, so a tick or ")
                TEXT("timer callback this script registered is neither counted nor clearable. ")
                TEXT("Keep the handle in a module you can import again and unregister it yourself."));
            LogArray.Add(MakeShared<FJsonValueObject>(LogEntry));
        }

        Result->SetArrayField(TEXT("log"), LogArray);
        return Result;
    };

    // A streaming caller (params._meta.progressToken + Accept: text/event-stream, and no
    // args.wait=false) gets a ticket so the script can emit MCP progress through
    // unreal.PinWrightProgressLibrary.report_progress while it runs. Everyone else takes
    // the original synchronous path unchanged: turning this verb into a ticket for
    // plain-JSON callers would replace their result with a ticket id they never asked for.
    UPinWrightSubsystem* Subsystem = Ctx.GetSubsystem();
    const bool bStreaming =
        Subsystem && Subsystem->IsStreamingRequest(Ctx.GetRequestId());

    if (!bStreaming)
    {
        // Publish "nobody is listening" for the duration rather than leaving whatever a
        // caller further up the stack published: a script's report_progress must never be
        // attributed to some other verb's job just because that verb invoked this one.
        // Braces, not parens: FScopedSink NoSink(FString()) is a function declaration.
        PinWright::Progress::FScopedSink NoSink{FString()};
        // Always return structured result — callers need the log array especially on failure
        Ctx.SendSuccess(RunPython());
        return true;
    }

    // No BindNativeDelegate: there is no engine completion hook to wait on, because the
    // work is this handler's own synchronous call below. StartJob only allocates the
    // ticket, registers the stream, and returns — the script then runs on this same stack
    // exactly as it did before, and this handler calls Complete itself.
    FJobBindArgs Args;
    Args.Method = TEXT("python.execute");

    // StartJob suppresses the immediate ticket response for a streaming request and
    // resolves the HTTP request from the job's terminal event instead, so the client's
    // final SSE frame carries this handler's normal body.
    const FString TicketId = Ctx.StartJob(Args);

    TSharedPtr<FJsonObject> Result;
    {
        PinWright::Progress::FScopedSink Sink(TicketId);
        Result = RunPython();
    }

    // bSuccess is true for a script that ran, whatever the script concluded: the
    // interpreter's own verdict is the measured "success" field inside Result, and
    // collapsing the two would make a streaming caller see isError:true where a
    // plain-JSON caller sees a structured failure report.
    FPluginState::Get().GetJobRegistry().Complete(TicketId, /*bSuccess=*/true, Result, FString());
    return true;
}
