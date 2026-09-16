// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/PythonCallbackRegistry.h"

#include "Compat/EngineVersionCompat.h"
#include "Editor.h"
#include "HAL/CriticalSection.h"
#include "IPythonScriptPlugin.h"
#include "Misc/ScopeLock.h"
#include "PythonScriptTypes.h"

DEFINE_LOG_CATEGORY(LogPinWrightPythonCallbacks);

namespace PinWright::PythonCallbacks
{
namespace
{
    // Imports the shim module and installs the wrappers. install() is idempotent on the
    // Python side, so re-running it after a failed attempt costs nothing.
    //
    // NO INTERPOLATED PATH, deliberately. The plugin's Content/Python is already on
    // sys.path - the engine adds it from FPythonScriptPlugin::RegisterModulePaths when the
    // plugin's content root mounts - and a defensive sys.path insert would have to
    // interpolate the install directory into this command. ExecPythonCommandEx routes the
    // whole command to RunFile the moment ".py" appears ANYWHERE in it
    // (PythonScriptPlugin.cpp:828), so a plugin installed under a directory containing
    // ".py" would silently take the file route and the tracker would never install.
    // Reached through __import__ so the console globals this runs in gain no binding.
    const TCHAR* const InstallShimScript =
        TEXT("__import__('pinwright_callbacks').install()\n");

    struct FState
    {
        FCriticalSection Mutex;
        TArray<FRecord> Records;
        // Records are keyed by a plain counter rather than a GUID because the id is read
        // by a human out of a log line and typed back into a clear call.
        int32 NextId = 1;
        bool bShimInstalled = false;
        // Sticky: a failing install writes a Python traceback to the log, and
        // EnsureTrackingReady is called from every python.execute. Retrying it per call
        // would turn one broken install into a traceback per script. Only the explicit
        // python.callbacks verb lifts it.
        bool bShimInstallFailed = false;
        FDateTime InstalledAt = FDateTime::MinValue();
        FString CurrentRequestId;
        FDelegateHandle EndPieHandle;
    };

    FState& State()
    {
        static FState Instance;
        return Instance;
    }

    // Runs a tracker script on its own FPythonCommandEx so its output never reaches a
    // caller's log array. Literal source through ExecuteFile is the only mode that takes
    // more than one statement; the engine routes to a file only when it finds ".py" in the
    // command, which none of these scripts contain.
    bool RunTrackerScript(IPythonScriptPlugin& Python, const FString& Script)
    {
        FPythonCommandEx Cmd;
        Cmd.Command = Script;
        Cmd.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
        return Python.ExecPythonCommandEx(Cmd);
    }

