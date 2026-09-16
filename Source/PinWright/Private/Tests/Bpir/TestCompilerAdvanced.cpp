// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_Event.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Timeline.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "EdGraph/EdGraph.h"

using namespace CompilerTestUtils;

// ============================================================================
// 1. Compiler.Integration.LatentDelay
// Compile a latent Delay node with label wiring; verify CallFunction nodes
// are created for PrintString calls and the Delay itself.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationLatentDelayTest,
    "PinWright.bpir.compiler.integration.LatentDelay",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationLatentDelayTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("AdvCompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call PrintString(InString: \"Start\")\n")
        TEXT("    %d = latent Delay(Duration: 2.0) [completed -> @after]\n")
        TEXT("\n")
        TEXT("@after:\n")
        TEXT("    call PrintString(InString: \"Done\")\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);

    // Expect at least 3 CallFunction nodes: 2x PrintString + 1x Delay
    int32 CallCount = CountNodesOfType<UK2Node_CallFunction>(BP);
    TestTrue(TEXT("At least 3 CallFunction nodes (2x PrintString + Delay)"), CallCount >= 3);
    return true;
}

// ============================================================================
// 2. Compiler.Integration.CastNode
// Compile a cast<Character> with success/fail branches; verify a
// DynamicCast node is created.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationCastNodeTest,
    "PinWright.bpir.compiler.integration.CastNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationCastNodeTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("AdvCompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %pawn = call GetPlayerPawn(PlayerIndex: 0)\n")
        TEXT("    %cast = cast<Character>(%pawn) [success -> @ok, fail -> @nope]\n")
        TEXT("\n")
        TEXT("@ok:\n")
        TEXT("    call PrintString(InString: \"Cast succeeded\")\n")
        TEXT("\n")
        TEXT("@nope:\n")
        TEXT("    call PrintString(InString: \"Cast failed\")\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestEqual(TEXT("Exactly 1 DynamicCast node"), CountNodesOfType<UK2Node_DynamicCast>(BP), 1);
    return true;
}

// ============================================================================
// 3. Compiler.Integration.TimelineNode
// Compile a timeline with a float curve; verify a Timeline node is created.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationTimelineNodeTest,
    "PinWright.bpir.compiler.integration.TimelineNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationTimelineNodeTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("AdvCompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %tl = timeline FadeIn(Alpha: float_curve((0.0, 0.0), (2.0, 1.0))) [update -> @tick, finished -> @done]\n")
        TEXT("\n")
        TEXT("@tick:\n")
        TEXT("    call PrintString(InString: \"Ticking\")\n")
        TEXT("\n")
        TEXT("@done:\n")
        TEXT("    call PrintString(InString: \"Finished\")\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestEqual(TEXT("Exactly 1 Timeline node"), CountNodesOfType<UK2Node_Timeline>(BP), 1);
    return true;
}

// ============================================================================
// 4. Compiler.Integration.MakeStruct
// Compile a make<Vector> node; verify a MakeStruct node is created.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationMakeStructTest,
    "PinWright.bpir.compiler.integration.MakeStruct",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationMakeStructTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("AdvCompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %v = make<Vector>(X: 1.0, Y: 2.0, Z: 3.0)\n")
        TEXT("    call PrintString(InString: \"Made vector\")\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    // FVector has HasNativeMake metadata, so the compiler routes to UK2Node_CallFunction (MakeVector)
    // instead of UK2Node_MakeStruct.
    TestNotNull(TEXT("MakeVector CallFunction node exists"),
        FindCallFunctionBySubstring(BP, TEXT("MakeVector")));
    return true;
}

// ============================================================================
// 5. Compiler.Integration.BreakStruct
// Compile a break<Margin> expression; verify a BreakStruct node is created.
// FMargin has no HasNativeBreak/HasNativeMake metadata, so the compiler emits
// a real UK2Node_BreakStruct rather than routing through a CallFunction node.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationBreakStructTest,
    "PinWright.bpir.compiler.integration.BreakStruct",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationBreakStructTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("AdvCompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %m = make<Margin>(Left: 1.0, Top: 2.0, Right: 3.0, Bottom: 4.0)\n")
        TEXT("    %b = break<Margin>(%m)\n")
        TEXT("    call PrintString(InString: \"Done\")\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestTrue(TEXT("At least 1 BreakStruct node"), CountNodesOfType<UK2Node_BreakStruct>(BP) >= 1);
    return true;
}
