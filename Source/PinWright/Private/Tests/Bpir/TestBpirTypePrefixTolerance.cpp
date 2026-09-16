// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "K2Node_GetSubsystem.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_VariableGet.h"

using namespace CompilerTestUtils;

// ============================================================================
// SubsystemUPrefix
// `subsystem<UGameInstanceSubsystem>()` resolves via ResolveUClass's prefix-strip
// path; compile succeeds and a UK2Node_GetSubsystem is emitted.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirSubsystemUPrefixToleratedTest,
    "PinWright.bpir.compiler.type_prefix.SubsystemUPrefix",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirSubsystemUPrefixToleratedTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %ss = subsystem<UGameInstanceSubsystem>()\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded with U-prefixed subsystem class"), Result.bSuccess);
    TestTrue(TEXT("UK2Node_GetSubsystem node was emitted"),
        CountNodesOfType<UK2Node_GetSubsystem>(BP) > 0);
    return true;
}

// ============================================================================
// MakeStructFPrefix
// `make<FVector>(...)` resolves via ResolveUScriptStruct's F-strip path; compile
// succeeds and a UK2Node_MakeStruct is emitted.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirMakeStructFPrefixToleratedTest,
    "PinWright.bpir.compiler.type_prefix.MakeStructFPrefix",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirMakeStructFPrefixToleratedTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %v = make<FVector>(X: 1.0, Y: 2.0, Z: 3.0)\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded with F-prefixed struct name"), Result.bSuccess);
    TestTrue(TEXT("UK2Node_MakeStruct node was emitted"),
        CountNodesOfType<UK2Node_MakeStruct>(BP) > 0);
    return true;
}

// ============================================================================
// BreakStructFPrefix
// `break<FVector>($v)` (with F) resolves via ResolveUScriptStruct's F-strip
// path; the prelude `make<Vector>` (no F) exercises the already-working
// short-name path. Both compile and a UK2Node_BreakStruct is emitted.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirBreakStructFPrefixToleratedTest,
    "PinWright.bpir.compiler.type_prefix.BreakStructFPrefix",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirBreakStructFPrefixToleratedTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %v = make<Vector>(X: 1.0, Y: 2.0, Z: 3.0)\n")
        TEXT("    break<FVector>($v)\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded with F-prefixed break struct"), Result.bSuccess);
    TestTrue(TEXT("UK2Node_BreakStruct node was emitted"),
        CountNodesOfType<UK2Node_BreakStruct>(BP) > 0);
    return true;
}

// ============================================================================
// DollarLocalFallbackNoOrphanGetter
// `$v` falls back to an existing `%v` local without pre-emitting an orphan
// VariableGet for a non-member Blueprint variable.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirDollarLocalFallbackNoOrphanGetterTest,
    "PinWright.bpir.compiler.DollarLocalFallback.NoOrphanGetter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirDollarLocalFallbackNoOrphanGetterTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %v = make<Vector>(X: 1.0, Y: 2.0, Z: 3.0)\n")
        TEXT("    break<FVector>($v)\n")
        TEXT("    %alias = $v\n")
        TEXT("    break<FVector>(%alias)\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded with direct and alias dollar local fallback"), Result.bSuccess);
    TestTrue(TEXT("UK2Node_BreakStruct node was emitted"),
        CountNodesOfType<UK2Node_BreakStruct>(BP) > 0);
    TestEqual(TEXT("No orphan UK2Node_VariableGet nodes were emitted"),
        CountNodesOfType<UK2Node_VariableGet>(BP), 0);
    return true;
}
