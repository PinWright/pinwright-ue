// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for board ticket B-console-verbs-bypass-typed-verb-guards.
//
// THE DEFECT. system.console_command / editor.console_command refused only scalability-CVar
// sets, so QUIT_EDITOR (skips editor.quit's in-use / unsaved-changes / asset-editor / job
// checks), PY (skips python.execute's scope restore, log capture and leak report), EXECFILE (runs
// a file's lines past both guards) and DEBUG CRASH-family lines reached GEditor->Exec.
//
// WHY THE HANDLER-LEVEL CASES USE ONLY EXECFILE AND PY. A handler test whose guard is reverted
// executes the line. QUIT_EDITOR would end the suite's editor and DEBUG CRASH would kill it, so
// those tokens are asserted on ConsoleCommandGuard::FindRefusal, the exact predicate both
// handlers call. The handler cases use lines that are harmless if they ever reach Exec: EXECFILE
// of a file that does not exist (ExecFile logs "Can't find file" at Log verbosity,
// EditorServer.cpp:5649) and `PY pass`.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "HAL/IConsoleManager.h"

#include "Handlers/ConsoleCommandGuard.h"
#include "Handlers/ErrorCodes.h"
#include "Tests/TestUtils.h"

namespace ConsoleCommandGuardTestSupport
{
    const TCHAR* const MissingExecFileLine = TEXT("EXECFILE PinWrightNoSuchFile_ConsoleCommandGuard.txt");
    // A plain, non-scalability console variable read: consumed by the console manager, no Set.
    const TCHAR* const ProbeCVarName = TEXT("PinWright.Test.ConsoleCommandGuardProbe");

    void ExpectRefused(FAutomationTestBase& Test, const TCHAR* Line, const TCHAR* ExpectedCode,
        const TCHAR* ExpectedUseVerb = nullptr)
    {
        const TOptional<ConsoleCommandGuard::FRefusal> Refusal = ConsoleCommandGuard::FindRefusal(Line);
        if (!Test.TestTrue(*FString::Printf(TEXT("'%s' is refused"), Line), Refusal.IsSet()))
        {
            return;
        }
        Test.TestEqual(*FString::Printf(TEXT("'%s' refusal code"), Line),
            FString(Refusal->Code), FString(ExpectedCode));
        Test.TestTrue(*FString::Printf(TEXT("'%s' refusal names force:true"), Line),
            Refusal->Message.Contains(TEXT("force:true")));
        Test.TestTrue(*FString::Printf(TEXT("'%s' refusal echoes the line"), Line),
            Refusal->Message.Contains(Line));
        if (ExpectedUseVerb)
        {
            Test.TestEqual(*FString::Printf(TEXT("'%s' useVerb"), Line),
                Refusal->UseVerb, FString(ExpectedUseVerb));
            Test.TestTrue(*FString::Printf(TEXT("'%s' refusal message names the typed verb"), Line),
                Refusal->Message.Contains(ExpectedUseVerb));
        }
    }

    void ExpectAllowed(FAutomationTestBase& Test, const TCHAR* Line)
    {
        const TOptional<ConsoleCommandGuard::FRefusal> Refusal = ConsoleCommandGuard::FindRefusal(Line);
        Test.TestFalse(*FString::Printf(TEXT("'%s' is not refused (got %s)"), Line,
                Refusal.IsSet() ? Refusal->Code : TEXT("none")),
            Refusal.IsSet());
    }

    // Registers a throwaway non-scalability cvar for one test so the "normal command still runs"
    // case reads something that exists on every host and cannot be pinned.
    class FScopedProbeCVar
    {
    public:
        FScopedProbeCVar()
        {
            IConsoleManager::Get().UnregisterConsoleObject(ProbeCVarName, /*bKeepState=*/false);
            IConsoleManager::Get().RegisterConsoleVariable(ProbeCVarName, (int32)0,
                TEXT("PinWright automation probe. Registered and removed by one test."), ECVF_Default);
        }
        ~FScopedProbeCVar()
        {
            IConsoleManager::Get().UnregisterConsoleObject(ProbeCVarName, /*bKeepState=*/false);
        }
        FScopedProbeCVar(const FScopedProbeCVar&) = delete;
        FScopedProbeCVar& operator=(const FScopedProbeCVar&) = delete;
    };

