// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-python-execute-private-scope-leaks-sys-modules.
//
// THE DEFECT. `scope: "private"` isolates the entry script's globals dict and nothing
// else. sys.modules is process-global to the embedded interpreter, so a helper module
// imported by one call stayed cached for the life of the editor: the next call ran the
// NEW entry script against the OLD module and still reported success: true. The visible
// symptom was worse than a stale value - a traceback quoted the current file's source
// text under the cached code object's line numbers, naming a function the failing frame
// was never in.
//
// WHAT THIS MEASURES. Two runs of the same entry script against a helper module that is
// edited on disk between them. The assertion is on the SECOND run: the module must not
// already be in sys.modules, and calling into it must return the edited value. Without
// the snapshot/restore in PythonExecuteHandler.cpp the second run reports cached=True and
// the pre-edit value, so the counterfactual is the shipped defect itself rather than a
// contrived one.
//
// THE MODULE NAME CARRIES A GUID because sys.modules survives for the whole editor
// session: a fixed name would be pre-cached by an earlier run of this same test in the
// same editor and the first run's cached=False assertion would measure nothing.
//
// THE TWO HELPER BODIES DIFFER IN LENGTH, not only in content. CPython validates a cached
// .pyc against the source's mtime AND size; two same-size edits inside one filesystem
// mtime granularity could otherwise be served from __pycache__ and fail the test for a
// reason that has nothing to do with sys.modules.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

namespace PythonModuleIsolationTestHelpers
{
    // Concatenates every "output" string of the response's log array. The probe line is
    // matched inside it rather than per entry, so a host that splits or prefixes a log
    // line does not turn the assertion into a false red.
    inline FString JoinPythonLog(const TSharedPtr<FJsonObject>& Result)
    {
        FString Joined;
        if (!Result.IsValid())
        {
            return Joined;
        }

        const TArray<TSharedPtr<FJsonValue>>* LogEntries = nullptr;
        if (!Result->TryGetArrayField(TEXT("log"), LogEntries) || !LogEntries)
        {
            return Joined;
        }

        for (const TSharedPtr<FJsonValue>& EntryValue : *LogEntries)
        {
            const TSharedPtr<FJsonObject>* EntryObject = nullptr;
            if (!EntryValue.IsValid() || !EntryValue->TryGetObject(EntryObject) || !EntryObject)
            {
                continue;
            }
            FString Output;
            (*EntryObject)->TryGetStringField(TEXT("output"), Output);
            Joined += Output;
            Joined += TEXT("\n");
        }
        return Joined;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonPrivateScopeModuleIsolationTest,
    "PinWright.python.execute.PrivateScopeReimportsEditedHelperModule",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPythonPrivateScopeModuleIsolationTest::RunTest(const FString& Parameters)
{
    const FString RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower();
    const FString ModuleName = FString::Printf(TEXT("pinwright_stale_probe_%s"), *RunId);
    // Absolute: ProjectIntermediateDir() is relative to the engine binaries directory, and
    // this path is handed to sys.path, where the interpreter's own cwd would resolve it.
    const FString ModuleDir = FPaths::ConvertRelativePathToFull(
        FPaths::ProjectIntermediateDir() / TEXT("PinWright/Tests/PythonModuleIsolation") / RunId);
    const FString ModulePath = ModuleDir / (ModuleName + TEXT(".py"));

    IFileManager::Get().MakeDirectory(*ModuleDir, /*Tree=*/true);
    ON_SCOPE_EXIT
    {
        IFileManager::Get().DeleteDirectory(*ModuleDir, /*RequireExists=*/false, /*Tree=*/true);
    };

    if (!FFileHelper::SaveStringToFile(
            TEXT("def probe():\n    return 'FIRST'\n"), *ModulePath))
    {
        AddError(FString::Printf(TEXT("Could not write helper module '%s'"), *ModulePath));
        return false;
    }

    // sys.path is restored by the script itself: the handler's scrub covers sys.modules,
    // not sys.path, so an unremoved entry would outlive the test and point at a deleted
    // directory for every later script in the session.
    const FString EntryScript = FString::Printf(
        TEXT("import sys\n")
        TEXT("import unreal\n")
        TEXT("_dir = r'%s'\n")
        TEXT("_cached = '%s' in sys.modules\n")
        TEXT("sys.path.insert(0, _dir)\n")
        TEXT("try:\n")
        TEXT("    import %s as _probe\n")
        TEXT("    unreal.log('PINWRIGHT_PROBE cached=' + str(_cached) + ' value=' + _probe.probe())\n")
        TEXT("finally:\n")
        TEXT("    sys.path.remove(_dir)\n"),
        *ModuleDir, *ModuleName, *ModuleName);

    auto RunEntryScript = [this, &EntryScript](FString& OutLog, FString& OutErrorCode) -> bool
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("code"), EntryScript);
        Payload->SetStringField(TEXT("mode"), TEXT("execute_file"));
        Payload->SetStringField(TEXT("scope"), TEXT("private"));

        FTestResponseCapture Capture;
        if (!InvokeHandlerWithCapture(TEXT("python.execute"), Payload, Capture))
        {
            AddError(TEXT("Handler 'python.execute' not registered"));
            return false;
        }

        OutErrorCode = Capture.ErrorCode;
        OutLog = PythonModuleIsolationTestHelpers::JoinPythonLog(Capture.Result);
        return Capture.bSuccess && Capture.Result.IsValid();
    };

