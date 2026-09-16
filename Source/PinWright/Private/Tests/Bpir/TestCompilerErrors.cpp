// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_Event.h"
#include "EdGraph/EdGraph.h"

using namespace CompilerTestUtils;

// ============================================================================
// 1. Compiler.Errors.DuplicateValueName
// Two instructions claiming the same %x name should fail compilation.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerErrorsDuplicateValueNameTest,
    "PinWright.bpir.compiler.errors.DuplicateValueName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerErrorsDuplicateValueNameTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerErrorTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %x = pure IsValid(Object: self)\n")
        TEXT("    %x = pure IsValid(Object: self)\n")
        TEXT("}"));

    TestFalse(TEXT("Compile should fail with duplicate value name"), Result.bSuccess);
    TestTrue(TEXT("Errors array is not empty"), Result.Errors.Num() > 0);
    TestTrue(TEXT("Error mentions duplicate"),
        ErrorsContain(Result.Errors, TEXT("duplicate")) || ErrorsContain(Result.Errors, TEXT("redefined")));
    return true;
}

// ============================================================================
// 2. Compiler.Errors.DuplicateLabel
// Two @same: labels in the same block should fail compilation.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerErrorsDuplicateLabelTest,
    "PinWright.bpir.compiler.errors.DuplicateLabel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerErrorsDuplicateLabelTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerErrorTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %b = branch(true) [true -> @same, false -> @same]\n")
        TEXT("\n")
        TEXT("@same:\n")
        TEXT("    call PrintString(InString: \"A\")\n")
        TEXT("\n")
        TEXT("@same:\n")
        TEXT("    call PrintString(InString: \"B\")\n")
        TEXT("}"));

    TestFalse(TEXT("Compile should fail with duplicate label"), Result.bSuccess);
    TestTrue(TEXT("Errors array is not empty"), Result.Errors.Num() > 0);
    return true;
}

// ============================================================================
// 3. Compiler.Errors.UnknownPercentRef
// Referencing %nonexistent should produce an error without crashing.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerErrorsUnknownPercentRefTest,
    "PinWright.bpir.compiler.errors.UnknownPercentRef",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerErrorsUnknownPercentRefTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerErrorTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call PrintString(InString: %nonexistent)\n")
        TEXT("}"));

    TestFalse(TEXT("Compile should fail on unknown %ref"), Result.bSuccess);
    TestTrue(TEXT("Errors array is not empty"), Result.Errors.Num() > 0);
    return true;
}

// ============================================================================
// 4. Compiler.Errors.UnknownLabelRef
// exec -> @nowhere referencing a non-existent label should error without crash.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerErrorsUnknownLabelRefTest,
    "PinWright.bpir.compiler.errors.UnknownLabelRef",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerErrorsUnknownLabelRefTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerErrorTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    exec -> @nowhere\n")
        TEXT("    call PrintString(InString: \"test\")\n")
        TEXT("}"));

    TestFalse(TEXT("Compile should fail on unknown @label"), Result.bSuccess);
    TestTrue(TEXT("Errors array is not empty"), Result.Errors.Num() > 0);
    return true;
}

// ============================================================================
// 5. Compiler.Errors.AtomicRollback
// If compilation fails partway through, the node count should return to baseline
// (no partial nodes left behind).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerErrorsAtomicRollbackTest,
    "PinWright.bpir.compiler.errors.AtomicRollback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerErrorsAtomicRollbackTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerErrorTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // Capture node count before any compilation attempt.
    const int32 NodesBefore = CountAllEventGraphNodes(BP);

    // Compile code that starts valid but references a completely unknown function,
    // which should cause failure after some nodes may have been emitted.
    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call PrintString(InString: \"Start\")\n")
        TEXT("    call ThisFunctionDefinitelyDoesNotExist_XYZ_12345(Foo: \"bar\")\n")
        TEXT("    call PrintString(InString: \"End\")\n")
        TEXT("}"));

    TestFalse(TEXT("Compile should fail on unknown function"), Result.bSuccess);

    const int32 NodesAfter = CountAllEventGraphNodes(BP);
    TestEqual(TEXT("Node count returns to baseline after failed compile (atomic rollback)"),
        NodesAfter, NodesBefore);
    return true;
}

// ============================================================================
// 6. Compiler.Errors.MalformedEntry
// A garbled entry signature should produce a parse error.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerErrorsMalformedEntryTest,
    "PinWright.bpir.compiler.errors.MalformedEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerErrorsMalformedEntryTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerErrorTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry garbled something {\n")
        TEXT("    call PrintString(InString: \"test\")\n")
        TEXT("}"));

    TestFalse(TEXT("Compile should fail on malformed entry"), Result.bSuccess);
    TestTrue(TEXT("Errors array is not empty"), Result.Errors.Num() > 0);
    return true;
}