    // The handler half of the contract for one console verb.
    void AssertHandlerContract(FAutomationTestBase& Test, const FString& Method)
    {
        auto Invoke = [&Test, &Method](const TCHAR* Line, bool bForce, FTestResponseCapture& Capture)
        {
            TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
            Payload->SetStringField(TEXT("command"), Line);
            if (bForce)
            {
                Payload->SetBoolField(TEXT("force"), true);
            }
            Test.TestTrue(*FString::Printf(TEXT("%s is registered ('%s')"), *Method, Line),
                InvokeHandlerWithCapture(Method, Payload, Capture));
        };

        // 1. EXECFILE is refused, including a lower-case, whitespace-led spelling.
        for (const TCHAR* Line : { MissingExecFileLine,
                 TEXT("  \texecFile PinWrightNoSuchFile_ConsoleCommandGuard.txt") })
        {
            FTestResponseCapture Capture;
            Invoke(Line, false, Capture);
            Test.TestFalse(*FString::Printf(TEXT("%s refuses '%s'"), *Method, Line), Capture.bSuccess);
            Test.TestEqual(*FString::Printf(TEXT("%s refusal code for '%s'"), *Method, Line),
                Capture.ErrorCode, FString(ErrorCodes::ERR_EXECFILE_SEND_LINES_INDIVIDUALLY));
        }

        // 2. PY is refused and the structured payload names python.execute.
        {
            FTestResponseCapture Capture;
            Invoke(TEXT("PY pass"), false, Capture);
            Test.TestEqual(*FString::Printf(TEXT("%s refuses PY"), *Method),
                Capture.ErrorCode, FString(ErrorCodes::ERR_PYTHON_USE_TYPED_VERB));
            FString UseVerb;
            Test.TestTrue(*FString::Printf(TEXT("%s PY refusal carries useVerb"), *Method),
                Capture.Result.IsValid() && Capture.Result->TryGetStringField(TEXT("useVerb"), UseVerb));
            Test.TestEqual(*FString::Printf(TEXT("%s PY refusal steers to python.execute"), *Method),
                UseVerb, FString(TEXT("python.execute")));
        }

        // 3. force:true gets past the guard. Asserted as "not the refusal code": ExecFile of a
        //    missing file is consumed and only logs, so any verdict here is past the guard.
        {
            FTestResponseCapture Capture;
            Invoke(MissingExecFileLine, true, Capture);
            Test.TestNotEqual(*FString::Printf(TEXT("%s with force:true runs EXECFILE"), *Method),
                Capture.ErrorCode, FString(ErrorCodes::ERR_EXECFILE_SEND_LINES_INDIVIDUALLY));
        }

        // 4. An ordinary line still runs: a cvar read is consumed and succeeds.
        {
            FScopedProbeCVar Probe;
            FTestResponseCapture Capture;
            Invoke(ProbeCVarName, false, Capture);
            Test.TestTrue(*FString::Printf(TEXT("%s runs an ordinary cvar read (code=%s)"), *Method,
                    *Capture.ErrorCode),
                Capture.bSuccess);
        }
    }
}