    FString FirstLog;
    FString FirstErrorCode;
    if (!RunEntryScript(FirstLog, FirstErrorCode))
    {
        // Python absent or refusing to initialize is a host property, not a defect. It is
        // reported as a skip so the run cannot be quoted as green on the strength of a
        // test that measured nothing.
        if (FirstErrorCode == TEXT("PYTHON_NOT_AVAILABLE") || FirstErrorCode == TEXT("PYTHON_INIT_FAILED"))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("python-interpreter-unavailable"),
                FString::Printf(TEXT("python.execute answered %s"), *FirstErrorCode));
            return true;
        }
        AddError(FString::Printf(TEXT("First python.execute call failed (errorCode='%s')"), *FirstErrorCode));
        return false;
    }

    // Baseline. A GUID-suffixed module cannot have been imported by anything else, so a
    // cached=True here would mean the fixture, not the fix, is what the test is reading.
    TestTrue(TEXT("first run imports the helper module fresh"),
        FirstLog.Contains(TEXT("PINWRIGHT_PROBE cached=False value=FIRST")));

    // Same module name, longer body: the on-disk source is now a different function.
    if (!FFileHelper::SaveStringToFile(
            TEXT("def probe():\n    return 'SECOND_AFTER_EDIT'\n"), *ModulePath))
    {
        AddError(FString::Printf(TEXT("Could not rewrite helper module '%s'"), *ModulePath));
        return false;
    }

    FString SecondLog;
    FString SecondErrorCode;
    if (!RunEntryScript(SecondLog, SecondErrorCode))
    {
        AddError(FString::Printf(TEXT("Second python.execute call failed (errorCode='%s')"), *SecondErrorCode));
        return false;
    }

    // THE ASSERTION. Both halves matter and they fail for different reasons: the module
    // must have been dropped from sys.modules by the previous call's restore, and the
    // re-import must have picked up the edited bytecode.
    TestTrue(TEXT("second run does not find the helper module cached in sys.modules"),
        SecondLog.Contains(TEXT("PINWRIGHT_PROBE cached=False")));
    TestTrue(TEXT("second run executes the edited helper module, not its previous bytecode"),
        SecondLog.Contains(TEXT("value=SECOND_AFTER_EDIT")));
    TestFalse(TEXT("second run does not report the pre-edit value"),
        SecondLog.Contains(TEXT("value=FIRST\n")));

    return true;
}