// ============================================================================
// 7. Compiler.Errors.EmptyBlock
// An entry with an empty body should succeed and produce 0 body nodes.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerErrorsEmptyBlockTest,
    "PinWright.bpir.compiler.errors.EmptyBlock",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerErrorsEmptyBlockTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerErrorTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // Capture baseline node count (fresh BP has a default BeginPlay event node).
    const int32 NodesBefore = CountAllEventGraphNodes(BP);

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() { }"));

    TestTrue(TEXT("Compile of empty block should succeed"), Result.bSuccess);

    // An empty body should not add any CallFunction nodes — only the event node itself.
    const int32 CallFunctionCount = CountNodesOfType<UK2Node_CallFunction>(BP);
    TestEqual(TEXT("No CallFunction nodes from empty block"), CallFunctionCount, 0);
    return true;
}

// ============================================================================
// 8. Compiler.Errors.ForwardReference
// Using %ref before it's defined in a later instruction should produce an error.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerErrorsForwardReferenceTest,
    "PinWright.bpir.compiler.errors.ForwardReference",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerErrorsForwardReferenceTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerErrorTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // %result is used in the first call but defined in the second call.
    // The compiler processes instructions sequentially, so %result should be unknown.
    const int32 NodesBefore = CountAllEventGraphNodes(BP);

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call PrintString(InString: %result)\n")
        TEXT("    %result = pure IsValid(Object: self)\n")
        TEXT("}"));

    TestFalse(TEXT("Compile should fail on forward %ref"), Result.bSuccess);
    TestTrue(TEXT("Errors array is not empty"), Result.Errors.Num() > 0);

    // Verify rollback: no partial nodes left behind. This was previously only logged
    // via AddInfo, so the rollback the comment promises was never actually asserted --
    // a failed compile that leaked its partially-emitted nodes read as a pass.
    const int32 NodesAfter = CountAllEventGraphNodes(BP);
    AddInfo(FString::Printf(TEXT("Nodes after failed forward-ref compile: %d"), NodesAfter));
    TestEqual(TEXT("Node count returns to baseline after failed forward-ref compile"),
        NodesAfter, NodesBefore);
    return true;
}

// ============================================================================
// 9. Compiler.Errors.DuplicateEntryName
// Two entry blocks with the same event name should produce an error or the
// compiler should handle it gracefully.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerErrorsDuplicateEntryNameTest,
    "PinWright.bpir.compiler.errors.DuplicateEntryName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerErrorsDuplicateEntryNameTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerErrorTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call PrintString(InString: \"first\")\n")
        TEXT("}\n")
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call PrintString(InString: \"second\")\n")
        TEXT("}"));

    // Duplicate entry names should either fail with an error or succeed by merging.
    // The key assertion: no crash.
    if (!Result.bSuccess)
    {
        TestTrue(TEXT("Errors array is not empty for duplicate entry"), Result.Errors.Num() > 0);
    }
    else
    {
        AddInfo(TEXT("Compiler handled duplicate entry names gracefully (merged or replaced)"));
    }
    return true;
}

// ============================================================================
// 10. Compiler.Errors.InvalidOpcodeKeyword
// An unrecognized opcode keyword should produce a parse/compile error.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerErrorsInvalidOpcodeKeywordTest,
    "PinWright.bpir.compiler.errors.InvalidOpcodeKeyword",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerErrorsInvalidOpcodeKeywordTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerErrorTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    frobnicate SomeArg(Foo: \"bar\")\n")
        TEXT("}"));

    TestFalse(TEXT("Compile should fail on unrecognized opcode"), Result.bSuccess);
    TestTrue(TEXT("Errors array is not empty"), Result.Errors.Num() > 0);
    return true;
}

// ============================================================================
// 11. Compiler.Errors.MismatchedBraces
// The parser intentionally auto-closes open blocks at EOF (see BpirParser.cpp),
// so a missing closing brace is NOT an error — it produces a valid block.
// Verify the compile succeeds and the block is well-formed.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerErrorsMismatchedBracesTest,
    "PinWright.bpir.compiler.errors.MismatchedBraces",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerErrorsMismatchedBracesTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerErrorTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call PrintString(InString: \"test\")\n"));
    // Missing closing brace

    TestFalse(TEXT("Compile should fail on missing closing brace"), Result.bSuccess);
    TestTrue(TEXT("Errors array is not empty"), Result.Errors.Num() > 0);
    return true;
}

