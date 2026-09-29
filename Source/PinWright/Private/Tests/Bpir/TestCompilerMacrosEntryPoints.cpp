// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_Event.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_MultiGate.h"
#include "K2Node_InputKey.h"
#include "K2Node_Select.h"
#include "EdGraph/EdGraph.h"

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

    // MultiGate is the native UK2Node_MultiGate on every UE 5.x (StandardMacros has no such
    // graph), so `macro MultiGate` must compile to that node with both bare-index targets wired.
    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestEqual(TEXT("Exactly one native MultiGate node exists"),
        CountNodesOfType<UK2Node_MultiGate>(BP), 1);
    TestEqual(TEXT("No MacroInstance was created for MultiGate"),
        CountNodesOfType<UK2Node_MacroInstance>(BP), 0);

    UK2Node_MultiGate* Gate = FindNodeOfType<UK2Node_MultiGate>(BP);
    if (!TestNotNull(TEXT("MultiGate node found"), Gate)) return false;
    for (const TCHAR* OutName : { TEXT("Out 0"), TEXT("Out 1") })
    {
        UEdGraphPin* OutPin = Gate->FindPin(OutName, EGPD_Output);
        TestTrue(FString::Printf(TEXT("'%s' is wired to its label"), OutName),
            OutPin && OutPin->LinkedTo.Num() == 1);
    }
    return true;
}

// ============================================================================
// 4b. Compiler.Integration.MacroMultiGateOutputCount
// `outputs: N` grows to max(N, highest wired index + 1); a non-positive count is
// rejected before any node is created.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationMacroMultiGateOutputCountTest,
    "PinWright.bpir.compiler.integration.MacroMultiGateOutputCount",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationMacroMultiGateOutputCountTest::RunTest(const FString& Parameters)
{
    auto CountExecOuts = [](const UEdGraphNode* Node)
    {
        int32 Count = 0;
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            Count += (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) ? 1 : 0;
        }
        return Count;
    };

    // A wired index beyond the declared count wins: outputs: 2 with Out 3 wired -> 4 outputs.
    {
        UBlueprint* BP = CreateTransientTestBP(TEXT("MacroEntryTestBP"));
        if (!TestNotNull(TEXT("Blueprint was created"), BP)) return false;
        FBpirCompiler Compiler(BP);
        FCompileResult Result = Compiler.Compile(
            TEXT("entry event BeginPlay() {\n")
            TEXT("    %mg = macro MultiGate(outputs: 2) [3 -> @last]\n")
            TEXT("\n")
            TEXT("@last:\n")
            TEXT("    call PrintString(InString: \"Out 3\")\n")
            TEXT("}"));
        for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); }
        TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
        UK2Node_MultiGate* Gate = FindNodeOfType<UK2Node_MultiGate>(BP);
        if (!TestNotNull(TEXT("MultiGate node found"), Gate)) return false;
        TestEqual(TEXT("Output count grows to the highest wired index + 1"), CountExecOuts(Gate), 4);
        UEdGraphPin* Out3 = Gate->FindPin(TEXT("Out 3"), EGPD_Output);
        TestTrue(TEXT("'Out 3' is wired"), Out3 && Out3->LinkedTo.Num() == 1);
    }

    // outputs: 0 is not a count; the compile fails and leaves no MultiGate behind.
    {
        UBlueprint* BP = CreateTransientTestBP(TEXT("MacroEntryTestBP"));
        if (!TestNotNull(TEXT("Blueprint was created"), BP)) return false;
        FBpirCompiler Compiler(BP);
        FCompileResult Result = Compiler.Compile(
            TEXT("entry event BeginPlay() {\n")
            TEXT("    %mg = macro MultiGate(outputs: 0)\n")
            TEXT("}"));
        TestFalse(TEXT("Compile fails on outputs: 0"), Result.bSuccess);
        bool bNamesTheArg = false;
        for (const FCompileError& Err : Result.Errors)
        {
            bNamesTheArg |= Err.Message.Contains(TEXT("'outputs' must be a positive integer"));
        }
        TestTrue(TEXT("Error names the outputs argument"), bNamesTheArg);
        TestEqual(TEXT("No MultiGate node is left in the graph"), CountNodesOfType<UK2Node_MultiGate>(BP), 0);
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
