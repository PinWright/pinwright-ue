// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-bpir-override-errors-line: override resolution and signature
// diagnostics raised while setting up an entry were built as FCompileError(-1, ...), so a
// multi-entry document reported `Line -1` and the caller had to bisect blocks to find the
// offender. The parser now records each block's `entry` line and the compiler stamps every
// line-less entry-setup error with it. Counterfactual: drop the stamp loop after
// SetupEntryPoint in BpirCompiler.cpp and each Line assertion below reads -1.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"

using namespace CompilerTestUtils;

namespace BpirOverrideErrorLinesTestHelpers
{
    // Line of the first error whose message contains Needle, or INDEX_NONE - 1 when absent
    // (distinct from the -1 the defect produced).
    int32 LineOfError(const FCompileResult& Result, const TCHAR* Needle)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            if (Err.Message.Contains(Needle))
            {
                return Err.Line;
            }
        }
        return INDEX_NONE - 1;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirOverrideErrorsCarryEntryLineTest,
    "PinWright.bpir.compiler.integration.OverrideErrorsCarryEntryLine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirOverrideErrorsCarryEntryLineTest::RunTest(const FString& Parameters)
{
    using namespace BpirOverrideErrorLinesTestHelpers;

    // Override-kind entries: resolution failure (line 4) and signature mismatch (line 7),
    // after a valid first entry so the offending blocks are not on line 1.
    {
        UBlueprint* BP = CreateTransientTestBP(TEXT("BpirOverrideErrorLinesBP"));
        if (!TestNotNull(TEXT("Blueprint created"), BP))
        {
            return false;
        }
        FBpirCompiler Compiler(BP);
        const FCompileResult Result = Compiler.Compile(
            TEXT("entry event BeginPlay() {\n")                         // 1
            TEXT("    call PrintString(InString: \"ok\")\n")           // 2
            TEXT("}\n")                                                 // 3
            TEXT("entry override PwNoSuchParentFunction() {\n")         // 4
            TEXT("    call PrintString(InString: \"a\")\n")            // 5
            TEXT("}\n")                                                 // 6
            TEXT("entry override ReceiveTick(int Wrong) {\n")           // 7
            TEXT("    call PrintString(InString: \"b\")\n")            // 8
            TEXT("}\n"));                                               // 9
        for (const FCompileError& Err : Result.Errors)
        {
            AddInfo(FString::Printf(TEXT("L%d: %s"), Err.Line, *Err.Message));
        }
        TestFalse(TEXT("document with bad overrides fails"), Result.bSuccess);
        TestEqual(TEXT("override resolution error carries its entry line"),
            LineOfError(Result, TEXT("No overridable parent function named 'PwNoSuchParentFunction'")), 4);
        TestEqual(TEXT("override signature mismatch carries its entry line"),
            LineOfError(Result, TEXT("does not match the parent signature")), 7);
    }

    // Function-kind entry with an explicit signature that collides with an overridable parent
    // function: the SetupEntryPoint Function arm's mismatch diagnostic.
    {
        UBlueprint* BP = CreateTransientTestBP(TEXT("BpirFunctionOverrideErrorLinesBP"));
        if (!TestNotNull(TEXT("Blueprint created"), BP))
        {
            return false;
        }
        FBpirCompiler Compiler(BP);
        const FCompileResult Result = Compiler.Compile(
            TEXT("entry event BeginPlay() {\n")                         // 1
            TEXT("    call PrintString(InString: \"ok\")\n")           // 2
            TEXT("}\n")                                                 // 3
            TEXT("entry function ReceiveTick(int Wrong) {\n")           // 4
            TEXT("    call PrintString(InString: \"c\")\n")            // 5
            TEXT("}\n"));                                               // 6
        for (const FCompileError& Err : Result.Errors)
        {
            AddInfo(FString::Printf(TEXT("L%d: %s"), Err.Line, *Err.Message));
        }
        TestFalse(TEXT("function colliding with a parent override signature fails"), Result.bSuccess);
        TestEqual(TEXT("function-arm override mismatch carries its entry line"),
            LineOfError(Result, TEXT("authored signature does not match the parent override signature")), 4);
    }

    return true;
}