    void OnEndPie(const bool /*bIsSimulating*/)
    {
        const TArray<FRecord> Live = Snapshot();
        if (Live.Num() == 0)
        {
            return;
        }

        // The defect this warning exists for was silent: a callback holding a PIE actor
        // threw 30 times a second for 93 minutes and every line named the symptom.
        UE_LOG(LogPinWrightPythonCallbacks, Warning,
            TEXT("%d Python-registered callback(s) survived PIE teardown and keep running. ")
            TEXT("Anything they captured from the play world is now stale. ")
            TEXT("Remove them with python.callbacks {\"action\": \"clear\", \"ids\": [...]}."),
            Live.Num());

        for (const FRecord& Record : Live)
        {
            UE_LOG(LogPinWrightPythonCallbacks, Warning,
                TEXT("  %s kind=%s slot=%s invocations=%lld registeredAt=%s source=%s"),
                *Record.Id, *Record.Kind,
                Record.RequestId.IsEmpty() ? TEXT("<untracked-caller>") : *Record.RequestId,
                Record.Invocations, *Record.RegisteredAt.ToIso8601(), *Record.Source);
        }
    }
}

EReadyStatus EnsureTrackingReady(bool bRetryFailedInstall)
{
    {
        FScopeLock Lock(&State().Mutex);
        if (State().bShimInstalled)
        {
            return EReadyStatus::Ready;
        }
        if (State().bShimInstallFailed && !bRetryFailedInstall)
        {
            return EReadyStatus::ShimInstallFailed;
        }
        State().bShimInstallFailed = false;
    }

    IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
    if (!Python)
    {
        return EReadyStatus::PythonNotAvailable;
    }

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    if (!Python->IsPythonInitialized())
    {
        Python->ForceEnablePythonAtRuntime();
        if (!Python->IsPythonInitialized())
        {
            return EReadyStatus::PythonInitFailed;
        }
    }
#else
    if (!Python->IsPythonAvailable())
    {
        return EReadyStatus::PythonInitFailed;
    }
#endif

    if (!RunTrackerScript(*Python, InstallShimScript))
    {
        FScopeLock FailureLock(&State().Mutex);
        State().bShimInstallFailed = true;
        return EReadyStatus::ShimInstallFailed;
    }

    FScopeLock Lock(&State().Mutex);
    if (!State().bShimInstalled)
    {
        State().bShimInstalled = true;
        State().InstalledAt = FDateTime::UtcNow();
        if (GEditor)
        {
            State().EndPieHandle = FEditorDelegates::EndPIE.AddStatic(&OnEndPie);
        }
    }
    return EReadyStatus::Ready;
}

FDateTime GetTrackingInstalledAt()
{
    FScopeLock Lock(&State().Mutex);
    return State().InstalledAt;
}

FString NotifyRegistered(const FString& Kind, const FString& Source)
{
    FScopeLock Lock(&State().Mutex);

    FRecord Record;
    Record.Serial = State().NextId++;
    Record.Id = FString::Printf(TEXT("pw-cb-%d"), Record.Serial);
    Record.Kind = Kind;
    Record.RequestId = State().CurrentRequestId;
    Record.Source = Source;
    Record.RegisteredAt = FDateTime::UtcNow();

    const FString Id = Record.Id;
    State().Records.Add(MoveTemp(Record));
    return Id;
}

void NotifyUnregistered(const FString& Id)
{
    FScopeLock Lock(&State().Mutex);
    State().Records.RemoveAll([&Id](const FRecord& Record) { return Record.Id == Id; });
}

void NotifyInvoked(const FString& Id, const FString& ErrorText)
{
    FScopeLock Lock(&State().Mutex);
    for (FRecord& Record : State().Records)
    {
        if (Record.Id == Id)
        {
            ++Record.Invocations;
            if (!ErrorText.IsEmpty())
            {
                Record.LastError = ErrorText;
            }
            return;
        }
    }
}

bool IsTracked(const FString& Id)
{
    FScopeLock Lock(&State().Mutex);
    for (const FRecord& Record : State().Records)
    {
        if (Record.Id == Id)
        {
            return true;
        }
    }
    return false;
}

TArray<FRecord> Snapshot()
{
    FScopeLock Lock(&State().Mutex);
    return State().Records;
}

bool RequestClear(const TArray<FString>& Ids)
{
    if (Ids.Num() == 0)
    {
        return true;
    }

    IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
    if (!Python)
    {
        return false;
    }

    TArray<FString> Quoted;
    Quoted.Reserve(Ids.Num());
    for (const FString& Id : Ids)
    {
        Quoted.Add(FString::Printf(TEXT("'%s'"), *Id));
    }

    const FString Script = FString::Printf(
        TEXT("__import__('pinwright_callbacks').clear([%s])\n"),
        *FString::Join(Quoted, TEXT(", ")));

    return RunTrackerScript(*Python, Script);
}

FScopedRequestSlot::FScopedRequestSlot(const FString& RequestId)
{
    FScopeLock Lock(&State().Mutex);
    Previous = State().CurrentRequestId;
    Watermark = State().NextId;
    State().CurrentRequestId = RequestId;
}

FScopedRequestSlot::~FScopedRequestSlot()
{
    FScopeLock Lock(&State().Mutex);
    State().CurrentRequestId = Previous;
}

int32 FScopedRequestSlot::CountSurviving() const
{
    FScopeLock Lock(&State().Mutex);
    int32 Count = 0;
    for (const FRecord& Record : State().Records)
    {
        if (Record.Serial >= Watermark)
        {
            ++Count;
        }
    }
    return Count;
}

void Shutdown()
{
    FScopeLock Lock(&State().Mutex);
    if (State().EndPieHandle.IsValid())
    {
        FEditorDelegates::EndPIE.Remove(State().EndPieHandle);
        State().EndPieHandle.Reset();
    }

    // Everything here dies with the DLL, while the Python side (wrappers, handles,
    // _pinwright_tracked markers) lives in the interpreter and survives a reload. Clearing
    // the install flag is what makes the next EnsureTrackingReady re-run install(), whose
    // re-adoption pass files the surviving callbacks back into this registry under fresh
    // ids; leaving it set would have left them running and unclearable.
    State().bShimInstalled = false;
    State().bShimInstallFailed = false;
    State().InstalledAt = FDateTime::MinValue();
    State().CurrentRequestId.Reset();
    State().Records.Reset();
}
}
