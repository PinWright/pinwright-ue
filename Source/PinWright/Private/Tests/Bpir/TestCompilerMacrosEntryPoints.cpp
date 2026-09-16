// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_Event.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_InputKey.h"
#include "K2Node_Select.h"
#include "EdGraph/EdGraph.h"
#include "Tests/TestSkipReporting.h"

using namespace CompilerTestUtils;

// ============================================================================
// 1. Compiler.Integration.MacroDoOnce
// Compile a DoOnce macro with a completed branch; verify a MacroInstance node.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationMacroDoOnceTest,
    "PinWright.bpir.compiler.integration.MacroDoOnce",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationMacroDoOnceTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("MacroEntryTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %m = macro DoOnce() [completed -> @done]\n")
        TEXT("\n")
        TEXT("@done:\n")
        TEXT("    call PrintString(InString: \"Once\")\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestTrue(TEXT("At least one MacroInstance node exists"),
        CountNodesOfType<UK2Node_MacroInstance>(BP) >= 1);
    return true;
}

// ============================================================================
// 2. Compiler.Integration.MacroFlipFlop
// Compile a FlipFlop macro with A/B branches.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationMacroFlipFlopTest,
    "PinWright.bpir.compiler.integration.MacroFlipFlop",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationMacroFlipFlopTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("MacroEntryTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %ff = macro FlipFlop() [A -> @pathA, B -> @pathB]\n")
        TEXT("\n")
        TEXT("@pathA:\n")
        TEXT("    call PrintString(InString: \"A\")\n")
        TEXT("\n")
        TEXT("@pathB:\n")
        TEXT("    call PrintString(InString: \"B\")\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestTrue(TEXT("At least one MacroInstance node exists"),
        CountNodesOfType<UK2Node_MacroInstance>(BP) >= 1);
    return true;
}

// ============================================================================
// 3. Compiler.Integration.MacroGate
// Compile a Gate macro with Open: true and an exit branch.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationMacroGateTest,
    "PinWright.bpir.compiler.integration.MacroGate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationMacroGateTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("MacroEntryTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %g = macro Gate(Open: true) [exit -> @through]\n")
        TEXT("\n")
        TEXT("@through:\n")
        TEXT("    call PrintString(InString: \"Through gate\")\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestTrue(TEXT("At least one MacroInstance node exists"),
        CountNodesOfType<UK2Node_MacroInstance>(BP) >= 1);
    return true;
}

// ============================================================================
// 4. Compiler.Integration.MacroMultiGate
// Compile a MultiGate macro with 0/1 output branches.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationMacroMultiGateTest,
    "PinWright.bpir.compiler.integration.MacroMultiGate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationMacroMultiGateTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("MacroEntryTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %mg = macro MultiGate() [0 -> @out0, 1 -> @out1]\n")
        TEXT("\n")
        TEXT("@out0:\n")
        TEXT("    call PrintString(InString: \"Out 0\")\n")
        TEXT("\n")
        TEXT("@out1:\n")
        TEXT("    call PrintString(InString: \"Out 1\")\n")
        TEXT("}"));

    // MultiGate may not exist in StandardMacros on all UE versions (e.g. UE 5.6+).
    // If it fails, verify it's a clean error about the macro not being found, not a crash.
    if (!Result.bSuccess)
    {
        bool bIsMacroNotFound = false;
        for (const FCompileError& Err : Result.Errors)
        {
            AddInfo(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
            if (Err.Message.Contains(TEXT("not found")))
            {
                bIsMacroNotFound = true;
            }
        }
        if (bIsMacroNotFound)
        {
            // Greppable SKIPPED marker rather than AddWarning: this runner treats
            // AddWarning as a test failure (see TestCompilerIntegration.cpp ~3322), and
            // MultiGate really is absent from StandardMacros on some engine versions, so
            // a warning here would turn a legitimate environment skip into a red test.
            // The marker at least makes the abandoned assertions greppable in the log.
            PinWrightTestSkip::SkipAssertions(*this, TEXT("multigate-macro-unavailable"),
                TEXT("SKIPPED: MultiGate macro not available in this UE version's StandardMacros; skipping MultiGate entry-point test."));
            return true;
        }
        // Unexpected failure
        for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); }
        TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    }
    else
    {
        TestTrue(TEXT("At least one MacroInstance node exists"),
            CountNodesOfType<UK2Node_MacroInstance>(BP) >= 1);
    }
    return true;
}

// ============================================================================
// 5. Compiler.Integration.ConstructionEntry
// Compile a ConstructionScript entry point; verify compilation succeeds.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationConstructionEntryTest,
    "PinWright.bpir.compiler.integration.ConstructionEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationConstructionEntryTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("MacroEntryTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry construction ConstructionScript() {\n")
        TEXT("    call PrintString(InString: \"Constructing\")\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);

    // "It compiled" is a proxy, not the property: the whole point of the construction
    // entry kind is that the body lands in UserConstructionScript rather than the
    // ubergraph. SetupConstructionScript() silently returning an ubergraph pin instead
    // still compiles cleanly, so without this the entry kind is untested.
    UEdGraph* ConstructionGraph = nullptr;
    for (UEdGraph* Graph : BP->FunctionGraphs)
    {
        if (Graph && Graph->GetName() == TEXT("UserConstructionScript"))
        {
            ConstructionGraph = Graph;
            break;
        }
    }
    TestNotNull(TEXT("UserConstructionScript graph exists"), ConstructionGraph);
    if (ConstructionGraph)
    {
        TestNotNull(TEXT("PrintString body node landed in UserConstructionScript"),
            FindNodeOfType<UK2Node_CallFunction>(ConstructionGraph));
    }
    return true;
}

// ============================================================================
// 6. Compiler.Integration.SelectNode
// Compile a select node with cond/true/false; verify a UK2Node_Select exists.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationSelectNodeTest,
    "PinWright.bpir.compiler.integration.SelectNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationSelectNodeTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("MacroEntryTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %val = select(cond: true, true: \"Day\", false: \"Night\")\n")
        TEXT("    call PrintString(InString: %val)\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestTrue(TEXT("Exactly one Select node exists"),
        CountNodesOfType<UK2Node_Select>(BP) == 1);
    return true;
}

// ============================================================================
// 7. Compiler.Integration.ForeachBreak
// Compile a foreach_break loop over an Items array variable.
// The BP must have a String array variable named "Items" added before compile.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationForeachBreakTest,
    "PinWright.bpir.compiler.integration.ForeachBreak",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationForeachBreakTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("MacroEntryTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // Add a String array member variable named "Items" so the foreach_break can reference it
    FEdGraphPinType ArrayType;
    ArrayType.PinCategory = UEdGraphSchema_K2::PC_String;
    ArrayType.ContainerType = EPinContainerType::Array;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("Items"), ArrayType);

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %loop = foreach_break($Items) [body -> @body, completed -> @after]\n")
        TEXT("\n")
        TEXT("@body:\n")
        TEXT("    call PrintString(InString: \"Item\")\n")
        TEXT("\n")
        TEXT("@after:\n")
        TEXT("    call PrintString(InString: \"Done\")\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestTrue(TEXT("At least one MacroInstance node exists (ForEachLoopWithBreak)"),
        CountNodesOfType<UK2Node_MacroInstance>(BP) >= 1);
    return true;
}