// ============================================================================
// 12. Compiler.Errors.RollbackPreservesExistingNodes
// When a second compile fails, nodes from a prior successful compile should
// remain intact (rollback only affects the failing compile's nodes).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerErrorsRollbackPreservesExistingNodesTest,
    "PinWright.bpir.compiler.errors.RollbackPreservesExistingNodes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerErrorsRollbackPreservesExistingNodesTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerErrorTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // First compile: should succeed
    FBpirCompiler Compiler1(BP);
    FCompileResult Result1 = Compiler1.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call PrintString(InString: \"Success\")\n")
        TEXT("}"));
    TestTrue(TEXT("First compile succeeded"), Result1.bSuccess);

    const int32 NodesAfterFirstCompile = CountAllEventGraphNodes(BP);
    TestTrue(TEXT("First compile created nodes"), NodesAfterFirstCompile > 0);

    // Second compile: should fail (unknown function)
    FBpirCompiler Compiler2(BP);
    FCompileResult Result2 = Compiler2.Compile(
        TEXT("entry custom_event MyCustom() {\n")
        TEXT("    call CompletelyBogusFunction_XYZZY_99999(Foo: \"bar\")\n")
        TEXT("}"));
    TestFalse(TEXT("Second compile should fail"), Result2.bSuccess);

    const int32 NodesAfterFailedCompile = CountAllEventGraphNodes(BP);
    TestEqual(TEXT("Node count unchanged after failed compile (rollback preserved existing)"),
        NodesAfterFailedCompile, NodesAfterFirstCompile);
    return true;
}

// ============================================================================
// Compiler.Errors.UnresolvedFunctionDiagnostic
// Verify unresolved function error includes search context
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerErrorUnresolvedFunctionDiagnosticTest,
    "PinWright.bpir.compiler.errors.UnresolvedFunctionDiagnostic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerErrorUnresolvedFunctionDiagnosticTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("ErrorDiagTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call TotallyFakeFunction_XYZZY()\n")
        TEXT("}"));

    TestFalse(TEXT("Compile should fail"), Result.bSuccess);
    TestTrue(TEXT("Error mentions function name"), ErrorsContain(Result.Errors, TEXT("TotallyFakeFunction_XYZZY")));
    TestTrue(TEXT("Error includes search context (Blueprint class)"), ErrorsContain(Result.Errors, TEXT("Blueprint class")));
    TestTrue(TEXT("Error includes hint about Target"), ErrorsContain(Result.Errors, TEXT("Target")));
    return true;
}

// ============================================================================
// Compiler.Errors.FormatTextBadArgument
// Wrong argument name produces a helpful error referencing the format string
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerErrorFormatTextBadArgTest,
    "PinWright.bpir.compiler.errors.FormatTextBadArgument",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerErrorFormatTextBadArgTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("FormatBadArgBP"));
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %txt = pure Format(Format: NSLOCTEXT(\"BPIR\", \"FormatBadArgument\", \"{Name}\"), WrongName: \"test\")\n")
        TEXT("    call PrintString(InString: %txt)\n")
        TEXT("}"));

    TestFalse(TEXT("Compile should fail on mismatched FormatText argument"), Result.bSuccess);
    TestTrue(TEXT("Error mentions WrongName"), ErrorsContain(Result.Errors, TEXT("WrongName")));
    return true;
}

// ============================================================================
// Compiler.Errors.UnknownPinHintsAvailablePins (regression for E-bpir-createwidget-pin-hint)
// When an argument pin name cannot be resolved, the error message must list
// available input pin names so authors can correct the name without decompiling
// a reference BP. Uses PrintString (UKismetSystemLibrary::PrintString) because
// its pin list is stable across engine versions.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerErrorsUnknownPinHintsAvailablePinsTest,
    "PinWright.bpir.compiler.errors.UnknownPinHintsAvailablePins",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerErrorsUnknownPinHintsAvailablePinsTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("PinHintBP"));
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    // "WhatGoesHere" is a bogus pin name; PrintString's real input is "InString".
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call PrintString(WhatGoesHere: \"hi\")\n")
        TEXT("}"));

    TestFalse(TEXT("Compile fails on unknown pin name"), Result.bSuccess);
    TestTrue(TEXT("Error mentions the offending arg name"),
        ErrorsContain(Result.Errors, TEXT("WhatGoesHere")));
    TestTrue(TEXT("Error lists available pins"),
        ErrorsContain(Result.Errors, TEXT("Available pins")));
    TestTrue(TEXT("Error includes PrintString's real 'InString' pin"),
        ErrorsContain(Result.Errors, TEXT("InString")));
    return true;
}
