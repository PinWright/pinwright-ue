// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-python-execute-reentrant-gc-crash (first slice).
//
// A UFUNCTION a python.execute script calls can run CollectGarbage() synchronously, and the
// engine's pre-GC hook then runs a full Python gc pass inside the live script; the recorded
// editor death (#1) faulted inside that pass. python.execute now runs every script with Python's
// cyclic collector disabled (PyGC_Collect returns at once while it is) and restores the prior
// state afterwards, and it warns when a synchronous collect happened inside the script.
//
// The script forces the nested collect with `obj gc` through SystemLibrary, the same shape as
// recompile_material -> BuildTextureStreamingData -> CollectGarbage().
//
// Counterfactual: drop the gc disable/enable calls from PythonExecuteHandler.cpp and the
// in-script gc.isenabled() reads True; drop the pre-GC counter and the warning is missing.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "IPythonScriptPlugin.h"
#include "Misc/EngineVersionComparison.h"
#include "PythonScriptTypes.h"

#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

namespace PythonExecuteNestedGcSuspensionTest
{
    // Reads gc.isenabled() straight through the engine plugin, outside python.execute's guard.
    FString ReadGcEnabledOutsideHandler(IPythonScriptPlugin& Python)
    {
        FPythonCommandEx Cmd;
        Cmd.Command = TEXT("__import__('gc').isenabled()");
        Cmd.ExecutionMode = EPythonCommandExecutionMode::EvaluateStatement;
        Python.ExecPythonCommandEx(Cmd);
        return Cmd.CommandResult;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonExecuteNestedGcSuspensionTest,
    "PinWright.python.execute.NestedGarbageCollectionRunsWithPythonGcSuspended",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPythonExecuteNestedGcSuspensionTest::RunTest(const FString& Parameters)
{
    namespace Helpers = PythonExecuteNestedGcSuspensionTest;

    IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
    if (!Python || !Python->IsPythonAvailable())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("python-interpreter-unavailable"),
            TEXT("PythonScriptPlugin is not loaded or not available"));
        return true;
    }

    // Fixture precondition: the "back on afterwards" assertion below only measures the
    // handler if the collector was on before the call.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    // The handler force-enables an uninitialized interpreter; the precondition read cannot.
    if (!Python->IsPythonInitialized())
    {
        Python->ForceEnablePythonAtRuntime();
    }
#endif
    if (!TestEqual(TEXT("Precondition: Python's cyclic collector is enabled before the call"),
            Helpers::ReadGcEnabledOutsideHandler(*Python), FString(TEXT("True"))))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("code"),
        TEXT("import gc, unreal\n")
        TEXT("print('pinwright_gc_enabled_in_script=%s' % gc.isenabled())\n")
        TEXT("unreal.SystemLibrary.execute_console_command(None, 'obj gc')\n"));

    FTestResponseCapture Capture;
    if (!InvokeHandlerWithCapture(TEXT("python.execute"), Payload, Capture))
    {
        AddError(TEXT("Handler 'python.execute' not registered"));
        return false;
    }
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        if (Capture.ErrorCode == TEXT("PYTHON_NOT_AVAILABLE") ||
            Capture.ErrorCode == TEXT("PYTHON_INIT_FAILED"))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("python-interpreter-unavailable"),
                FString::Printf(TEXT("python.execute answered %s"), *Capture.ErrorCode));
            return true;
        }
        AddError(FString::Printf(TEXT("python.execute failed (errorCode='%s')"), *Capture.ErrorCode));
        return false;
    }

    // Read after the call: python.execute runs the probe inside its own guard, so only a
    // direct engine evaluation sees the state the handler left behind.
    const FString GcEnabledAfter = Helpers::ReadGcEnabledOutsideHandler(*Python);

    bool bScriptSucceeded = false;
    TestTrue(TEXT("script runs to completion"),
        Capture.Result->TryGetBoolField(TEXT("success"), bScriptSucceeded) && bScriptSucceeded);

    FString InScript;
    bool bGcWarning = false;
    const TArray<TSharedPtr<FJsonValue>>* LogEntries = nullptr;
    Capture.Result->TryGetArrayField(TEXT("log"), LogEntries);
    for (const TSharedPtr<FJsonValue>& EntryValue : LogEntries ? *LogEntries : TArray<TSharedPtr<FJsonValue>>())
    {
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        FString Output;
        if (!EntryValue.IsValid() || !EntryValue->TryGetObject(Entry) || !Entry ||
            !(*Entry)->TryGetStringField(TEXT("output"), Output))
        {
            continue;
        }
        if (Output.Contains(TEXT("pinwright_gc_enabled_in_script=")))
        {
            InScript = Output.TrimStartAndEnd();
        }
        FString Type;
        if ((*Entry)->TryGetStringField(TEXT("type"), Type) && Type == TEXT("Warning") &&
            Output.Contains(TEXT("synchronous garbage collection")) &&
            Output.Contains(TEXT("cyclic collector was disabled for the script")) &&
            Output.Contains(TEXT("material.authoring.compile_material")))
        {
            bGcWarning = true;
        }
    }

    // Precondition: the nested collect really happened, or the rest measures nothing.
    TestTrue(TEXT("`obj gc` ran a synchronous collect inside the script and the response warns about it"),
        bGcWarning);
    TestEqual(TEXT("Python's cyclic collector is off while the script runs"),
        InScript, FString(TEXT("pinwright_gc_enabled_in_script=False")));
    TestEqual(TEXT("Python's cyclic collector is back on after python.execute returns"),
        GcEnabledAfter, FString(TEXT("True")));
    return true;
}