// ============================================================================
// Every refused token, its code and its typed verb, on the predicate both handlers call.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FConsoleCommandGuardRefusedTokensTest,
    "PinWright.core.console_command_guard.RefusesEachBypassToken",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConsoleCommandGuardRefusedTokensTest::RunTest(const FString& Parameters)
{
    using namespace ConsoleCommandGuardTestSupport;

    ExpectRefused(*this, TEXT("QUIT_EDITOR"), ErrorCodes::ERR_EDITOR_QUIT_USE_TYPED_VERB, TEXT("editor.quit"));
    ExpectRefused(*this, TEXT("CLOSE_SLATE_MAINFRAME"), ErrorCodes::ERR_EDITOR_QUIT_USE_TYPED_VERB, TEXT("editor.quit"));
    ExpectRefused(*this, TEXT("PY print('x')"), ErrorCodes::ERR_PYTHON_USE_TYPED_VERB, TEXT("python.execute"));
    ExpectRefused(*this, TEXT("EXECFILE cmds.txt"), ErrorCodes::ERR_EXECFILE_SEND_LINES_INDIVIDUALLY);

    ExpectRefused(*this, TEXT("DEBUG CRASH"), ErrorCodes::ERR_DEBUG_COMMAND_CRASHES_PROCESS);
    ExpectRefused(*this, TEXT("DEBUG GPF"), ErrorCodes::ERR_DEBUG_COMMAND_CRASHES_PROCESS);
    ExpectRefused(*this, TEXT("DEBUG RENDERCRASH"), ErrorCodes::ERR_DEBUG_COMMAND_CRASHES_PROCESS);
    ExpectRefused(*this, TEXT("DEBUG ENSURE"), ErrorCodes::ERR_DEBUG_COMMAND_CRASHES_PROCESS);
    ExpectRefused(*this, TEXT("DEBUG TERMINATE"), ErrorCodes::ERR_DEBUG_COMMAND_CRASHES_PROCESS);
    ExpectRefused(*this, TEXT("DEBUG STALL 5"), ErrorCodes::ERR_DEBUG_COMMAND_HANGS_PROCESS);
    ExpectRefused(*this, TEXT("DEBUG INFINITELOOP"), ErrorCodes::ERR_DEBUG_COMMAND_HANGS_PROCESS);
    ExpectRefused(*this, TEXT("DEBUG SOFTLOCK"), ErrorCodes::ERR_DEBUG_COMMAND_HANGS_PROCESS);
    ExpectRefused(*this, TEXT("DEBUG EATMEM"), ErrorCodes::ERR_DEBUG_COMMAND_EXHAUSTS_MEMORY);
    ExpectRefused(*this, TEXT("DEBUG OOM"), ErrorCodes::ERR_DEBUG_COMMAND_EXHAUSTS_MEMORY);

    const TOptional<ConsoleCommandGuard::FRefusal> Debug = ConsoleCommandGuard::FindRefusal(TEXT("DEBUG CRASH"));
    TestEqual(TEXT("DEBUG refusal reports the matched subcommand"),
        Debug.IsSet() ? Debug->MatchedCommand : FString(), FString(TEXT("DEBUG CRASH")));
    TestTrue(TEXT("DEBUG refusal has no typed verb to steer to"), Debug.IsSet() && Debug->UseVerb.IsEmpty());
    return true;
}

// ============================================================================
// Case, whitespace and word-boundary variants follow FParse::Command, the matcher the engine's
// exec handlers use: a word ends at the first non-alphanumeric character.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FConsoleCommandGuardVariantsTest,
    "PinWright.core.console_command_guard.CaseAndWhitespaceVariants",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConsoleCommandGuardVariantsTest::RunTest(const FString& Parameters)
{
    using namespace ConsoleCommandGuardTestSupport;

    ExpectRefused(*this, TEXT("quit_editor"), ErrorCodes::ERR_EDITOR_QUIT_USE_TYPED_VERB, TEXT("editor.quit"));
    ExpectRefused(*this, TEXT("   Quit_Editor"), ErrorCodes::ERR_EDITOR_QUIT_USE_TYPED_VERB, TEXT("editor.quit"));
    ExpectRefused(*this, TEXT("\r\n\tQUIT_EDITOR"), ErrorCodes::ERR_EDITOR_QUIT_USE_TYPED_VERB, TEXT("editor.quit"));
    ExpectRefused(*this, TEXT("py import unreal"), ErrorCodes::ERR_PYTHON_USE_TYPED_VERB, TEXT("python.execute"));
    ExpectRefused(*this, TEXT("\tPy   pass"), ErrorCodes::ERR_PYTHON_USE_TYPED_VERB, TEXT("python.execute"));
    ExpectRefused(*this, TEXT("PY"), ErrorCodes::ERR_PYTHON_USE_TYPED_VERB, TEXT("python.execute"));
    // '.' is not alphanumeric, so the engine's PY handler takes this line too.
    ExpectRefused(*this, TEXT("py.foo 1"), ErrorCodes::ERR_PYTHON_USE_TYPED_VERB, TEXT("python.execute"));
    ExpectRefused(*this, TEXT("  execfile a.txt"), ErrorCodes::ERR_EXECFILE_SEND_LINES_INDIVIDUALLY);
    ExpectRefused(*this, TEXT("debug crash"), ErrorCodes::ERR_DEBUG_COMMAND_CRASHES_PROCESS);
    ExpectRefused(*this, TEXT("  Debug \t  Crash"), ErrorCodes::ERR_DEBUG_COMMAND_CRASHES_PROCESS);
    // '_' ends the word for FParse::Command, so the engine crashes on this spelling as well.
    ExpectRefused(*this, TEXT("DEBUG CRASH_now"), ErrorCodes::ERR_DEBUG_COMMAND_CRASHES_PROCESS);
    return true;
}

// ============================================================================
// Lines the guard must leave alone.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FConsoleCommandGuardAllowedTest,
    "PinWright.core.console_command_guard.AllowsOrdinaryAndProfilingLines",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConsoleCommandGuardAllowedTest::RunTest(const FString& Parameters)
{
    using namespace ConsoleCommandGuardTestSupport;

    ExpectAllowed(*this, TEXT(""));
    ExpectAllowed(*this, TEXT("stat unit"));
    ExpectAllowed(*this, TEXT("r.VSync 0"));
    // Bounded profiling sleeps, kept available on purpose.
    ExpectAllowed(*this, TEXT("DEBUG HITCH"));
    ExpectAllowed(*this, TEXT("DEBUG HITCH 200"));
    ExpectAllowed(*this, TEXT("debug renderhitch 50"));
    ExpectAllowed(*this, TEXT("DEBUG RESETLOADERS"));
    ExpectAllowed(*this, TEXT("DEBUG"));
    // Word boundaries: a longer alphanumeric word is a different command.
    ExpectAllowed(*this, TEXT("python.execute"));
    ExpectAllowed(*this, TEXT("pyfoo 1"));
    ExpectAllowed(*this, TEXT("DEBUGGER"));
    ExpectAllowed(*this, TEXT("DEBUG CRASHER"));
    ExpectAllowed(*this, TEXT("QUIT_EDITORS"));
    // Only the first command word counts.
    ExpectAllowed(*this, TEXT("log LogTemp QUIT_EDITOR"));
    ExpectAllowed(*this, TEXT("r.Foo PY"));
    // MACRO / EXEC open a suppressed "deprecated command" dialog; EXIT / QUIT are not editor quit
    // commands on the GEditor->Exec route (only ULocalPlayer::Exec_Editor handles them, in PIE).
    ExpectAllowed(*this, TEXT("MACRO foo"));
    ExpectAllowed(*this, TEXT("EXEC foo.txt"));
    ExpectAllowed(*this, TEXT("EXIT"));
    ExpectAllowed(*this, TEXT("QUIT"));
    return true;
}

// ============================================================================
// Both verbs consult the guard, honour force:true and still run ordinary lines.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemConsoleCommandTypedVerbGuardTest,
    "PinWright.system.console_command.TypedVerbBypassRefusedWithoutForce",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemConsoleCommandTypedVerbGuardTest::RunTest(const FString& Parameters)
{
    ConsoleCommandGuardTestSupport::AssertHandlerContract(*this, TEXT("system.console_command"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorConsoleCommandTypedVerbGuardTest,
    "PinWright.editor.console_command.TypedVerbBypassRefusedWithoutForce",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorConsoleCommandTypedVerbGuardTest::RunTest(const FString& Parameters)
{
    ConsoleCommandGuardTestSupport::AssertHandlerContract(*this, TEXT("editor.console_command"));
    return true;
}
