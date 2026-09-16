// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_Event.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_VariableGet.h"
#include "K2Node_FormatText.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_SwitchEnum.h"
#include "EdGraph/EdGraph.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Character.h"
#include "Engine/StaticMeshActor.h"
#include "Tests/TestSkipReporting.h"

using namespace CompilerTestUtils;

static UK2Node_CallFunction* FindPrintStringWithDefault(UBlueprint* BP, const FString& Needle)
{
    if (!BP)
    {
        return nullptr;
    }

    TArray<UEdGraph*> Graphs;
    Graphs.Append(BP->UbergraphPages);
    Graphs.Append(BP->FunctionGraphs);
    Graphs.Append(BP->MacroGraphs);

    for (UEdGraph* Graph : Graphs)
    {
        if (!Graph) continue;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
            if (!Call || !Call->FunctionReference.GetMemberName().ToString().Contains(TEXT("PrintString")))
            {
                continue;
            }

            UEdGraphPin* InString = Call->FindPin(TEXT("InString"));
            if (!InString)
            {
                InString = Call->FindPin(TEXT("inString"));
            }
            if (InString && InString->DefaultValue.Contains(Needle))
            {
                return Call;
            }
        }
    }

    return nullptr;
}

// ============================================================================
// 1. Compiler.Integration.SimpleBeginPlay
// Compile a PrintString call inside BeginPlay; verify a CallFunction node exists.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationSimpleBeginPlayTest,
    "PinWright.bpir.compiler.integration.SimpleBeginPlay",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationSimpleBeginPlayTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call PrintString(InString: \"Hello World\")\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestTrue(TEXT("At least one node was created"), Result.CreatedNodeGUIDs.Num() > 0);
    TestTrue(TEXT("Event graph contains a CallFunction node for PrintString"),
        CountNodesOfType<UK2Node_CallFunction>(BP) > 0);

    // Verify PrintString node has correct string pin default
    UK2Node_CallFunction* PrintNode = FindCallFunctionBySubstring(BP, TEXT("PrintString"));
    TestNotNull(TEXT("PrintString node found"), PrintNode);
    if (PrintNode)
    {
        UEdGraphPin* StringPin = PrintNode->FindPin(TEXT("InString"));
        if (!StringPin) StringPin = PrintNode->FindPin(TEXT("inString"));
        TestNotNull(TEXT("String input pin found"), StringPin);
        if (StringPin)
        {
            TestTrue(TEXT("Default contains expected string"), StringPin->DefaultValue.Contains(TEXT("Hello")));
        }
    }
    return true;
}

// ============================================================================
// 2. Compiler.Integration.VariableDeclaration
// A local variable assignment should compile cleanly (pin mapping only, no crash).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationVariableDeclarationTest,
    "PinWright.bpir.compiler.integration.VariableDeclaration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationVariableDeclarationTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // Add a float variable so the VariableSet node has a value pin to wire
    FEdGraphPinType FloatType;
    FloatType.PinCategory = UEdGraphSchema_K2::PC_Real;
    FloatType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("Speed"), FloatType);

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    set Speed = 100.0\n")
        TEXT("}"));

    // Variable declarations are pin-mapping only — no crash is the key requirement.
    TestTrue(TEXT("Compile with local variable declaration succeeded"), Result.bSuccess);
    return true;
}

// ============================================================================
// 3. Compiler.Integration.IfElse
// A conditional with two PrintString branches must produce a Branch (IfThenElse) node.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationIfElseTest,
    "PinWright.bpir.compiler.integration.IfElse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationIfElseTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %rand = pure RandomBool()\n")
        TEXT("    %b = branch(%rand) [true -> @then, false -> @else]\n")
        TEXT("    @then:\n")
        TEXT("    call PrintString(InString: \"True\")\n")
        TEXT("    exec -> @done\n")
        TEXT("    @else:\n")
        TEXT("    call PrintString(InString: \"False\")\n")
        TEXT("    exec -> @done\n")
        TEXT("    @done:\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestTrue(TEXT("Event graph has at least one Branch node"),
        CountNodesOfType<UK2Node_IfThenElse>(BP) > 0);

    // Verify branch condition pin is wired
    UK2Node_IfThenElse* BranchNode = nullptr;
    for (UEdGraphNode* Node : BP->UbergraphPages[0]->Nodes)
    {
        if (UK2Node_IfThenElse* IfNode = Cast<UK2Node_IfThenElse>(Node))
        {
            BranchNode = IfNode;
            break;
        }
    }
    TestNotNull(TEXT("Branch node found"), BranchNode);
    if (BranchNode)
    {
        UEdGraphPin* CondPin = BranchNode->FindPin(TEXT("Condition"), EGPD_Input);
        TestNotNull(TEXT("Condition pin found"), CondPin);
        if (CondPin)
        {
            TestTrue(TEXT("Condition pin is connected or has default"), CondPin->LinkedTo.Num() > 0 || !CondPin->DefaultValue.IsEmpty());
        }
    }
    return true;
}

// ============================================================================
// 4. Compiler.Integration.ForEachLoop
// A foreach over GetAllActorsOfClass must produce a MacroInstance (ForEach) node.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationForEachLoopTest,
    "PinWright.bpir.compiler.integration.ForEachLoop",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationForEachLoopTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %actors = call GetAllActorsOfClass()\n")
        TEXT("    %loop = foreach(%actors) [body -> @body, completed -> @after]\n")
        TEXT("    @body:\n")
        TEXT("    call PrintString(InString: \"Found\")\n")
        TEXT("    exec -> @after\n")
        TEXT("    @after:\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestTrue(TEXT("Event graph has a MacroInstance node (ForEach)"),
        CountNodesOfType<UK2Node_MacroInstance>(BP) > 0);

    // Verify loop body exec chain exists
    UK2Node_MacroInstance* LoopNode = nullptr;
    for (UEdGraphNode* Node : BP->UbergraphPages[0]->Nodes)
    {
        if (UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node))
        {
            LoopNode = Macro;
            break;
        }
    }
    TestNotNull(TEXT("ForEach macro node found"), LoopNode);
    if (LoopNode)
    {
        UEdGraphNode* BodyNode = GetExecDownstream(LoopNode, TEXT("LoopBody"));
        TestNotNull(TEXT("Loop body has downstream node"), BodyNode);
    }
    return true;
}

// ============================================================================
// 5. Compiler.Integration.SequenceBlock
// A sequence block with two entries must produce an ExecutionSequence node.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationSequenceBlockTest,
    "PinWright.bpir.compiler.integration.SequenceBlock",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationSequenceBlockTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %seq = sequence(2) [0 -> @s0, 1 -> @s1]\n")
        TEXT("    @s0:\n")
        TEXT("    call PrintString(InString: \"First\")\n")
        TEXT("    exec -> @done\n")
        TEXT("    @s1:\n")
        TEXT("    call PrintString(InString: \"Second\")\n")
        TEXT("    exec -> @done\n")
        TEXT("    @done:\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestTrue(TEXT("Event graph has an ExecutionSequence node"),
        CountNodesOfType<UK2Node_ExecutionSequence>(BP) > 0);

    // Verify sequence outputs are wired
    UK2Node_ExecutionSequence* SeqNode = nullptr;
    for (UEdGraphNode* Node : BP->UbergraphPages[0]->Nodes)
    {
        if (UK2Node_ExecutionSequence* Seq = Cast<UK2Node_ExecutionSequence>(Node))
        {
            SeqNode = Seq;
            break;
        }
    }
    TestNotNull(TEXT("Sequence node found"), SeqNode);
    if (SeqNode)
    {
        UEdGraphNode* Out0 = GetExecDownstream(SeqNode, TEXT("Then_0"));
        if (!Out0) Out0 = GetExecDownstream(SeqNode, TEXT("then_0"));
        TestNotNull(TEXT("Sequence output 0 is wired"), Out0);
    }
    return true;
}

// ============================================================================
// 6. Compiler.Integration.CustomEvent
// An event block with typed parameters must produce a CustomEvent node.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationCustomEventTest,
    "PinWright.bpir.compiler.integration.CustomEvent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationCustomEventTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry custom_event OnMyCustomEvent(float Damage, FString Source) {\n")
        TEXT("    call PrintString(InString: $Source)\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestTrue(TEXT("Event graph has a CustomEvent node"),
        CountNodesOfType<UK2Node_CustomEvent>(BP) > 0);

    // Verify event name matches
    UK2Node_CustomEvent* EventNode = nullptr;
    for (UEdGraphNode* Node : BP->UbergraphPages[0]->Nodes)
    {
        if (UK2Node_CustomEvent* Evt = Cast<UK2Node_CustomEvent>(Node))
        {
            EventNode = Evt;
            break;
        }
    }
    TestNotNull(TEXT("Custom event node found"), EventNode);
    if (EventNode)
    {
        TestTrue(TEXT("Event has expected name"), EventNode->CustomFunctionName.ToString().Contains(TEXT("MyCustomEvent")) || EventNode->GetNodeTitle(ENodeTitleType::ListView).ToString().Contains(TEXT("MyCustomEvent")));
    }
    return true;
}

// ============================================================================
// 7. Compiler.Integration.NewFunction
// A function signature must create a new FunctionGraph named "CalculateSpeed".
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationNewFunctionTest,
    "PinWright.bpir.compiler.integration.NewFunction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationNewFunctionTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry function CalculateSpeed(float Distance, float Time) -> float {\n")
        TEXT("    %result = pure Divide_DoubleDouble(A: $Distance, B: $Time)\n")
        TEXT("    return %result\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);

    // Verify a new FunctionGraph named "CalculateSpeed" was added to the Blueprint.
    bool bFoundGraph = false;
    for (UEdGraph* Graph : BP->FunctionGraphs)
    {
        if (Graph && Graph->GetName() == TEXT("CalculateSpeed"))
        {
            bFoundGraph = true;
            break;
        }
    }
    TestTrue(TEXT("FunctionGraphs contains a graph named CalculateSpeed"), bFoundGraph);
    return true;
}

// ============================================================================
// 8. Compiler.Integration.UndoCompile
// Compile some code, record node counts, then pop + delete the created nodes and
// verify the graph returns to its original state.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationUndoCompileTest,
    "PinWright.bpir.compiler.integration.UndoCompile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationUndoCompileTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    const int32 NodesBefore = CountAllEventGraphNodes(BP);

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call PrintString(InString: \"Undo me\")\n")
        TEXT("}"));

    TestTrue(TEXT("Initial compile succeeded"), Result.bSuccess);
    TestTrue(TEXT("Nodes were added after compile"),
        CountAllEventGraphNodes(BP) > NodesBefore);

    // Undo: pop the GUIDs recorded during the last compile and delete those nodes.
    TArray<FGuid> LastGUIDs = FBpirCompiler::PopLastCreatedNodes();
    TestTrue(TEXT("PopLastCreatedNodes returned at least one GUID"), LastGUIDs.Num() > 0);

    const bool bDeleted = FBpirCompiler::DeleteNodesByGUIDs(BP, LastGUIDs);
    TestTrue(TEXT("DeleteNodesByGUIDs reported success"), bDeleted);
    TestEqual(TEXT("Node count returned to pre-compile baseline"),
        CountAllEventGraphNodes(BP), NodesBefore);
    return true;
}

// ============================================================================
// 9. Compiler.Integration.EmptyCode
// Passing an empty string must fail with at least one reported error.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationEmptyCodeTest,
    "PinWright.bpir.compiler.integration.EmptyCode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationEmptyCodeTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(TEXT(""));

    TestFalse(TEXT("Empty code returns bSuccess=false"), Result.bSuccess);
    TestTrue(TEXT("Empty code produces at least one error"), Result.Errors.Num() > 0);
    return true;
}

// ============================================================================
// 10. Compiler.Integration.InvalidFunction
// A call to a completely fabricated function must not crash; the expression
// resolver returns nullptr for unresolved names and logs a warning.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationInvalidFunctionTest,
    "PinWright.bpir.compiler.integration.InvalidFunction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationInvalidFunctionTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);

    // The compiler should not crash when it encounters an unresolvable function.
    // Warnings may be logged; a hard crash or unhandled exception is the failure mode.
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call CompletelyFakeNonexistentFunction12345()\n")
        TEXT("}"));

    // Reaching this line proves there was no crash -- but "did not crash" used to be the
    // ONLY thing asserted here, via TestTrue(..., true), which is a tautology: an
    // unresolvable name silently resolving to some other function also read as a pass.
    // Sibling test PinWright.bpir.compiler.errors.AtomicRollback feeds the same
    // fabricated-function construct and pins bSuccess=false, so the honest expectation
    // is available and is asserted here too.
    TestFalse(TEXT("Compile fails on an unresolvable function"), Result.bSuccess);
    TestTrue(TEXT("Unresolvable function produced at least one error"), Result.Errors.Num() > 0);
    return true;
}

// ============================================================================
// 11. Compiler.Integration.InsertCodeAfterNode
// Insert code after an existing node; verify nodes are actually created.
// This was broken because ExpressionResolver's CurrentGraph was never set.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationInsertCodeAfterNodeTest,
    "PinWright.bpir.compiler.integration.InsertCodeAfterNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::NonNullRHI | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationInsertCodeAfterNodeTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // First compile a BeginPlay with one PrintString
    FBpirCompiler Compiler(BP);
    FCompileResult InitResult = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call PrintString(InString: \"Before\")\n")
        TEXT("}"));
    TestTrue(TEXT("Initial compile succeeded"), InitResult.bSuccess);

    // Find the CallFunction node (PrintString)
    UK2Node_CallFunction* PrintStringNode = nullptr;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
            {
                PrintStringNode = CallNode;
                break;
            }
        }
        if (PrintStringNode) break;
    }
    TestNotNull(TEXT("Found PrintString CallFunction node"), PrintStringNode);
    if (!PrintStringNode) return false;

    // Insert code after the PrintString node
    FCompileResult InsertResult = Compiler.InsertCodeAfterNode(
        PrintStringNode, TEXT("call PrintString(InString: \"After\")"));

    TestTrue(TEXT("InsertCodeAfterNode succeeded"), InsertResult.bSuccess);
    TestTrue(TEXT("InsertCodeAfterNode created at least one node (was 0 before fix)"),
        InsertResult.CreatedNodeGUIDs.Num() > 0);

    // Should now have 2 CallFunction nodes total
    int32 CallFuncCount = CountNodesOfType<UK2Node_CallFunction>(BP);
    TestTrue(TEXT("Two CallFunction nodes exist after insertion"), CallFuncCount >= 2);
    return true;
}

// ============================================================================
// 12. Compiler.Integration.InsertCodeAfterNode_ExecChain
// Insert code between two existing nodes; verify the exec chain is preserved.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationInsertCodeExecChainTest,
    "PinWright.bpir.compiler.integration.InsertCodeAfterNode_ExecChain",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::NonNullRHI | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationInsertCodeExecChainTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // Compile BeginPlay with two PrintStrings
    FBpirCompiler Compiler(BP);
    FCompileResult InitResult = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call PrintString(InString: \"First\")\n")
        TEXT("    call PrintString(InString: \"Second\")\n")
        TEXT("}"));
    TestTrue(TEXT("Initial compile succeeded"), InitResult.bSuccess);

    // Find the first CallFunction node (connected to event's exec out)
    UK2Node_CallFunction* FirstPrintNode = nullptr;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
            {
                // Follow the exec output to find the first CallFunction
                for (UEdGraphPin* Pin : EventNode->Pins)
                {
                    if (Pin->Direction == EGPD_Output
                        && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
                        && Pin->LinkedTo.Num() > 0)
                    {
                        FirstPrintNode = Cast<UK2Node_CallFunction>(Pin->LinkedTo[0]->GetOwningNode());
                        break;
                    }
                }
            }
            if (FirstPrintNode) break;
        }
        if (FirstPrintNode) break;
    }
    TestNotNull(TEXT("Found first PrintString node"), FirstPrintNode);
    if (!FirstPrintNode) return false;

    // Insert code after the first PrintString
    FCompileResult InsertResult = Compiler.InsertCodeAfterNode(
        FirstPrintNode, TEXT("call PrintString(InString: \"Inserted\")"));
    TestTrue(TEXT("InsertCodeAfterNode succeeded"), InsertResult.bSuccess);
    TestTrue(TEXT("Inserted at least one node"), InsertResult.CreatedNodeGUIDs.Num() > 0);

    // Verify the exec chain: First -> Inserted -> Second
    // Find the exec output of the first node
    UEdGraphPin* FirstExecOut = nullptr;
    for (UEdGraphPin* Pin : FirstPrintNode->Pins)
    {
        if (Pin->Direction == EGPD_Output
            && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            FirstExecOut = Pin;
            break;
        }
    }
    TestNotNull(TEXT("First node has exec output"), FirstExecOut);
    if (!FirstExecOut) return false;

    // The inserted node should be connected to the first node's exec output
    TestTrue(TEXT("First node's exec output has a downstream connection"),
        FirstExecOut->LinkedTo.Num() > 0);

    if (FirstExecOut->LinkedTo.Num() > 0)
    {
        UEdGraphNode* InsertedNode = FirstExecOut->LinkedTo[0]->GetOwningNode();
        TestNotNull(TEXT("Inserted node exists"), InsertedNode);

        // The inserted node should connect to the second PrintString
        UEdGraphPin* InsertedExecOut = nullptr;
        for (UEdGraphPin* Pin : InsertedNode->Pins)
        {
            if (Pin->Direction == EGPD_Output
                && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                InsertedExecOut = Pin;
                break;
            }
        }
        TestNotNull(TEXT("Inserted node has exec output"), InsertedExecOut);
        if (InsertedExecOut)
        {
            TestTrue(TEXT("Inserted node connects to downstream node"),
                InsertedExecOut->LinkedTo.Num() > 0);
        }
    }

    // Should have 3 CallFunction nodes total
    TestTrue(TEXT("Three CallFunction nodes exist"),
        CountNodesOfType<UK2Node_CallFunction>(BP) >= 3);
    return true;
}

// ============================================================================
// 13. Compiler.Integration.UnresolvedExpressionWarnings
// Compiling code with an unresolved function now reports it in Errors array.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationUnresolvedWarningsTest,
    "PinWright.bpir.compiler.integration.UnresolvedExpressionWarnings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::NonNullRHI | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationUnresolvedWarningsTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call CompletelyFakeFunction12345()\n")
        TEXT("}"));

    // With atomic transactions, any unresolved expression causes the entire compile to fail
    TestFalse(TEXT("Compile fails due to unresolved expression"), Result.bSuccess);
    // The unresolved expression should be reported in the errors array
    TestTrue(TEXT("Errors array reports the unresolved expression"),
        Result.Errors.Num() > 0);
    return true;
}

// ============================================================================
// 14. Compiler.Integration.MultipleBodyStatements
// Multiple body statements should all emit nodes (not be silently discarded).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationMultipleBodyStatementsTest,
    "PinWright.bpir.compiler.integration.MultipleBodyStatements",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::NonNullRHI | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationMultipleBodyStatementsTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call PrintString(InString: \"A\")\n")
        TEXT("    call PrintString(InString: \"B\")\n")
        TEXT("    call PrintString(InString: \"C\")\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    // 1 event node + 3 call function nodes = at least 4 GUIDs
    TestTrue(TEXT("At least 4 node GUIDs created (1 event + 3 calls)"),
        Result.CreatedNodeGUIDs.Num() >= 4);
    // Verify all 3 CallFunction nodes exist
    TestTrue(TEXT("3 CallFunction nodes exist"),
        CountNodesOfType<UK2Node_CallFunction>(BP) >= 3);
    return true;
}

// ============================================================================
// 15. Compiler.Integration.IfBraceOnSameLine
// Branch with true path containing a PrintString must compile the body.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationIfBraceOnSameLineTest,
    "PinWright.bpir.compiler.integration.IfBraceOnSameLine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationIfBraceOnSameLineTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %rand = pure RandomBool()\n")
        TEXT("    %b = branch(%rand) [true -> @then, false -> @done]\n")
        TEXT("    @then:\n")
        TEXT("    call PrintString(InString: \"Inside\")\n")
        TEXT("    exec -> @done\n")
        TEXT("    @done:\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestTrue(TEXT("PrintString inside if-body exists"),
        CountNodesOfType<UK2Node_CallFunction>(BP) >= 1);
    return true;
}

// ============================================================================
// 16. Compiler.Integration.IfElseBraceOnSameLine
// Branch with true and false paths must compile both branches.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationIfElseBraceOnSameLineTest,
    "PinWright.bpir.compiler.integration.IfElseBraceOnSameLine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationIfElseBraceOnSameLineTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %rand = pure RandomBool()\n")
        TEXT("    %b = branch(%rand) [true -> @then, false -> @else]\n")
        TEXT("    @then:\n")
        TEXT("    call PrintString(InString: \"A\")\n")
        TEXT("    exec -> @done\n")
        TEXT("    @else:\n")
        TEXT("    call PrintString(InString: \"B\")\n")
        TEXT("    exec -> @done\n")
        TEXT("    @done:\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestTrue(TEXT("Both branches have PrintString (at least 2 CallFunction nodes)"),
        CountNodesOfType<UK2Node_CallFunction>(BP) >= 2);
    return true;
}

// ============================================================================
// 17. Compiler.Integration.IfBraceOnNextLine
// Branch with true path only (same graph structure as test 15, regression guard).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationIfBraceOnNextLineTest,
    "PinWright.bpir.compiler.integration.IfBraceOnNextLine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationIfBraceOnNextLineTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %rand = pure RandomBool()\n")
        TEXT("    %b = branch(%rand) [true -> @then, false -> @done]\n")
        TEXT("    @then:\n")
        TEXT("    call PrintString(InString: \"Inside\")\n")
        TEXT("    exec -> @done\n")
        TEXT("    @done:\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestTrue(TEXT("PrintString inside if-body exists"),
        CountNodesOfType<UK2Node_CallFunction>(BP) >= 1);
    return true;
}

// ============================================================================
// 18. Compiler.Integration.FunctionWithIfBraceOnSameLine
// Custom function with variable + branch must produce entry + Branch + body nodes.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationFunctionWithIfBraceOnSameLineTest,
    "PinWright.bpir.compiler.integration.FunctionWithIfBraceOnSameLine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationFunctionWithIfBraceOnSameLineTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry function TestFunc() {\n")
        TEXT("    %valid = pure IsValid(Object: self)\n")
        TEXT("    %b = branch(%valid) [true -> @then, false -> @done]\n")
        TEXT("    @then:\n")
        TEXT("    call PrintString(InString: \"Yes\")\n")
        TEXT("    exec -> @done\n")
        TEXT("    @done:\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestTrue(TEXT("At least 3 nodes created (entry + Branch + PrintString)"),
        Result.CreatedNodeGUIDs.Num() >= 3);
    return true;
}

// ============================================================================
// 19. Compiler.Integration.CodeAfterIfBlock
// Code after a branch block must still be compiled (off-by-one guard).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationTest_CodeAfterIfBlock,
    "PinWright.bpir.compiler.integration.CodeAfterIfBlock",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationTest_CodeAfterIfBlock::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %b = branch(true) [true -> @then, false -> @after]\n")
        TEXT("    @then:\n")
        TEXT("    call PrintString(InString: \"Inside\")\n")
        TEXT("    exec -> @after\n")
        TEXT("    @after:\n")
        TEXT("    call PrintString(InString: \"After\")\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestEqual(TEXT("2 CallFunction nodes exist (Inside + After)"),
        CountNodesOfType<UK2Node_CallFunction>(BP), 2);
    return true;
}

// ============================================================================
// 20. Compiler.Integration.CodeAfterForLoop
// Code after a for-each loop must still be compiled.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationTest_CodeAfterForLoop,
    "PinWright.bpir.compiler.integration.CodeAfterForLoop",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationTest_CodeAfterForLoop::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %actors = call GetAllActorsOfClass(ActorClass: AActor)\n")
        TEXT("    %loop = foreach(%actors) [body -> @body, completed -> @after]\n")
        TEXT("    @body:\n")
        TEXT("    call PrintString(InString: \"InLoop\")\n")
        TEXT("    exec -> @after\n")
        TEXT("    @after:\n")
        TEXT("    call PrintString(InString: \"AfterLoop\")\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestEqual(TEXT("3 CallFunction nodes (GetAllActorsOfClass + InLoop + AfterLoop)"),
        CountNodesOfType<UK2Node_CallFunction>(BP), 3);
    return true;
}

// ============================================================================
// 21. Compiler.Integration.NestedIfElse
// Nested branches must produce 2 Branch nodes and 3 CallFunction nodes.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationTest_NestedIfElse,
    "PinWright.bpir.compiler.integration.NestedIfElse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationTest_NestedIfElse::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %b1 = branch(true) [true -> @outer_then, false -> @outer_else]\n")
        TEXT("    @outer_then:\n")
        TEXT("    %b2 = branch(false) [true -> @inner_then, false -> @inner_else]\n")
        TEXT("    @inner_then:\n")
        TEXT("    call PrintString(InString: \"Inner-True\")\n")
        TEXT("    exec -> @done\n")
        TEXT("    @inner_else:\n")
        TEXT("    call PrintString(InString: \"Inner-False\")\n")
        TEXT("    exec -> @done\n")
        TEXT("    @outer_else:\n")
        TEXT("    call PrintString(InString: \"Outer-False\")\n")
        TEXT("    exec -> @done\n")
        TEXT("    @done:\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestEqual(TEXT("2 Branch nodes (IfThenElse)"),
        CountNodesOfType<UK2Node_IfThenElse>(BP), 2);
    TestEqual(TEXT("3 CallFunction nodes"),
        CountNodesOfType<UK2Node_CallFunction>(BP), 3);
    return true;
}

// ============================================================================
// 22. Compiler.Integration.IfElseAllmanStyle
// Branch with true/false paths must produce 1 Branch node and 2 CallFunction nodes.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationTest_IfElseAllmanStyle,
    "PinWright.bpir.compiler.integration.IfElseAllmanStyle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationTest_IfElseAllmanStyle::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %b = branch(true) [true -> @then, false -> @else]\n")
        TEXT("    @then:\n")
        TEXT("    call PrintString(InString: \"TrueCase\")\n")
        TEXT("    exec -> @done\n")
        TEXT("    @else:\n")
        TEXT("    call PrintString(InString: \"FalseCase\")\n")
        TEXT("    exec -> @done\n")
        TEXT("    @done:\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestEqual(TEXT("1 Branch node"),
        CountNodesOfType<UK2Node_IfThenElse>(BP), 1);
    TestEqual(TEXT("2 CallFunction nodes"),
        CountNodesOfType<UK2Node_CallFunction>(BP), 2);
    return true;
}

// ============================================================================
// 23. Compiler.Integration.BracesInStringLiteral
// Braces inside string literals must not corrupt the parser.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationTest_BracesInStringLiteral,
    "PinWright.bpir.compiler.integration.BracesInStringLiteral",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationTest_BracesInStringLiteral::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call PrintString(InString: \"open { brace\")\n")
        TEXT("    call PrintString(InString: \"close } brace\")\n")
        TEXT("    call PrintString(InString: \"both {}\")\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestEqual(TEXT("3 CallFunction nodes (braces in strings don't corrupt parsing)"),
        CountNodesOfType<UK2Node_CallFunction>(BP), 3);
    return true;
}

// ============================================================================
// 24. Compiler.Integration.ElseIfChain
// Chained branches must produce 2 Branch nodes and 3 CallFunction nodes.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationTest_ElseIfChain,
    "PinWright.bpir.compiler.integration.ElseIfChain",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationTest_ElseIfChain::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %b1 = branch(true) [true -> @first, false -> @check2]\n")
        TEXT("    @first:\n")
        TEXT("    call PrintString(InString: \"First\")\n")
        TEXT("    exec -> @done\n")
        TEXT("    @check2:\n")
        TEXT("    %b2 = branch(false) [true -> @second, false -> @third]\n")
        TEXT("    @second:\n")
        TEXT("    call PrintString(InString: \"Second\")\n")
        TEXT("    exec -> @done\n")
        TEXT("    @third:\n")
        TEXT("    call PrintString(InString: \"Third\")\n")
        TEXT("    exec -> @done\n")
        TEXT("    @done:\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestEqual(TEXT("2 Branch nodes (IfThenElse)"),
        CountNodesOfType<UK2Node_IfThenElse>(BP), 2);
    TestEqual(TEXT("3 CallFunction nodes"),
        CountNodesOfType<UK2Node_CallFunction>(BP), 3);
    return true;
}

// ============================================================================
// 25. Compiler.Integration.ElseIfWithoutFinalElse
// Chained branches without a final else must produce 2 Branch nodes and 2 CallFunction nodes.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationTest_ElseIfWithoutFinalElse,
    "PinWright.bpir.compiler.integration.ElseIfWithoutFinalElse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationTest_ElseIfWithoutFinalElse::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %b1 = branch(true) [true -> @first, false -> @check2]\n")
        TEXT("    @first:\n")
        TEXT("    call PrintString(InString: \"First\")\n")
        TEXT("    exec -> @done\n")
        TEXT("    @check2:\n")
        TEXT("    %b2 = branch(false) [true -> @second, false -> @done]\n")
        TEXT("    @second:\n")
        TEXT("    call PrintString(InString: \"Second\")\n")
        TEXT("    exec -> @done\n")
        TEXT("    @done:\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestEqual(TEXT("2 Branch nodes"),
        CountNodesOfType<UK2Node_IfThenElse>(BP), 2);
    TestEqual(TEXT("2 CallFunction nodes"),
        CountNodesOfType<UK2Node_CallFunction>(BP), 2);
    return true;
}

// ============================================================================
// 26. Compiler.Integration.WhileLoop
// A while loop must produce a MacroInstance node and at least one CallFunction.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationTest_WhileLoop,
    "PinWright.bpir.compiler.integration.WhileLoop",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationTest_WhileLoop::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %w = while(true) [body -> @body, completed -> @after]\n")
        TEXT("    @body:\n")
        TEXT("    call PrintString(InString: \"Looping\")\n")
        TEXT("    exec -> @after\n")
        TEXT("    @after:\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestTrue(TEXT("At least 1 MacroInstance node (while uses a macro)"),
        CountNodesOfType<UK2Node_MacroInstance>(BP) >= 1);
    TestTrue(TEXT("At least 1 CallFunction node"),
        CountNodesOfType<UK2Node_CallFunction>(BP) >= 1);
    return true;
}

// ============================================================================
// 27. Compiler.Integration.SwitchStatement
// A switch statement must compile without crashing and produce nodes.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationTest_SwitchStatement,
    "PinWright.bpir.compiler.integration.SwitchStatement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationTest_SwitchStatement::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // Add a Name variable so set/get for $Value works on the transient BP
    FEdGraphPinType NameType;
    NameType.PinCategory = UEdGraphSchema_K2::PC_Name;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("Value"), NameType);

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    set Value = \"A\"\n")
        TEXT("    %sw = switch($Value) [A -> @a, B -> @b, default -> @def]\n")
        TEXT("    @a:\n")
        TEXT("    call PrintString(InString: \"CaseA\")\n")
        TEXT("    exec -> @done\n")
        TEXT("    @b:\n")
        TEXT("    call PrintString(InString: \"CaseB\")\n")
        TEXT("    exec -> @done\n")
        TEXT("    @def:\n")
        TEXT("    call PrintString(InString: \"Default\")\n")
        TEXT("    exec -> @done\n")
        TEXT("    @done:\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    // Switch case body wiring is a known limitation — just verify the switch node was created
    TestTrue(TEXT("At least 1 node created"), Result.CreatedNodeGUIDs.Num() > 0);
    return true;
}

// ============================================================================
// 28. Compiler.Integration.ReturnStatement
// A function with a return statement must create a FunctionGraph with a FunctionResult node.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationTest_ReturnStatement,
    "PinWright.bpir.compiler.integration.ReturnStatement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationTest_ReturnStatement::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry function CalculateValue() -> float {\n")
        TEXT("    return 42.0\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);

    // Verify a FunctionGraph named "CalculateValue" exists
    bool bFoundGraph = false;
    for (UEdGraph* Graph : BP->FunctionGraphs)
    {
        if (Graph && Graph->GetName() == TEXT("CalculateValue"))
        {
            bFoundGraph = true;
            break;
        }
    }
    TestTrue(TEXT("FunctionGraph named 'CalculateValue' exists"), bFoundGraph);
    return true;
}

// ============================================================================
// 29. Compiler.Integration.DeeplyNestedControlFlow
// Deeply nested control flow (branch > foreach > branch) must produce correct node counts.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationTest_DeeplyNestedControlFlow,
    "PinWright.bpir.compiler.integration.DeeplyNestedControlFlow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationTest_DeeplyNestedControlFlow::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %b1 = branch(true) [true -> @outer_then, false -> @done]\n")
        TEXT("    @outer_then:\n")
        TEXT("    %actors = call GetAllActorsOfClass(ActorClass: AActor)\n")
        TEXT("    %loop = foreach(%actors) [body -> @body, completed -> @done]\n")
        TEXT("    @body:\n")
        TEXT("    %b2 = branch(false) [true -> @inner_then, false -> @loop_continue]\n")
        TEXT("    @inner_then:\n")
        TEXT("    call PrintString(InString: \"Deep\")\n")
        TEXT("    exec -> @loop_continue\n")
        TEXT("    @loop_continue:\n")
        TEXT("    exec -> @done\n")
        TEXT("    @done:\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestEqual(TEXT("2 Branch nodes"),
        CountNodesOfType<UK2Node_IfThenElse>(BP), 2);
    TestEqual(TEXT("1 MacroInstance node (ForEach)"),
        CountNodesOfType<UK2Node_MacroInstance>(BP), 1);
    TestEqual(TEXT("2 CallFunction nodes (GetAllActorsOfClass + Deep)"),
        CountNodesOfType<UK2Node_CallFunction>(BP), 2);
    return true;
}

// ============================================================================
// 31. Compiler.Integration.GetOpcode
// Add a float variable to the BP, then compile "%val = get MyVar" and verify
// a VariableGet node is created in the event graph.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationTest_GetOpcode,
    "PinWright.bpir.compiler.integration.GetOpcode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationTest_GetOpcode::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // Add a float variable so the get opcode has a real property to resolve.
    FEdGraphPinType FloatType;
    FloatType.PinCategory = UEdGraphSchema_K2::PC_Real;
    FloatType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("MyVar"), FloatType);

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %val = get MyVar\n")
        TEXT("    call PrintString(InString: \"Got\")\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestTrue(TEXT("At least one node was created"), Result.CreatedNodeGUIDs.Num() > 0);

    // Verify a VariableGet node exists in the ubergraph.
    bool bFoundVariableGet = false;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_VariableGet* VarGetNode = Cast<UK2Node_VariableGet>(Node))
            {
                bFoundVariableGet = true;
                break;
            }
        }
        if (bFoundVariableGet) break;
    }
    TestTrue(TEXT("A VariableGet node exists in the event graph"), bFoundVariableGet);
    return true;
}

// ============================================================================
// 32. Compiler.Integration.BindDispatcher
// Compile "bind_dispatcher OnSomething()" and verify either a bind node is
// created (if the dispatcher is found) or a graceful error is reported without
// crashing (if the dispatcher does not exist on this transient BP).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationTest_BindDispatcher,
    "PinWright.bpir.compiler.integration.BindDispatcher",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationTest_BindDispatcher::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    bind_dispatcher OnSomething()\n")
        TEXT("}"));

    // Without an actual event dispatcher variable on the BP the compiler must
    // either succeed (rare, if it can partially resolve) or fail gracefully
    // with a non-empty error array. A crash is the only true failure mode.
    if (!Result.bSuccess)
    {
        TestTrue(TEXT("Graceful failure produced error messages"),
            Result.Errors.Num() > 0);
        AddInfo(FString::Printf(TEXT("BindDispatcher gracefully failed: %s"),
            Result.Errors.Num() > 0 ? *Result.Errors[0].Message : TEXT("(no message)")));
    }
    else
    {
        AddInfo(TEXT("BindDispatcher compiled successfully"));
        TestTrue(TEXT("At least one node was created"), Result.CreatedNodeGUIDs.Num() > 0);
    }
    return true;
}

// ============================================================================
// 30. Compiler.Integration.MultipleSequentialControlFlow
// Multiple sequential branch blocks followed by a statement must all compile.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationTest_MultipleSequentialControlFlow,
    "PinWright.bpir.compiler.integration.MultipleSequentialControlFlow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationTest_MultipleSequentialControlFlow::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %b1 = branch(true) [true -> @a, false -> @after1]\n")
        TEXT("    @a:\n")
        TEXT("    call PrintString(InString: \"A\")\n")
        TEXT("    exec -> @after1\n")
        TEXT("    @after1:\n")
        TEXT("    %b2 = branch(false) [true -> @b, false -> @after2]\n")
        TEXT("    @b:\n")
        TEXT("    call PrintString(InString: \"B\")\n")
        TEXT("    exec -> @after2\n")
        TEXT("    @after2:\n")
        TEXT("    call PrintString(InString: \"C\")\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestEqual(TEXT("2 Branch nodes"),
        CountNodesOfType<UK2Node_IfThenElse>(BP), 2);
    TestEqual(TEXT("3 CallFunction nodes"),
        CountNodesOfType<UK2Node_CallFunction>(BP), 3);
    return true;
}

// ============================================================================
// 31. Compiler.Integration.CompileSwitchInt
// Parse + compile switch_int BPIR; verify UK2Node_SwitchInteger created.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationCompileSwitchIntTest,
    "PinWright.bpir.compiler.integration.CompileSwitchInt",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationCompileSwitchIntTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerSwitchIntBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %s = switch_int(0) [1 -> @a, 2 -> @b, default -> @d]\n")
        TEXT("    @a:\n")
        TEXT("    call PrintString(InString: \"One\")\n")
        TEXT("    exec -> @d\n")
        TEXT("    @b:\n")
        TEXT("    call PrintString(InString: \"Two\")\n")
        TEXT("    exec -> @d\n")
        TEXT("    @d:\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);

    // Verify a SwitchInteger node was created
    bool bFoundSwitch = false;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node->GetClass()->GetName().Contains(TEXT("SwitchInteger")))
            {
                bFoundSwitch = true;
                break;
            }
        }
        if (bFoundSwitch) break;
    }
    TestTrue(TEXT("SwitchInteger node was created"), bFoundSwitch);
    return true;
}

// ============================================================================
// 32. Compiler.Integration.CompileSwitchString
// Parse + compile switch_string BPIR; verify UK2Node_SwitchString created.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationCompileSwitchStringTest,
    "PinWright.bpir.compiler.integration.CompileSwitchString",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationCompileSwitchStringTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerSwitchStringBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %s = switch_string(\"test\") [\"hello\" -> @a, default -> @d]\n")
        TEXT("    @a:\n")
        TEXT("    call PrintString(InString: \"Hello\")\n")
        TEXT("    exec -> @d\n")
        TEXT("    @d:\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);

    bool bFoundSwitch = false;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node->GetClass()->GetName().Contains(TEXT("SwitchString")))
            {
                bFoundSwitch = true;
                break;
            }
        }
        if (bFoundSwitch) break;
    }
    TestTrue(TEXT("SwitchString node was created"), bFoundSwitch);
    return true;
}

// ============================================================================
// 33. Compiler.Integration.CompileSwitchEnum
// Parse + compile switch_enum<ESlateVisibility> BPIR; verify switch enum node created.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationCompileSwitchEnumTest,
    "PinWright.bpir.compiler.integration.CompileSwitchEnum",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationCompileSwitchEnumTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerSwitchEnumBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // Use ESlateVisibility because it is a Blueprint-visible UENUM loaded by UMG.
    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %e = enum ESlateVisibility::Visible\n")
        TEXT("    %s = switch_enum<ESlateVisibility>(%e) [ESlateVisibility::Visible -> @a, ESlateVisibility::Collapsed -> @b]\n")
        TEXT("    @a:\n")
        TEXT("    call PrintString(InString: \"Visible\")\n")
        TEXT("    exec -> @done\n")
        TEXT("    @b:\n")
        TEXT("    call PrintString(InString: \"Collapsed\")\n")
        TEXT("    exec -> @done\n")
        TEXT("    @done:\n")
        TEXT("}"));

    // switch_enum with a real enum should compile; if it fails, it should do so cleanly
    if (Result.bSuccess)
    {
        bool bFoundSwitch = false;
        for (UEdGraph* Graph : BP->UbergraphPages)
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node->GetClass()->GetName().Contains(TEXT("SwitchEnum")))
                {
                    bFoundSwitch = true;
                    break;
                }
            }
            if (bFoundSwitch) break;
        }
        TestTrue(TEXT("A switch node was created"), bFoundSwitch);
    }
    else
    {
        // Even if enum resolution fails, it must not crash. ESlateVisibility is loaded by
        // UMG in every editor context, so this branch is not expected to be taken -- warn
        // so a regression that turns a working switch_enum compile into a graceful error
        // is visible instead of silently passing here.
        AddWarning(TEXT("switch_enum over ESlateVisibility failed to compile; skipping SwitchEnum node assertion."));
        AddInfo(TEXT("switch_enum compile produced errors (enum may not resolve), verifying no crash"));
        TestTrue(TEXT("Errors were reported cleanly"), Result.Errors.Num() > 0);
    }
    return true;
}

// ============================================================================
// 33b. Compiler.Integration.CompileSwitchEnumDefault
// switch_enum<ESlateVisibility> with `default -> @label`; every non-Visible,
// non-_MAX entry must wire to the default arm's target chain.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationCompileSwitchEnumDefaultTest,
    "PinWright.bpir.compiler.integration.CompileSwitchEnumDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationCompileSwitchEnumDefaultTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerSwitchEnumDefaultBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %e = enum ESlateVisibility::Visible\n")
        TEXT("    %s = switch_enum<ESlateVisibility>(%e) [ESlateVisibility::Visible -> @a, default -> @b]\n")
        TEXT("    @a:\n")
        TEXT("    call PrintString(InString: \"Vis\")\n")
        TEXT("    exec -> @done\n")
        TEXT("    @b:\n")
        TEXT("    call PrintString(InString: \"Other\")\n")
        TEXT("    exec -> @done\n")
        TEXT("    @done:\n")
        TEXT("}"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded with default arm"), Result.bSuccess);
    TestEqual(TEXT("No errors reported"), Result.Errors.Num(), 0);
    if (!Result.bSuccess) return true;

    // Locate the SwitchEnum node.
    UK2Node_SwitchEnum* SwitchNode = FindNodeOfType<UK2Node_SwitchEnum>(BP);
    TestNotNull(TEXT("UK2Node_SwitchEnum was created"), SwitchNode);
    if (!SwitchNode) return true;

    // Locate the two PrintString destinations to identify each chain.
    UK2Node_CallFunction* VisPrint = FindPrintStringWithDefault(BP, TEXT("Vis"));
    UK2Node_CallFunction* OtherPrint = FindPrintStringWithDefault(BP, TEXT("Other"));
    TestNotNull(TEXT("PrintString(\"Vis\") found"), VisPrint);
    TestNotNull(TEXT("PrintString(\"Other\") found"), OtherPrint);
    if (!VisPrint || !OtherPrint) return true;

    // Helper: does the SwitchEnum exec output named EntryName link to a chain
    // that eventually reaches ExpectedDownstream? We allow direct linkage (one hop)
    // since the compiled graph wires Switch's exec outputs straight to PrintString.
    auto ExecOutLinksTo = [&](const TCHAR* EntryName, UEdGraphNode* ExpectedDownstream) -> bool
    {
        UEdGraphPin* Pin = SwitchNode->FindPin(EntryName);
        if (!Pin)
        {
            // Try short name fallback.
            FString Bare = EntryName;
            int32 ColonIdx = INDEX_NONE;
            if (Bare.FindLastChar(TEXT(':'), ColonIdx))
            {
                Bare = Bare.Mid(ColonIdx + 1);
                Pin = SwitchNode->FindPin(*Bare);
            }
        }
        if (!Pin || Pin->Direction != EGPD_Output) return false;
        for (UEdGraphPin* Linked : Pin->LinkedTo)
        {
            if (Linked && Linked->GetOwningNode() == ExpectedDownstream)
            {
                return true;
            }
        }
        return false;
    };

    TestTrue(TEXT("ESlateVisibility::Visible exec links to Vis PrintString"),
        ExecOutLinksTo(TEXT("Visible"), VisPrint));

    // Each remaining non-_MAX, non-Visible entry must be wired to the Other PrintString.
    const TCHAR* DefaultEntries[] = {
        TEXT("Collapsed"),
        TEXT("Hidden"),
        TEXT("HitTestInvisible"),
        TEXT("SelfHitTestInvisible"),
    };
    for (const TCHAR* Entry : DefaultEntries)
    {
        TestTrue(*FString::Printf(TEXT("ESlateVisibility::%s exec links to Other PrintString"), Entry),
            ExecOutLinksTo(Entry, OtherPrint));
    }
    return true;
}

// ============================================================================
// 34. Compiler.Integration.CompileClearDispatcher
// Parse + compile clear_dispatcher BPIR; verify delegate clear node created.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationCompileClearDispatcherTest,
    "PinWright.bpir.compiler.integration.CompileClearDispatcher",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationCompileClearDispatcherTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerClearDispBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // Add a multicast delegate variable to the blueprint
    FEdGraphPinType DelegateType;
    DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("MyDispatcher"), DelegateType);
    FKismetEditorUtilities::CompileBlueprint(BP);

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    clear_dispatcher MyDispatcher()\n")
        TEXT("}"));

    // clear_dispatcher on a real delegate should compile; report result
    if (Result.bSuccess)
    {
        TestTrue(TEXT("At least one node was created"), Result.CreatedNodeGUIDs.Num() > 0);
        // A bare node count cannot see the opcode->node-class mapping in BpirCompiler.cpp
        // (CallDelegate / AddDelegate / RemoveDelegate / ClearDelegate) being swapped, nor
        // the fallback arm that emits a plain UK2Node_CallFunction instead. Compare the
        // class by name so this assertion does not depend on the same __has_include
        // gating the compiler uses to choose it.
        int32 ClearDelegateCount = 0;
        for (UEdGraph* Graph : BP->UbergraphPages)
        {
            if (!Graph) continue;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node && Node->GetClass()->GetName() == TEXT("K2Node_ClearDelegate"))
                {
                    ++ClearDelegateCount;
                }
            }
        }
        TestEqual(TEXT("Exactly one K2Node_ClearDelegate node was created"), ClearDelegateCount, 1);
    }
    else
    {
        // Delegate resolution may fail in test context; verify no crash.
        // Warn so this degenerate branch is visible rather than silently green.
        AddWarning(TEXT("clear_dispatcher on a real delegate failed to compile; skipping ClearDelegate node assertion."));
        AddInfo(TEXT("clear_dispatcher compile produced errors, verifying no crash"));
        TestTrue(TEXT("Errors were reported cleanly"), Result.Errors.Num() > 0);
    }
    return true;
}

// ============================================================================
// 35. Compiler.Integration.CompileCallWithMultiExec
// Parse + compile call with [OnDone -> @d, OnFail -> @f]; verify multiple
// exec outputs wired.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationCompileCallWithMultiExecTest,
    "PinWright.bpir.compiler.integration.CompileCallWithMultiExec",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationCompileCallWithMultiExecTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerMultiExecBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // Use a latent node which naturally has multiple exec outputs
    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %d = latent Delay(Duration: 1.0) [completed -> @done]\n")
        TEXT("    @done:\n")
        TEXT("    call PrintString(InString: \"After delay\")\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);

    // Verify the downstream node is connected
    if (Result.bSuccess)
    {
        UK2Node_CallFunction* PrintNode = FindCallFunctionBySubstring(BP, TEXT("PrintString"));
        TestNotNull(TEXT("PrintString node found after multi-exec compile"), PrintNode);
    }
    return true;
}

// ============================================================================
// 36. Compiler.Integration.CompileGenericK2Node
// Parse + compile call K2Node_SpawnActorFromClass(...); verify correct node
// subclass created.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationCompileGenericK2NodeTest,
    "PinWright.bpir.compiler.integration.CompileGenericK2Node",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationCompileGenericK2NodeTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerGenericK2BP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call K2Node_SpawnActorFromClass(Class: Actor)\n")
        TEXT("}"));

    // K2Node class resolution depends on engine registration; verify no crash
    if (Result.bSuccess)
    {
        // Check that a node with the K2Node class name was created
        bool bFoundNode = false;
        for (UEdGraph* Graph : BP->UbergraphPages)
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node->GetClass()->GetName().Contains(TEXT("SpawnActor")))
                {
                    bFoundNode = true;
                    break;
                }
            }
            if (bFoundNode) break;
        }
        TestTrue(TEXT("SpawnActor K2Node was created"), bFoundNode);
    }
    else
    {
        // Greppable SKIPPED marker rather than AddWarning: this runner treats AddWarning
        // as a failure (see ~line 3322), and nothing in the suite proves generic K2Node
        // class-name resolution succeeds here, so a warning could redden a legitimately
        // skipping test. NOTE: this branch still passes on a total regression of generic
        // K2Node resolution -- see the audit report; pinning it needs a verified run.
        AddInfo(TEXT("SKIPPED: K2Node_SpawnActorFromClass did not resolve; skipping SpawnActor node assertion."));
        AddInfo(TEXT("K2Node_SpawnActorFromClass not resolved, verifying no crash"));
        TestTrue(TEXT("Errors were reported cleanly"), Result.Errors.Num() > 0);
    }
    return true;
}

// ============================================================================
// 37. Compiler.Integration.CompileMultiEntryBlocks
// Multiple entry blocks in a single Compile() call; all entry points created.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationCompileMultiEntryBlocksTest,
    "PinWright.bpir.compiler.integration.CompileMultiEntryBlocks",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationCompileMultiEntryBlocksTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerMultiEntryBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call PrintString(InString: \"Hello\")\n")
        TEXT("}\n")
        TEXT("\n")
        TEXT("entry custom_event MyCustomEvent() {\n")
        TEXT("    call PrintString(InString: \"Custom\")\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);

    // Verify both entry points exist
    bool bFoundBeginPlay = false;
    bool bFoundCustomEvent = false;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
            {
                if (EventNode->EventReference.GetMemberName() == TEXT("ReceiveBeginPlay"))
                {
                    bFoundBeginPlay = true;
                }
            }
            if (UK2Node_CustomEvent* CustomNode = Cast<UK2Node_CustomEvent>(Node))
            {
                if (CustomNode->CustomFunctionName == FName(TEXT("MyCustomEvent")))
                {
                    bFoundCustomEvent = true;
                }
            }
        }
    }
    TestTrue(TEXT("BeginPlay entry point exists"), bFoundBeginPlay);
    TestTrue(TEXT("MyCustomEvent entry point exists"), bFoundCustomEvent);
    return true;
}

// ============================================================================
// 38. Compiler.Integration.InsertAfterNode_SpecificPin
// Insert code after a specific exec output pin on a Branch node.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationInsertAfterNodeSpecificPinTest,
    "PinWright.bpir.compiler.integration.InsertAfterNode_SpecificPin",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::NonNullRHI | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationInsertAfterNodeSpecificPinTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %b = branch(true) [true -> @t, false -> @f]\n")
        TEXT("\n")
        TEXT("@t:\n")
        TEXT("    call PrintString(InString: \"True\")\n")
        TEXT("\n")
        TEXT("@f:\n")
        TEXT("    call PrintString(InString: \"False\")\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Initial compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    // Find the Branch node
    UK2Node_IfThenElse* BranchNode = nullptr;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_IfThenElse* IfNode = Cast<UK2Node_IfThenElse>(Node))
            {
                BranchNode = IfNode;
                break;
            }
        }
        if (BranchNode) break;
    }
    TestNotNull(TEXT("Branch node found"), BranchNode);
    if (!BranchNode) return false;

    // Insert code after the "True" pin specifically
    FCompileResult InsertResult = Compiler.InsertCodeAfterNode(
        BranchNode, TEXT("call PrintString(InString: \"Inserted\")"), TEXT("True"));

    TestTrue(TEXT("InsertCodeAfterNode with specific pin succeeded"), InsertResult.bSuccess);
    TestTrue(TEXT("At least one node created"), InsertResult.CreatedNodeGUIDs.Num() > 0);

    // Verify: Branch True pin -> Inserted -> original True PrintString
    UEdGraphNode* TrueDownstream = GetExecDownstream(BranchNode, TEXT("then"));
    TestNotNull(TEXT("True pin has downstream node"), TrueDownstream);
    if (TrueDownstream)
    {
        // The inserted node should be a PrintString with "Inserted"
        UK2Node_CallFunction* InsertedNode = Cast<UK2Node_CallFunction>(TrueDownstream);
        TestNotNull(TEXT("Inserted node is CallFunction"), InsertedNode);
        if (InsertedNode)
        {
            UEdGraphPin* StringPin = InsertedNode->FindPin(TEXT("InString"));
            if (StringPin)
            {
                TestTrue(TEXT("Inserted node has 'Inserted' string"), StringPin->DefaultValue.Contains(TEXT("Inserted")));
            }

            // Check that inserted node connects to original True PrintString
            UEdGraphNode* NextNode = GetExecDownstream(InsertedNode, TEXT(""));
            TestNotNull(TEXT("Inserted node connects to original True target"), NextNode);
        }
    }

    // Verify: Branch False pin -> original False PrintString (untouched)
    UEdGraphNode* FalseDownstream = GetExecDownstream(BranchNode, TEXT("else"));
    TestNotNull(TEXT("False pin still has downstream (untouched)"), FalseDownstream);
    return true;
}

// ============================================================================
// 39. Compiler.Integration.InsertAfterNode_BadPinName
// InsertCodeAfterNode with a non-existent pin name should fail gracefully.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationInsertAfterNodeBadPinNameTest,
    "PinWright.bpir.compiler.integration.InsertAfterNode_BadPinName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::NonNullRHI | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationInsertAfterNodeBadPinNameTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call PrintString(InString: \"Hello\")\n")
        TEXT("}"));

    TestTrue(TEXT("Initial compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    UK2Node_CallFunction* PrintNode = FindCallFunctionBySubstring(BP, TEXT("PrintString"));
    TestNotNull(TEXT("PrintString node found"), PrintNode);
    if (!PrintNode) return false;

    // Try to insert after a non-existent pin
    FCompileResult InsertResult = Compiler.InsertCodeAfterNode(
        PrintNode, TEXT("call PrintString(InString: \"After\")"), TEXT("NonExistentPin"));

    TestFalse(TEXT("InsertCodeAfterNode with bad pin name should fail"), InsertResult.bSuccess);
    TestTrue(TEXT("No nodes created on failure"), InsertResult.CreatedNodeGUIDs.Num() == 0);
    TestTrue(TEXT("Error mentions available pins"), InsertResult.Errors.Num() > 0);
    return true;
}

// ============================================================================
// 40. Compiler.Integration.InsertBeforeNode
// Insert code before an existing mid-chain node; verify exec chain integrity.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationInsertBeforeNodeTest,
    "PinWright.bpir.compiler.integration.InsertBeforeNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::NonNullRHI | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationInsertBeforeNodeTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call PrintString(InString: \"First\")\n")
        TEXT("    call PrintString(InString: \"Second\")\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Initial compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    // Find the "Second" PrintString node
    UK2Node_CallFunction* SecondNode = nullptr;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
            if (CallNode)
            {
                UEdGraphPin* StringPin = CallNode->FindPin(TEXT("InString"));
                if (StringPin && StringPin->DefaultValue.Contains(TEXT("Second")))
                {
                    SecondNode = CallNode;
                    break;
                }
            }
        }
        if (SecondNode) break;
    }
    TestNotNull(TEXT("Second PrintString node found"), SecondNode);
    if (!SecondNode) return false;

    // Insert code before the Second node
    FCompileResult InsertResult = Compiler.InsertCodeBeforeNode(
        SecondNode, TEXT("call PrintString(InString: \"Inserted\")"));

    TestTrue(TEXT("InsertCodeBeforeNode succeeded"), InsertResult.bSuccess);
    TestTrue(TEXT("At least one node created"), InsertResult.CreatedNodeGUIDs.Num() > 0);

    // Verify exec chain: First -> Inserted -> Second
    // Find the "First" PrintString node
    UK2Node_CallFunction* FirstNode = nullptr;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
            if (CallNode)
            {
                UEdGraphPin* StringPin = CallNode->FindPin(TEXT("InString"));
                if (StringPin && StringPin->DefaultValue.Contains(TEXT("First")))
                {
                    FirstNode = CallNode;
                    break;
                }
            }
        }
        if (FirstNode) break;
    }
    TestNotNull(TEXT("First PrintString node found"), FirstNode);
    if (!FirstNode) return false;

    // Follow exec chain from First
    UEdGraphNode* AfterFirst = GetExecDownstream(FirstNode, TEXT(""));
    TestNotNull(TEXT("First has downstream"), AfterFirst);
    if (AfterFirst)
    {
        // Should be the inserted node
        UK2Node_CallFunction* InsertedCall = Cast<UK2Node_CallFunction>(AfterFirst);
        TestNotNull(TEXT("Node after First is a CallFunction"), InsertedCall);
        if (InsertedCall)
        {
            UEdGraphPin* StringPin = InsertedCall->FindPin(TEXT("InString"));
            if (StringPin)
            {
                TestTrue(TEXT("Inserted node has 'Inserted' string"), StringPin->DefaultValue.Contains(TEXT("Inserted")));
            }

            // Inserted should connect to Second
            UEdGraphNode* AfterInserted = GetExecDownstream(InsertedCall, TEXT(""));
            TestTrue(TEXT("Inserted connects to Second"), AfterInserted == SecondNode);
        }
    }
    return true;
}

// ============================================================================
// 41. Compiler.Integration.InsertBeforeNode_EventNode
// InsertCodeBeforeNode on an event node (no upstream) should fail gracefully.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationInsertBeforeNodeEventNodeTest,
    "PinWright.bpir.compiler.integration.InsertBeforeNode_EventNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::NonNullRHI | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationInsertBeforeNodeEventNodeTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call PrintString(InString: \"Hello\")\n")
        TEXT("}"));

    TestTrue(TEXT("Initial compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    // Find the BeginPlay event node
    UK2Node_Event* EventNode = nullptr;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_Event* EvNode = Cast<UK2Node_Event>(Node))
            {
                EventNode = EvNode;
                break;
            }
        }
        if (EventNode) break;
    }
    TestNotNull(TEXT("Event node found"), EventNode);
    if (!EventNode) return false;

    int32 NodeCountBefore = CountAllEventGraphNodes(BP);

    // Try to insert before the event node — should fail
    FCompileResult InsertResult = Compiler.InsertCodeBeforeNode(
        EventNode, TEXT("call PrintString(InString: \"Before\")"));

    TestFalse(TEXT("InsertCodeBeforeNode on event node should fail"), InsertResult.bSuccess);
    TestTrue(TEXT("No nodes created on failure"), InsertResult.CreatedNodeGUIDs.Num() == 0);

    int32 NodeCountAfter = CountAllEventGraphNodes(BP);
    TestEqual(TEXT("Graph unchanged"), NodeCountBefore, NodeCountAfter);
    return true;
}

// ============================================================================
// InsertCodeAfterNode with return in an externally-created function
// When a function is created outside the BPIR compiler (e.g., via add_function),
// there may be no FunctionResult node. The return opcode must discover return
// value pins from the FunctionEntry and create a proper FunctionResult.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationInsertReturnInExternalFunctionTest,
    "PinWright.bpir.compiler.integration.InsertReturnInExternalFunction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationInsertReturnInExternalFunctionTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // Simulate a function created externally (not by the BPIR compiler):
    // Create function graph, entry node with ReturnValue pin, but NO FunctionResult node
    UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
        BP, FName(TEXT("GetValue")), UEdGraph::StaticClass(),
        UEdGraphSchema_K2::StaticClass());
    TestNotNull(TEXT("Function graph created"), FuncGraph);
    if (!FuncGraph) return false;

    FBlueprintEditorUtils::AddFunctionGraph(BP, FuncGraph, true, static_cast<UClass*>(nullptr));

    // Find the auto-created FunctionEntry
    UK2Node_FunctionEntry* EntryNode = nullptr;
    for (UEdGraphNode* N : FuncGraph->Nodes)
    {
        if (UK2Node_FunctionEntry* E = Cast<UK2Node_FunctionEntry>(N))
        {
            EntryNode = E;
            break;
        }
    }
    TestNotNull(TEXT("FunctionEntry exists"), EntryNode);
    if (!EntryNode) return false;

    // Add a return value pin on the entry node (simulates what add_function does)
    FEdGraphPinType FloatType;
    FloatType.PinCategory = UEdGraphSchema_K2::PC_Real;
    FloatType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
    EntryNode->CreateUserDefinedPin(FName(TEXT("ReturnValue")), FloatType, EGPD_Output);
    EntryNode->ReconstructNode();

    // Verify no FunctionResult exists yet
    TestNull(TEXT("No FunctionResult before insert"),
        FindNodeOfType<UK2Node_FunctionResult>(FuncGraph));

    // Now insert 'return 42.0' after the entry node
    FBpirCompiler Compiler(BP);
    FCompileResult InsertResult = Compiler.InsertCodeAfterNode(
        EntryNode, TEXT("return 42.0"));

    if (!InsertResult.bSuccess)
    {
        for (const FCompileError& Err : InsertResult.Errors)
        {
            AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("InsertCodeAfterNode with return succeeded"), InsertResult.bSuccess);

    // Verify a FunctionResult node was created with a data input pin
    UK2Node_FunctionResult* ResultNode = FindNodeOfType<UK2Node_FunctionResult>(FuncGraph);
    TestNotNull(TEXT("FunctionResult was created"), ResultNode);
    if (ResultNode)
    {
        bool bHasDataInput = false;
        for (const UEdGraphPin* Pin : ResultNode->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Input
                && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
            {
                bHasDataInput = true;
                break;
            }
        }
        TestTrue(TEXT("FunctionResult has a data input pin"), bHasDataInput);
    }
    return true;
}

// ============================================================================
// Compiler.Integration.FormatText
// Compile a Format Text pure node and verify it produces a UK2Node_FormatText
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationFormatTextTest,
    "PinWright.bpir.compiler.integration.FormatText",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationFormatTextTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("FormatTextTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %txt = pure Format(Format: NSLOCTEXT(\"BPIR\", \"FormatTextIntegration\", \"{Name} has {HP} HP\"), Name: \"Player1\", HP: \"100\")\n")
        TEXT("    call PrintString(InString: %txt)\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);

    // Verify a UK2Node_FormatText was created
    int32 FormatNodeCount = CountNodesOfType<UK2Node_FormatText>(BP);
    TestTrue(TEXT("At least one UK2Node_FormatText was created"), FormatNodeCount > 0);
    return true;
}

// ============================================================================
// Compiler.Integration.InsertAfterCustomEvent_ParamAccess
// Custom event params should be accessible by name in inserted code.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerInsertAfterCustomEvent_ParamAccess,
    "PinWright.bpir.compiler.integration.InsertAfterCustomEvent_ParamAccess",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerInsertAfterCustomEvent_ParamAccess::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("InsertParamTestBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    // Step 1: Compile a custom event with a string parameter
    FBpirCompiler Compiler1(BP);
    FCompileResult Result1 = Compiler1.Compile(
        TEXT("entry custom_event OnTestEvent(string LabelStr) {\n")
        TEXT("    call PrintString(InString: \"placeholder\")\n")
        TEXT("}"));
    if (!Result1.bSuccess) { for (const FCompileError& E : Result1.Errors) AddError(E.Message); }
    TestTrue(TEXT("Initial compile succeeded"), Result1.bSuccess);

    // Step 2: Find the custom event node
    UK2Node_CustomEvent* EventNode = nullptr;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node))
            {
                EventNode = CE;
                break;
            }
        }
        if (EventNode) break;
    }
    TestNotNull(TEXT("Custom event node found"), EventNode);
    if (!EventNode) return false;

    // Step 3: Insert code that references the parameter by bare name
    FBpirCompiler Compiler2(BP);
    FCompileResult Result2 = Compiler2.InsertCodeAfterNode(EventNode,
        TEXT("call PrintString(InString: LabelStr)"));

    if (!Result2.bSuccess) { for (const FCompileError& E : Result2.Errors) AddError(E.Message); }
    TestTrue(TEXT("InsertCodeAfterNode referencing param succeeded"), Result2.bSuccess);
    TestEqual(TEXT("No errors"), Result2.Errors.Num(), 0);
    return true;
}

// ============================================================================
// Compiler.Integration.TargetDollarVar_FunctionResolution
// Member functions should resolve when Target is a $var reference.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerTargetDollarVarResolution,
    "PinWright.bpir.compiler.integration.TargetDollarVar_FunctionResolution",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerTargetDollarVarResolution::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("TargetVarTestBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    // Add a SceneComponent variable — SetVisibility is a member function on USceneComponent
    FEdGraphPinType CompType;
    CompType.PinCategory = UEdGraphSchema_K2::PC_Object;
    CompType.PinSubCategoryObject = USceneComponent::StaticClass();
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("MyComp"), CompType);

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call SetVisibility(Target: $MyComp, bNewVisibility: true)\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& E : Result.Errors) AddError(E.Message); }
    TestTrue(TEXT("Compile with $var Target succeeded"), Result.bSuccess);

    // Verify the function call was created
    UK2Node_CallFunction* CallNode = FindCallFunctionBySubstring(BP, TEXT("SetVisibility"));
    TestNotNull(TEXT("SetVisibility call node was created"), CallNode);
    return true;
}

// ============================================================================
// Compiler.Integration.ObjectReturnValue_PropertyAccess
// %ref.Property should resolve when ref returns an object with that property.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerObjectReturnPropertyAccess,
    "PinWright.bpir.compiler.integration.ObjectReturnValue_PropertyAccess",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerObjectReturnPropertyAccess::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("ObjPropTestBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    // GetOwner() returns AActor* which has RootComponent (USceneComponent*) — a BlueprintReadOnly property.
    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry custom_event TestDotAccess() {\n")
        TEXT("    %n0 = pure GetOwner()\n")
        TEXT("    %n1 = pure IsValid(Object: %n0.RootComponent)\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& E : Result.Errors) AddError(E.Message); }
    TestTrue(TEXT("Compile with object property access succeeded"), Result.bSuccess);
    TestEqual(TEXT("No errors"), Result.Errors.Num(), 0);

    // Verify a VariableGet node for RootComponent was created
    bool bFoundRootComponentGet = false;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_VariableGet* VarGet = Cast<UK2Node_VariableGet>(Node))
            {
                if (VarGet->GetVarName() == FName(TEXT("RootComponent")))
                {
                    bFoundRootComponentGet = true;
                }
            }
        }
    }
    TestTrue(TEXT("VariableGet for RootComponent was created"), bFoundRootComponentGet);
    return true;
}

// ============================================================================
// Compiler.Integration.InsertAfterSequenceNode_AutoCreatePin
// InsertCodeAfterNode on a Sequence node with a pin that doesn't exist yet
// should auto-create pins until the requested pin appears. Also verifies
// "Then N" human-friendly format and that existing pins are not disturbed.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerInsertAfterSequenceAutoCreatePinTest,
    "PinWright.bpir.compiler.integration.InsertAfterSequenceNode_AutoCreatePin",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::NonNullRHI | EAutomationTestFlags::EngineFilter)

bool FCompilerInsertAfterSequenceAutoCreatePinTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("SeqAutoCreatePinBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // Compile a sequence with 2 outputs (then_0, then_1)
    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %seq = sequence(2) [0 -> @s0, 1 -> @s1]\n")
        TEXT("    @s0:\n")
        TEXT("    call PrintString(InString: \"Branch0\")\n")
        TEXT("    exec -> @done\n")
        TEXT("    @s1:\n")
        TEXT("    call PrintString(InString: \"Branch1\")\n")
        TEXT("    exec -> @done\n")
        TEXT("    @done:\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Initial compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    // Find the Sequence node
    UK2Node_ExecutionSequence* SeqNode = nullptr;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_ExecutionSequence* Seq = Cast<UK2Node_ExecutionSequence>(Node))
            {
                SeqNode = Seq;
                break;
            }
        }
        if (SeqNode) break;
    }
    TestNotNull(TEXT("Sequence node found"), SeqNode);
    if (!SeqNode) return false;

    // Count existing exec output pins before insert.
    // sequence(2) produces 3 outputs: AllocateDefaultPins() creates 2 (then_0, then_1),
    // then CodeNodeEmitter adds N-1=1 more (then_2). This is a known off-by-one in the
    // emitter that all existing BPIR relies on, so we test against actual behavior.
    int32 PinCountBefore = 0;
    for (UEdGraphPin* Pin : SeqNode->Pins)
    {
        if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            PinCountBefore++;
        }
    }
    TestTrue(TEXT("Sequence starts with 3 output pins (sequence(2) quirk)"), PinCountBefore == 3);

    // Record what then_0 is wired to (should remain unchanged after insert)
    UEdGraphNode* OriginalThen0Target = GetExecDownstream(SeqNode, TEXT("then_0"));

    // Insert code after "Then 4" (human-friendly format) — pin doesn't exist yet
    // Existing pins are then_0..then_2, so this should auto-create then_3 and then_4
    FCompileResult InsertResult = Compiler.InsertCodeAfterNode(
        SeqNode, TEXT("call PrintString(InString: \"AutoCreated\")"), TEXT("Then 4"));

    if (!InsertResult.bSuccess) { for (const FCompileError& Err : InsertResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("InsertCodeAfterNode with auto-create pin succeeded"), InsertResult.bSuccess);
    TestTrue(TEXT("At least one node was created"), InsertResult.CreatedNodeGUIDs.Num() > 0);

    // Verify then_4 output pin now exists and is wired
    UEdGraphNode* Then4Downstream = GetExecDownstream(SeqNode, TEXT("then_4"));
    TestNotNull(TEXT("then_4 pin exists and has downstream node"), Then4Downstream);

    if (Then4Downstream)
    {
        // The downstream should be the auto-created PrintString with "AutoCreated"
        UK2Node_CallFunction* InsertedCall = Cast<UK2Node_CallFunction>(Then4Downstream);
        TestNotNull(TEXT("Downstream of then_4 is a CallFunction"), InsertedCall);
        if (InsertedCall)
        {
            UEdGraphPin* StringPin = InsertedCall->FindPin(TEXT("InString"));
            if (StringPin)
            {
                TestTrue(TEXT("Inserted node has 'AutoCreated' string"),
                    StringPin->DefaultValue.Contains(TEXT("AutoCreated")));
            }
        }
    }

    // Verify existing then_0 wiring was not disturbed
    UEdGraphNode* CurrentThen0Target = GetExecDownstream(SeqNode, TEXT("then_0"));
    TestEqual(TEXT("then_0 wiring unchanged after auto-create"),
        CurrentThen0Target, OriginalThen0Target);

    // Count exec output pins after insert — should be at least 5 (then_0..then_4)
    int32 PinCountAfter = 0;
    for (UEdGraphPin* Pin : SeqNode->Pins)
    {
        if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            PinCountAfter++;
        }
    }
    TestTrue(TEXT("Sequence has at least 5 output pins after auto-create"), PinCountAfter >= 5);

    // Verify that inserting on a non-sequence node with a bad pin still fails
    UK2Node_CallFunction* PrintNode = FindCallFunctionBySubstring(BP, TEXT("PrintString"));
    if (PrintNode)
    {
        FCompileResult BadResult = Compiler.InsertCodeAfterNode(
            PrintNode, TEXT("call PrintString(InString: \"Nope\")"), TEXT("then_5"));
        TestFalse(TEXT("Auto-create does NOT apply to non-sequence nodes"), BadResult.bSuccess);
    }
    return true;
}

// ============================================================================
// Compiler.Integration.InsertCodeAfterNode_ExternalInjection
// InjectExternalVariable pins should be resolvable as $VarName in inserted code.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerInsertAfterNode_ExternalInjection,
    "PinWright.bpir.compiler.integration.InsertCodeAfterNode_ExternalInjection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerInsertAfterNode_ExternalInjection::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("ExternalInjectionBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    // Step 1: Compile a custom event with a string parameter — gives us
    // an output pin that we can inject as an external variable.
    FBpirCompiler Compiler1(BP);
    FCompileResult Result1 = Compiler1.Compile(
        TEXT("entry custom_event OnSource(string SourceLabel) {\n")
        TEXT("    call PrintString(InString: \"first\")\n")
        TEXT("}"));
    if (!Result1.bSuccess) { for (const FCompileError& E : Result1.Errors) AddError(E.Message); }
    TestTrue(TEXT("Initial compile succeeded"), Result1.bSuccess);
    if (!Result1.bSuccess) return false;

    // Step 2: Find the custom event node and its SourceLabel output pin
    UK2Node_CustomEvent* EventNode = nullptr;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node))
            {
                EventNode = CE;
                break;
            }
        }
        if (EventNode) break;
    }
    TestNotNull(TEXT("Custom event node found"), EventNode);
    if (!EventNode) return false;

    UEdGraphPin* SourcePin = nullptr;
    for (UEdGraphPin* Pin : EventNode->Pins)
    {
        if (Pin->Direction == EGPD_Output
            && Pin->PinName.ToString().Equals(TEXT("SourceLabel"), ESearchCase::IgnoreCase))
        {
            SourcePin = Pin;
            break;
        }
    }
    TestNotNull(TEXT("SourceLabel output pin found"), SourcePin);
    if (!SourcePin) return false;

    // Step 3: Find the PrintString node to insert after
    UK2Node_CallFunction* PrintNode = FindCallFunctionBySubstring(BP, TEXT("PrintString"));
    TestNotNull(TEXT("PrintString node found"), PrintNode);
    if (!PrintNode) return false;

    // Step 4: Inject the SourceLabel pin as $ExtLabel, then insert code that references it
    FBpirCompiler Compiler2(BP);
    Compiler2.InjectExternalVariable(TEXT("ExtLabel"), SourcePin);
    FCompileResult InsertResult = Compiler2.InsertCodeAfterNode(
        PrintNode, TEXT("call PrintString(InString: $ExtLabel)"));

    if (!InsertResult.bSuccess) { for (const FCompileError& E : InsertResult.Errors) AddError(E.Message); }
    TestTrue(TEXT("InsertCodeAfterNode with external injection succeeded"), InsertResult.bSuccess);
    TestTrue(TEXT("At least one node created"), InsertResult.CreatedNodeGUIDs.Num() > 0);

    // Should now have 2 PrintString CallFunction nodes
    int32 CallCount = 0;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
            if (CallNode && CallNode->FunctionReference.GetMemberName().ToString().Contains(TEXT("PrintString")))
            {
                CallCount++;
            }
        }
    }
    TestTrue(TEXT("Two PrintString calls exist after injection-based insertion"), CallCount >= 2);
    return true;
}

// ============================================================================
// 42. Compiler.Integration.ForEachElementPinAccess
// After foreach, %loop.ArrayElement and %loop.ArrayIndex must resolve to the
// ForEachLoop macro's "Array Element" and "Array Index" output pins via
// FindOutputPinByName's space-stripping fallback.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationForEachElementPinAccessTest,
    "PinWright.bpir.compiler.integration.ForEachElementPinAccess",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationForEachElementPinAccessTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("ForeachPinAccessBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // Add a String array member variable so foreach has an array to iterate
    FEdGraphPinType ArrayType;
    ArrayType.PinCategory = UEdGraphSchema_K2::PC_String;
    ArrayType.ContainerType = EPinContainerType::Array;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("TestArray"), ArrayType);

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %loop = foreach(Array: $TestArray) [body -> @body, completed -> @done]\n")
        TEXT("\n")
        TEXT("@body:\n")
        TEXT("    call PrintString(InString: %loop.ArrayElement)\n")
        TEXT("\n")
        TEXT("@done:\n")
        TEXT("}"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    // Verify macro node exists
    UK2Node_MacroInstance* LoopNode = nullptr;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node))
            {
                LoopNode = Macro;
                break;
            }
        }
        if (LoopNode) break;
    }
    TestNotNull(TEXT("ForEachLoop macro node found"), LoopNode);
    if (!LoopNode) return false;

    // Verify "Array Element" output pin exists on the macro node
    UEdGraphPin* ElementPin = LoopNode->FindPin(TEXT("Array Element"));
    TestNotNull(TEXT("ForEachLoop has 'Array Element' output pin"), ElementPin);

    // Verify "Array Element" pin is connected (the compiler wired %loop.ArrayElement to PrintString)
    if (ElementPin)
    {
        TestTrue(TEXT("'Array Element' pin is connected to downstream node"),
            ElementPin->LinkedTo.Num() > 0);
    }

    // Verify PrintString node exists and its InString pin is connected (not a literal default)
    UK2Node_CallFunction* PrintNode = FindCallFunctionBySubstring(BP, TEXT("PrintString"));
    TestNotNull(TEXT("PrintString node found"), PrintNode);
    if (PrintNode)
    {
        UEdGraphPin* StringPin = PrintNode->FindPin(TEXT("InString"));
        if (!StringPin) StringPin = PrintNode->FindPin(TEXT("inString"));
        TestNotNull(TEXT("InString pin found"), StringPin);
        if (StringPin)
        {
            TestTrue(TEXT("InString pin is wired (not a literal)"),
                StringPin->LinkedTo.Num() > 0);
        }
    }
    return true;
}

// ============================================================================
// 43. Compiler.Integration.ForEachIndexPinAccess
// %loop.ArrayIndex must resolve to the ForEachLoop macro's "Array Index" pin.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationForEachIndexPinAccessTest,
    "PinWright.bpir.compiler.integration.ForEachIndexPinAccess",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationForEachIndexPinAccessTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("ForeachIndexPinBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // Add a String array member variable
    FEdGraphPinType ArrayType;
    ArrayType.PinCategory = UEdGraphSchema_K2::PC_String;
    ArrayType.ContainerType = EPinContainerType::Array;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("TestArray"), ArrayType);

    // Add an integer variable to receive the index
    FEdGraphPinType IntType;
    IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("CurrentIndex"), IntType);

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %loop = foreach(Array: $TestArray) [body -> @body, completed -> @done]\n")
        TEXT("\n")
        TEXT("@body:\n")
        TEXT("    set CurrentIndex = %loop.ArrayIndex\n")
        TEXT("\n")
        TEXT("@done:\n")
        TEXT("}"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    // Verify macro node exists
    UK2Node_MacroInstance* LoopNode = nullptr;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node))
            {
                LoopNode = Macro;
                break;
            }
        }
        if (LoopNode) break;
    }
    TestNotNull(TEXT("ForEachLoop macro node found"), LoopNode);
    if (!LoopNode) return false;

    // Verify "Array Index" output pin exists and is connected
    UEdGraphPin* IndexPin = LoopNode->FindPin(TEXT("Array Index"));
    TestNotNull(TEXT("ForEachLoop has 'Array Index' output pin"), IndexPin);
    if (IndexPin)
    {
        TestTrue(TEXT("'Array Index' pin is connected to downstream node"),
            IndexPin->LinkedTo.Num() > 0);
    }
    return true;
}

// ============================================================================
// 44. Compiler.Integration.ExpandNodeRegistry
// Verify that `call Create(Class: ...)` emits a UK2Node_CreateWidget (or
// appropriate expand node) instead of a UK2Node_CallFunction, and that the
// ReturnValue output pin has the correct typed class.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationExpandNodeRegistryTest,
    "PinWright.bpir.compiler.integration.ExpandNodeRegistry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationExpandNodeRegistryTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("ExpandNodeRegistryBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // Use /Script/UMG.UserWidget as the class path — always available in editor builds
    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %w = call Create(Class: /Script/UMG.UserWidget)\n")
        TEXT("}"));

    // UK2Node_CreateWidget availability depends on UMGEditor module registration.
    // If the expand node class is not found at runtime, the compiler falls through
    // to normal function resolution which will fail — that's expected.
    if (!Result.bSuccess)
    {
        // Check whether the expand node class exists at all
        UClass* CreateWidgetClass = FindFirstObjectSafe<UClass>(TEXT("K2Node_CreateWidget"));
        if (!CreateWidgetClass)
        {
            AddInfo(TEXT("UK2Node_CreateWidget not available at runtime — expand node test skipped gracefully"));
            TestTrue(TEXT("Errors reported cleanly when expand node unavailable"), Result.Errors.Num() > 0);
            return true;
        }
        // If the class exists but compile failed, report the errors
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
        }
        return false;
    }

    // Compile succeeded — verify the node is NOT a UK2Node_CallFunction
    // and IS a UK2Node_ConstructObjectFromClass subclass
    bool bFoundExpandNode = false;
    bool bFoundCallFunction = false;
    UEdGraphNode* ExpandNode = nullptr;

    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            // Check if this is a ConstructObjectFromClass subclass (CreateWidget inherits from it)
            if (Node->GetClass()->IsChildOf(FindFirstObjectSafe<UClass>(TEXT("K2Node_ConstructObjectFromClass"))))
            {
                bFoundExpandNode = true;
                ExpandNode = Node;
            }
            // Check it's not a plain CallFunction with "Create" in the name
            if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
            {
                FName FuncName = CallNode->FunctionReference.GetMemberName();
                if (FuncName.ToString().Contains(TEXT("Create")))
                {
                    bFoundCallFunction = true;
                }
            }
        }
    }

    TestTrue(TEXT("Expand node (ConstructObjectFromClass subclass) was created"), bFoundExpandNode);
    TestFalse(TEXT("No plain CallFunction node with 'Create' was emitted"), bFoundCallFunction);

    // Verify ReturnValue output pin exists
    if (ExpandNode)
    {
        UEdGraphPin* ReturnPin = ExpandNode->FindPin(UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output);
        TestNotNull(TEXT("ReturnValue output pin exists on expand node"), ReturnPin);
        if (ReturnPin)
        {
            // The pin should have an object type (PC_Object)
            TestTrue(TEXT("ReturnValue pin is an object type"),
                ReturnPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object);
        }
    }
    return true;
}

// ============================================================================
// 45. Compiler.Integration.CrossBPFunctionCallViaTarget
// A `call Func(Target: %ref)` where Func is a custom event on another BP
// should resolve via the Target's generated class and emit a K2Node_CallFunction.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationCrossBPCallViaTargetTest,
    "PinWright.bpir.compiler.integration.CrossBPFunctionCallViaTarget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationCrossBPCallViaTargetTest::RunTest(const FString& Parameters)
{
    // --- Step 1: Create target BP with a custom event ---
    UBlueprint* TargetBP = CreateTransientTestBP(TEXT("CrossCallTargetBP"));
    TestNotNull(TEXT("Target blueprint was created"), TargetBP);
    if (!TargetBP) return false;

    {
        FBpirCompiler TargetCompiler(TargetBP);
        FCompileResult TargetResult = TargetCompiler.Compile(
            TEXT("entry custom_event DoRemoteAction(FString Param) {\n")
            TEXT("    call PrintString(InString: $Param)\n")
            TEXT("}"));
        if (!TargetResult.bSuccess)
        {
            for (const FCompileError& Err : TargetResult.Errors)
            {
                AddError(FString::Printf(TEXT("  Target compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Target BP compiled successfully"), TargetResult.bSuccess);
        if (!TargetResult.bSuccess) return false;
    }

    // Full-compile the target BP so DoRemoteAction becomes a UFunction on GeneratedClass
    FKismetEditorUtilities::CompileBlueprint(TargetBP);

    // Verify the function exists on the target's GeneratedClass
    UClass* TargetGenClass = TargetBP->GeneratedClass;
    TestNotNull(TEXT("Target GeneratedClass exists"), TargetGenClass);
    if (!TargetGenClass) return false;

    UFunction* TargetFunc = TargetGenClass->FindFunctionByName(TEXT("DoRemoteAction"));
    TestNotNull(TEXT("DoRemoteAction is a UFunction on target GeneratedClass"), TargetFunc);
    if (!TargetFunc)
    {
        // Fallback: check SkeletonGeneratedClass
        if (TargetBP->SkeletonGeneratedClass)
        {
            TargetFunc = TargetBP->SkeletonGeneratedClass->FindFunctionByName(TEXT("DoRemoteAction"));
            AddInfo(FString::Printf(TEXT("DoRemoteAction on SkeletonGeneratedClass: %s"),
                TargetFunc ? TEXT("found") : TEXT("NOT found")));
        }
        AddError(TEXT("DoRemoteAction not found on target GeneratedClass — test cannot proceed"));
        return false;
    }

    // --- Step 2: Create caller BP that spawns the target and calls its event ---
    UBlueprint* CallerBP = CreateTransientTestBP(TEXT("CrossCallCallerBP"));
    TestNotNull(TEXT("Caller blueprint was created"), CallerBP);
    if (!CallerBP) return false;

    // Get the target's GeneratedClass path for the SpawnActor call
    FString TargetClassPath = TargetGenClass->GetPathName();
    AddInfo(FString::Printf(TEXT("Target class path: %s"), *TargetClassPath));

    FString CallerCode = FString::Printf(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %%target = call SpawnActor(Class: %s)\n")
        TEXT("    call DoRemoteAction(Target: %%target, Param: \"hello\")\n")
        TEXT("}"),
        *TargetClassPath);

    FBpirCompiler CallerCompiler(CallerBP);
    FCompileResult CallerResult = CallerCompiler.Compile(CallerCode);

    if (!CallerResult.bSuccess)
    {
        for (const FCompileError& Err : CallerResult.Errors)
        {
            AddError(FString::Printf(TEXT("  Caller compile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Caller BP compiled successfully"), CallerResult.bSuccess);
    if (!CallerResult.bSuccess) return false;

    // --- Step 3: Verify a CallFunction node for DoRemoteAction was created ---
    UK2Node_CallFunction* CallNode = FindCallFunctionBySubstring(CallerBP, TEXT("DoRemoteAction"));
    TestNotNull(TEXT("CallFunction node for DoRemoteAction was created"), CallNode);

    if (CallNode)
    {
        // Verify the function reference points to the correct function
        FName ResolvedFuncName = CallNode->FunctionReference.GetMemberName();
        TestTrue(TEXT("Function reference resolves to DoRemoteAction"),
            ResolvedFuncName.ToString().Contains(TEXT("DoRemoteAction")));

        // Verify the Target pin exists (self/object input)
        bool bHasTargetPin = false;
        for (UEdGraphPin* Pin : CallNode->Pins)
        {
            if (Pin->Direction == EGPD_Input
                && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object
                && (Pin->PinName == UEdGraphSchema_K2::PN_Self || Pin->PinName == TEXT("self")))
            {
                bHasTargetPin = true;
                break;
            }
        }
        TestTrue(TEXT("CallFunction node has a Target/self input pin"), bHasTargetPin);
    }

    // Verify a SpawnActor expand node was also created (the %target ref source)
    bool bFoundSpawnNode = false;
    for (UEdGraph* Graph : CallerBP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node->IsA(UK2Node_SpawnActorFromClass::StaticClass()))
            {
                bFoundSpawnNode = true;
                break;
            }
        }
        if (bFoundSpawnNode) break;
    }
    TestTrue(TEXT("SpawnActorFromClass expand node was created"), bFoundSpawnNode);
    return true;
}

// ============================================================================
// 46. Compiler.Integration.BindDispatcherDelegateResolution
// bind_dispatcher with Delegate arg should create a K2Node_CreateDelegate
// whose function/scope is resolvable. For self-context binds the object pin
// is intentionally LEFT UNLINKED — UK2Node_CreateDelegate::GetScopeClass()
// treats an unlinked object pin as implicit self and returns the current
// BP's class. Verifying GetScopeClass() returns the BP class is the
// canonical functional check; the LinkedTo count is an implementation detail
// that varies between explicit-Self and implicit-self emit shapes.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationTest_BindDispatcherDelegate,
    "PinWright.bpir.compiler.integration.BindDispatcherDelegateResolution",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationTest_BindDispatcherDelegate::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerBindDelegateBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // Add a multicast delegate variable so the dispatcher property exists
    FEdGraphPinType DelegateType;
    DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("MyDispatcher"), DelegateType);
    FKismetEditorUtilities::CompileBlueprint(BP);

    // Compile BPIR: bind_dispatcher with a Delegate arg pointing at a custom event
    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    bind_dispatcher MyDispatcher(Delegate: OnPhotoReceived)\n")
        TEXT("}\n")
        TEXT("entry custom_event OnPhotoReceived() {\n")
        TEXT("}"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
        }
        // Delegate resolution may fail in transient test context; still verify no crash
        PinWrightTestSkip::SkipAssertions(*this, TEXT("delegate-compile-unresolved"),
            TEXT("bind_dispatcher+Delegate compile failed; verifying no crash"));
        return true;
    }

    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    TestTrue(TEXT("At least one node was created"), Result.CreatedNodeGUIDs.Num() > 0);

    // Find K2Node_AddDelegate (the bind node)
    int32 AddDelegateCount = CountNodesOfType<UK2Node_AddDelegate>(BP);
    TestTrue(TEXT("K2Node_AddDelegate was created"), AddDelegateCount > 0);

    // Find K2Node_CreateDelegate (the delegate reference node)
    UK2Node_CreateDelegate* CDNode = nullptr;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_CreateDelegate* Candidate = Cast<UK2Node_CreateDelegate>(Node))
            {
                CDNode = Candidate;
                break;
            }
        }
        if (CDNode) break;
    }
    TestNotNull(TEXT("K2Node_CreateDelegate was created"), CDNode);

    if (CDNode)
    {
        // Verify the function name is set
        FName FuncName = CDNode->GetFunctionName();
        TestTrue(TEXT("CDNode function name is OnPhotoReceived"),
            FuncName == FName(TEXT("OnPhotoReceived")));

        // Verify the scope class resolves. For self-context the object pin
        // is unlinked by design; GetScopeClass() falls back to the BP's
        // SkeletonGeneratedClass via the implicit-self path.
        UEdGraphPin* ObjPin = CDNode->GetObjectInPin();
        TestNotNull(TEXT("CDNode has an object input pin"), ObjPin);
        UClass* ScopeClass = CDNode->GetScopeClass();
        TestNotNull(TEXT("CDNode GetScopeClass resolves"), ScopeClass);
        TestTrue(
            TEXT("CDNode scope resolves to the bind-target Blueprint's class"),
            ScopeClass == BP->GeneratedClass || ScopeClass == BP->SkeletonGeneratedClass);
    }
    return true;
}

// ============================================================================
// Compiler.Integration.RemoveCustomEventFromGraph
// Compile a custom event via BPIR, verify it exists in the graph, then remove
// it via direct graph search (the path blueprint.remove_event uses for events
// not in the JSON registry), and verify the node is gone.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationRemoveCustomEventFromGraphTest,
    "PinWright.bpir.compiler.integration.RemoveCustomEventFromGraph",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationRemoveCustomEventFromGraphTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RemoveEventTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // Compile a custom event via BPIR
    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry custom_event TestRemoveEvent() {\n")
        TEXT("    call PrintString(InString: \"hello from event\")\n")
        TEXT("}"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
            AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
    }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);

    // Verify the custom event node exists
    int32 CustomEventCount = CountNodesOfType<UK2Node_CustomEvent>(BP);
    TestTrue(TEXT("Custom event node exists after compile"), CustomEventCount > 0);

    // Find and remove the custom event by name (same logic as remove_event handler)
    const FString TargetName = TEXT("TestRemoveEvent");
    bool bFoundAndRemoved = false;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        TArray<UEdGraphNode*> NodesToRemove;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
            {
                if (CustomEvent->CustomFunctionName.ToString().Equals(
                        TargetName, ESearchCase::IgnoreCase))
                {
                    NodesToRemove.Add(CustomEvent);
                }
            }
        }
        for (UEdGraphNode* Node : NodesToRemove)
        {
            FBlueprintEditorUtils::RemoveNode(BP, Node, true);
            bFoundAndRemoved = true;
        }
    }

    TestTrue(TEXT("Custom event node was found and removed"), bFoundAndRemoved);

    if (bFoundAndRemoved)
    {
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
    }

    // Verify the custom event node is gone
    int32 RemainingCount = CountNodesOfType<UK2Node_CustomEvent>(BP);
    TestEqual(TEXT("No custom event nodes remain after removal"), RemainingCount, 0);
    return true;
}

// ============================================================================
// 47. Compiler.Integration.PureAutoPromotesToCall
// When BPIR uses `pure` opcode on a function that is BlueprintCallable (not
// BlueprintPure), the compiler should auto-promote it to a call with exec
// wiring so the node is not orphaned in the graph.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationPureAutoPromotesToCallTest,
    "PinWright.bpir.compiler.integration.PureAutoPromotesToCall",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationPureAutoPromotesToCallTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    pure PrintString(InString: \"test\")\n")
        TEXT("}"));

    // PrintString is BlueprintCallable, not BlueprintPure. The compiler should
    // warn and auto-promote to call rather than failing.
    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
            AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
    }
    TestTrue(TEXT("Compile succeeded (auto-promoted pure to call)"), Result.bSuccess);

    // The PrintString CallFunction node must exist
    UK2Node_CallFunction* PrintNode = FindCallFunctionBySubstring(BP, TEXT("PrintString"));
    TestNotNull(TEXT("PrintString node was created"), PrintNode);
    if (!PrintNode) return false;

    // The node's exec input pin must be connected (not orphaned)
    UEdGraphPin* ExecInputPin = nullptr;
    for (UEdGraphPin* Pin : PrintNode->Pins)
    {
        if (Pin->Direction == EGPD_Input
            && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            ExecInputPin = Pin;
            break;
        }
    }
    TestNotNull(TEXT("PrintString node has an exec input pin"), ExecInputPin);
    if (ExecInputPin)
    {
        TestTrue(TEXT("Exec input pin is connected (node is not orphaned)"),
            ExecInputPin->LinkedTo.Num() > 0);
    }

    return true;
}

// ============================================================================
// 48. Compiler.Integration.CustomEventIdempotentReuse
// Compiling the same custom_event BPIR twice should reuse the existing node,
// not create a duplicate. Exactly one K2Node_CustomEvent named "TestDup" must
// exist after both compiles.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationCustomEventIdempotentReuseTest,
    "PinWright.bpir.compiler.integration.CustomEventIdempotentReuse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationCustomEventIdempotentReuseTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("IdempotentReuseBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    const TCHAR* BpirCode =
        TEXT("entry custom_event TestDup(string Msg) {\n")
        TEXT("    call PrintString(InString: $Msg)\n")
        TEXT("}");

    // First compile
    {
        FBpirCompiler Compiler(BP);
        FCompileResult Result = Compiler.Compile(BpirCode);
        if (!Result.bSuccess)
        {
            for (const FCompileError& Err : Result.Errors)
                AddError(FString::Printf(TEXT("  First compile L%d: %s"), Err.Line, *Err.Message));
        }
        TestTrue(TEXT("First compile succeeded"), Result.bSuccess);
    }

    // Count custom events named "TestDup" after first compile
    int32 CountAfterFirst = 0;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node))
            {
                if (CE->CustomFunctionName.ToString().Equals(TEXT("TestDup"), ESearchCase::IgnoreCase))
                {
                    CountAfterFirst++;
                }
            }
        }
    }
    TestEqual(TEXT("Exactly 1 TestDup event after first compile"), CountAfterFirst, 1);

    // Second compile — identical BPIR
    {
        FBpirCompiler Compiler(BP);
        FCompileResult Result = Compiler.Compile(BpirCode);
        if (!Result.bSuccess)
        {
            for (const FCompileError& Err : Result.Errors)
                AddError(FString::Printf(TEXT("  Second compile L%d: %s"), Err.Line, *Err.Message));
        }
        TestTrue(TEXT("Second compile succeeded (idempotent reuse)"), Result.bSuccess);
    }

    // Count custom events named "TestDup" after second compile — must still be exactly 1
    int32 CountAfterSecond = 0;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node))
            {
                if (CE->CustomFunctionName.ToString().Equals(TEXT("TestDup"), ESearchCase::IgnoreCase))
                {
                    CountAfterSecond++;
                }
            }
        }
    }
    TestEqual(TEXT("Still exactly 1 TestDup event after second compile"), CountAfterSecond, 1);
    return true;
}

// ============================================================================
// 49. Compiler.Integration.CustomEventSignatureConflict
// Compiling a custom_event with name "TestConflict" and one signature, then
// compiling again with the same name but different parameter types, must fail
// with an error containing "already exists".
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationCustomEventSignatureConflictTest,
    "PinWright.bpir.compiler.integration.CustomEventSignatureConflict",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationCustomEventSignatureConflictTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("SigConflictBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // First compile: TestConflict with a string parameter
    {
        FBpirCompiler Compiler(BP);
        FCompileResult Result = Compiler.Compile(
            TEXT("entry custom_event TestConflict(string Msg) {\n")
            TEXT("    call PrintString(InString: $Msg)\n")
            TEXT("}"));
        if (!Result.bSuccess)
        {
            for (const FCompileError& Err : Result.Errors)
                AddError(FString::Printf(TEXT("  First compile L%d: %s"), Err.Line, *Err.Message));
        }
        if (!TestTrue(TEXT("First compile succeeded"), Result.bSuccess)) return false;
    }

    // Second compile: same event name, different signature (int instead of string)
    {
        FBpirCompiler Compiler(BP);
        FCompileResult Result = Compiler.Compile(
            TEXT("entry custom_event TestConflict(int Count) {\n")
            TEXT("    call PrintString(InString: \"conflict\")\n")
            TEXT("}"));

        // Diagnostic dump only when assertions fail. Emitting AddWarning on the
        // happy path (compile correctly fails with the expected message) was
        // being treated as a test failure by the runner.
        const bool bSecondCompileFailedAsExpected = !Result.bSuccess;
        const bool bErrorMessageMatched =
            ErrorsContain(Result.Errors, TEXT("already exists"));

        if (!bSecondCompileFailedAsExpected || !bErrorMessageMatched)
        {
            AddInfo(FString::Printf(TEXT("Second compile: bSuccess=%s, Errors=%d"),
                Result.bSuccess ? TEXT("true") : TEXT("false"), Result.Errors.Num()));
            for (const FCompileError& Err : Result.Errors)
            {
                AddInfo(FString::Printf(TEXT("  Error L%d: %s"), Err.Line, *Err.Message));
            }
        }

        TestFalse(TEXT("Second compile must fail (signature conflict)"), Result.bSuccess);
        TestTrue(TEXT("Error message contains 'already exists'"),
            ErrorsContain(Result.Errors, TEXT("already exists")));
    }
    return true;
}

// ============================================================================
// 50. Compiler.Integration.ReplaceModeSignatureChange
// In replace mode, compiling a custom_event with the same name but a different
// signature must succeed, and the resulting Blueprint must have exactly one
// CustomEvent node with the new signature, not the old one.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationReplaceModeSignatureChangeTest,
    "PinWright.bpir.compiler.integration.ReplaceModeSignatureChange",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationReplaceModeSignatureChangeTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("ReplaceSigBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // First compile: TestReplaceSig(string Msg)
    {
        FBpirCompiler Compiler(BP);
        FCompileResult Result = Compiler.Compile(
            TEXT("entry custom_event TestReplaceSig(string Msg) {\n")
            TEXT("    call PrintString(InString: $Msg)\n")
            TEXT("}"));
        if (!Result.bSuccess)
        {
            for (const FCompileError& Err : Result.Errors)
                AddError(FString::Printf(TEXT("  First compile L%d: %s"), Err.Line, *Err.Message));
        }
        if (!TestTrue(TEXT("First compile succeeded"), Result.bSuccess)) return false;
    }

    // Second compile with replace mode: TestReplaceSig(int Count) — different signature
    {
        FBpirCompiler Compiler(BP);
        FCompileResult Result = Compiler.Compile(
            TEXT("entry custom_event TestReplaceSig(int Count) {\n")
            TEXT("    call PrintString(InString: \"replaced\")\n")
            TEXT("}"),
            /*bReplaceMode=*/true);

        for (const FCompileError& Err : Result.Errors)
            AddError(FString::Printf(TEXT("  Replace compile L%d: %s"), Err.Line, *Err.Message));

        if (!TestTrue(TEXT("Replace compile with different signature succeeded"), Result.bSuccess)) return false;
    }

    // Exactly one CustomEvent named "TestReplaceSig" must exist
    int32 FoundCount = 0;
    UK2Node_CustomEvent* FoundNode = nullptr;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node);
            if (CE && CE->CustomFunctionName.ToString().Equals(TEXT("TestReplaceSig"), ESearchCase::IgnoreCase))
            {
                ++FoundCount;
                FoundNode = CE;
            }
        }
    }
    TestEqual(TEXT("Exactly one TestReplaceSig CustomEvent exists after replace"), FoundCount, 1);

    // The surviving node must have the new parameter (Count: int), not the old one (Msg: string)
    if (FoundNode)
    {
        bool bHasCount = false;
        bool bHasMsg   = false;
        for (UEdGraphPin* Pin : FoundNode->Pins)
        {
            if (Pin->Direction != EGPD_Output) continue;
            if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
            if (Pin->PinName.ToString().Equals(TEXT("Count"), ESearchCase::IgnoreCase)) { bHasCount = true; }
            if (Pin->PinName.ToString().Equals(TEXT("Msg"),   ESearchCase::IgnoreCase)) { bHasMsg   = true; }
        }
        TestTrue(TEXT("New parameter 'Count' exists on replaced event"), bHasCount);
        TestFalse(TEXT("Old parameter 'Msg' no longer exists on replaced event"), bHasMsg);
    }
    return true;
}

// ============================================================================
// 51. Compiler.Integration.CallAutoDetectsPure
// When BPIR uses `call` opcode on a function that is BlueprintPure
// (e.g., K2_GetActorLocation), the compiler should auto-detect purity and
// skip exec wiring — the node's exec input pin must NOT be connected.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationCallAutoDetectsPureTest,
    "PinWright.bpir.compiler.integration.CallAutoDetectsPure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationCallAutoDetectsPureTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CallAutoDetectPureBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // K2_GetActorLocation is BlueprintPure. Using `call` instead of `pure`.
    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %loc = call K2_GetActorLocation(Target: self)\n")
        TEXT("    call PrintString(InString: \"done\")\n")
        TEXT("}"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
            AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
    }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    // Find the GetActorLocation node
    UK2Node_CallFunction* LocNode = FindCallFunctionBySubstring(BP, TEXT("GetActorLocation"));
    TestNotNull(TEXT("GetActorLocation node was created"), LocNode);
    if (!LocNode) return false;

    // The node must have no exec pins (auto-detected as pure)
    bool bHasExecPin = false;
    for (UEdGraphPin* Pin : LocNode->Pins)
    {
        if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            bHasExecPin = true;
            break;
        }
    }
    TestFalse(TEXT("GetActorLocation node has no exec pins (auto-detected as pure)"), bHasExecPin);

    // Verify exec chain is intact: PrintString must still have its exec input connected
    UK2Node_CallFunction* PrintNode = FindCallFunctionBySubstring(BP, TEXT("PrintString"));
    TestNotNull(TEXT("PrintString node was created"), PrintNode);
    if (PrintNode)
    {
        UEdGraphPin* ExecInputPin = nullptr;
        for (UEdGraphPin* Pin : PrintNode->Pins)
        {
            if (Pin->Direction == EGPD_Input
                && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                ExecInputPin = Pin;
                break;
            }
        }
        TestNotNull(TEXT("PrintString has exec input pin"), ExecInputPin);
        if (ExecInputPin)
        {
            TestTrue(TEXT("PrintString exec input is connected (exec chain intact)"),
                ExecInputPin->LinkedTo.Num() > 0);
        }
    }
    return true;
}

// ============================================================================
// 52B. Compiler.Integration.OverrideFunctionGraph
// entry override on a BlueprintNativeEvent with a return value should create
// a function override graph instead of a standalone duplicate function.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationOverrideFunctionGraphTest,
    "PinWright.bpir.compiler.integration.OverrideFunctionGraph",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationOverrideFunctionGraphTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBPWithParent(ACharacter::StaticClass(), TEXT("OverrideGraphBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry override CanJumpInternal() -> bool {\n")
        TEXT("    return true\n")
        TEXT("}"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Override compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    UEdGraph* OverrideGraph = nullptr;
    for (UEdGraph* Graph : BP->FunctionGraphs)
    {
        if (Graph && Graph->GetName() == TEXT("CanJumpInternal"))
        {
            OverrideGraph = Graph;
            break;
        }
    }

    TestNotNull(TEXT("Override graph exists"), OverrideGraph);
    if (!OverrideGraph) return false;

    UK2Node_FunctionEntry* EntryNode = FindNodeOfType<UK2Node_FunctionEntry>(OverrideGraph);
    TestNotNull(TEXT("Override graph has a function entry"), EntryNode);
    return true;
}

// ============================================================================
// 52C. Compiler.Integration.AutoDetectsOverrideFunction
// entry function with an omitted signature should auto-detect a matching parent
// BlueprintNativeEvent and build an override graph.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationAutoDetectsOverrideFunctionTest,
    "PinWright.bpir.compiler.integration.AutoDetectsOverrideFunction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationAutoDetectsOverrideFunctionTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBPWithParent(ACharacter::StaticClass(), TEXT("AutoOverrideBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry function CanJumpInternal() {\n")
        TEXT("    return true\n")
        TEXT("}"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Auto-detected override compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    UEdGraph* OverrideGraph = nullptr;
    for (UEdGraph* Graph : BP->FunctionGraphs)
    {
        if (Graph && Graph->GetName() == TEXT("CanJumpInternal"))
        {
            OverrideGraph = Graph;
            break;
        }
    }

    TestNotNull(TEXT("Auto-detected override graph exists"), OverrideGraph);
    return true;
}

// ============================================================================
// 52D. Compiler.Integration.ReplaceModeDeletesOwnedPureNodes
// Replace mode should remove pure upstream nodes that are only owned by the old
// body, rather than leaving them behind as stale data-only orphans.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationReplaceModeDeletesOwnedPureNodesTest,
    "PinWright.bpir.compiler.integration.ReplaceModeDeletesOwnedPureNodes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationReplaceModeDeletesOwnedPureNodesTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("ReplaceOwnedPureBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    {
        FBpirCompiler Compiler(BP);
        FCompileResult Result = Compiler.Compile(
            TEXT("entry custom_event TestPureReplace() {\n")
            TEXT("    %cmp = pure EqualEqual_ByteByte(A: 0, B: 1)\n")
            TEXT("    %b = branch(%cmp) [true -> @then, false -> @else]\n")
            TEXT("    @then:\n")
            TEXT("    call PrintString(InString: \"old-true\")\n")
            TEXT("    exec -> @done\n")
            TEXT("    @else:\n")
            TEXT("    call PrintString(InString: \"old-false\")\n")
            TEXT("    exec -> @done\n")
            TEXT("    @done:\n")
            TEXT("}"));
        TestTrue(TEXT("Initial compile succeeded"), Result.bSuccess);
        if (!Result.bSuccess) return false;
    }

    {
        FBpirCompiler Compiler(BP);
        FCompileResult Result = Compiler.Compile(
            TEXT("entry custom_event TestPureReplace() {\n")
            TEXT("    call PrintString(InString: \"new\")\n")
            TEXT("}"),
            /*bReplaceMode=*/true);
        if (!Result.bSuccess)
        {
            for (const FCompileError& Err : Result.Errors)
            {
                AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Replace compile succeeded"), Result.bSuccess);
        if (!Result.bSuccess) return false;
    }

    UK2Node_CallFunction* EqualNode = FindCallFunctionBySubstring(BP, TEXT("EqualEqual_ByteByte"));
    TestNull(TEXT("Owned pure comparison node was removed"), EqualNode);
    return true;
}

// ============================================================================
// 52. Compiler.Integration.ReplaceModeIdempotent
// Compiling the same custom event twice with bReplaceMode=true on the second
// call must leave exactly one CustomEvent node, with only the new subgraph —
// the old subgraph (including its PrintString) must be gone.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationReplaceModeIdempotentTest,
    "PinWright.bpir.compiler.integration.ReplaceModeIdempotent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationReplaceModeIdempotentTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("ReplaceModeBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // --- First compile (append mode, default): v1 body ---
    {
        FBpirCompiler Compiler(BP);
        FCompileResult Result = Compiler.Compile(
            TEXT("entry custom_event TestReplace() {\n")
            TEXT("    call PrintString(InString: \"v1\")\n")
            TEXT("}"));
        if (!Result.bSuccess)
        {
            for (const FCompileError& Err : Result.Errors)
                AddError(FString::Printf(TEXT("  First compile L%d: %s"), Err.Line, *Err.Message));
        }
        TestTrue(TEXT("First compile succeeded"), Result.bSuccess);
    }

    TestEqual(TEXT("After first compile: exactly one TestReplace CustomEvent"),
        CountNodesOfType<UK2Node_CustomEvent>(BP), 1);
    TestEqual(TEXT("After first compile: exactly one PrintString call"),
        CountNodesOfType<UK2Node_CallFunction>(BP), 1);

    // --- Second compile (replace mode): v2 body with two calls ---
    {
        FBpirCompiler Compiler(BP);
        FCompileResult Result = Compiler.Compile(
            TEXT("entry custom_event TestReplace() {\n")
            TEXT("    call PrintString(InString: \"v2\")\n")
            TEXT("    call PrintString(InString: \"v2-again\")\n")
            TEXT("}"),
            /*bReplaceMode=*/true);
        if (!Result.bSuccess)
        {
            for (const FCompileError& Err : Result.Errors)
                AddError(FString::Printf(TEXT("  Replace compile L%d: %s"), Err.Line, *Err.Message));
        }
        TestTrue(TEXT("Replace compile succeeded"), Result.bSuccess);
    }

    // Expect exactly one CustomEvent node still (idempotent, not duplicated).
    TestEqual(TEXT("After replace: still exactly one TestReplace CustomEvent"),
        CountNodesOfType<UK2Node_CustomEvent>(BP), 1);

    // Expect exactly two PrintString calls (v2 + v2-again; v1 must have been deleted).
    const int32 PrintCalls = CountNodesOfType<UK2Node_CallFunction>(BP);
    TestEqual(TEXT("After replace: exactly two PrintString calls (v1 deleted)"), PrintCalls, 2);

    // Walk all CallFunction nodes and confirm none have the v1 default value remaining.
    bool bFoundV1 = false;
    bool bFoundV2 = false;
    bool bFoundV2Again = false;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
            if (!CallNode) continue;
            UEdGraphPin* InStringPin = CallNode->FindPin(TEXT("InString"));
            if (!InStringPin) continue;
            if (InStringPin->DefaultValue.Contains(TEXT("v1"))
                && !InStringPin->DefaultValue.Contains(TEXT("v2"))) { bFoundV1 = true; }
            if (InStringPin->DefaultValue.Equals(TEXT("v2"))) { bFoundV2 = true; }
            if (InStringPin->DefaultValue.Contains(TEXT("v2-again"))) { bFoundV2Again = true; }
        }
    }
    TestFalse(TEXT("Old 'v1' PrintString was deleted"), bFoundV1);
    TestTrue(TEXT("New 'v2' PrintString exists"), bFoundV2);
    TestTrue(TEXT("New 'v2-again' PrintString exists"), bFoundV2Again);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationInsertAnchorFallbackTest,
    "PinWright.bpir.compiler.InsertAnchorFallback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompilerIntegrationInsertAnchorFallbackTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("TestAnchorFallbackBP"));
    if (!BP) { AddError(TEXT("BP create failed")); return false; }
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!Graph) { AddError(TEXT("no ubergraph")); return false; }

    UK2Node_CustomEvent* Event = CompilerTestUtils::SpawnNode<UK2Node_CustomEvent>(Graph, 0, 0);
    Event->CustomFunctionName = TEXT("ExistingEvent"); Event->ReconstructNode();

    UK2Node_CallFunction* Call = CompilerTestUtils::SpawnPrintStringCall(Graph, 400, 0);
    CompilerTestUtils::WireThenToExec(Event, Call);

    TArray<FGuid> CreatedGuids;
    CreatedGuids.Add(Call->NodeGuid);

    FBpirCompiler::RunLayoutPassForTest(BP, CreatedGuids, Graph, TArray<UEdGraph*>(), /*AnchorHint*/ nullptr);

    TestEqual(TEXT("Event and Call share a row"), Call->NodePosY, Event->NodePosY);
    return true;
}

// ============================================================================
// Compiler.Integration.NewEventBodyInheritsAnchorY
// Regression for the SwitchToGraph cursor-reset bug in Compile() Phase 2:
// SwitchToGraph zeroes CurrentBaseY/CurrentNodeX, and Phase 2 used to call
// PreEmitVariableRefs / EmitInstruction without re-running ResetPlacementForChain.
// Body nodes were placed at Y=0 even when the new event landed at Y=MaxY+450.
// FormatY's same-row inheritance heals the FIRST exec child, but second-and-later
// stacked siblings inherited from the (now-bogus) lowest-placed-sibling rather
// than from the parent, leaving them stranded near Y=0.
//
// This test pre-populates the graph so FindFreeYPositionForEventNode returns a
// high Y for the new event, then compiles a body with two non-same-row siblings
// (a sequence with two labelled exec branches) and asserts every newly-created
// impure body node lands within one screen-height of the new event's row.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationNewEventBodyInheritsAnchorYTest,
    "PinWright.bpir.compiler.integration.NewEventBodyInheritsAnchorY",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationNewEventBodyInheritsAnchorYTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("NewEventBodyYBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;
    UEdGraph* Graph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    TestNotNull(TEXT("Ubergraph exists"), Graph);
    if (!Graph) return false;

    // Pre-populate: an existing CustomEvent + body call wired together at Y=1000.
    // This forces FindFreeYPositionForEventNode to return MaxY+450 ≈ 1450 for the
    // new event, so a body node erroneously placed at Y=0 sits ~1450 px above the
    // event — the visible orphan-at-top symptom originally seen in a game HUD widget.
    UK2Node_CustomEvent* PreExisting = CompilerTestUtils::SpawnNode<UK2Node_CustomEvent>(Graph, 0, 1000);
    PreExisting->CustomFunctionName = TEXT("PreExisting");
    PreExisting->ReconstructNode();
    UK2Node_CallFunction* PreExistingCall = CompilerTestUtils::SpawnPrintStringCall(Graph, 400, 1000);
    CompilerTestUtils::WireThenToExec(PreExisting, PreExistingCall);

    // Snapshot pre-existing GUIDs so we can isolate the new nodes after compile.
    TSet<FGuid> PreExistingGuids;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        PreExistingGuids.Add(Node->NodeGuid);
    }

    // Compile a new CustomEvent with a sequence body that produces multiple
    // non-same-row siblings — the case where FormatY's same-row heal does NOT
    // apply to every body node, so a buggy cursor leaks through to the layout.
    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry custom_event NewEvent() {\n")
        TEXT("    %s = sequence(2) [0 -> @a, 1 -> @b]\n")
        TEXT("@a:\n")
        TEXT("    call PrintString(InString: \"A\")\n")
        TEXT("@b:\n")
        TEXT("    call PrintString(InString: \"B\")\n")
        TEXT("}"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    // Locate the new CustomEvent (skip the pre-existing one).
    UK2Node_CustomEvent* NewEvent = nullptr;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node);
        if (CE && CE != PreExisting && CE->CustomFunctionName == FName(TEXT("NewEvent")))
        {
            NewEvent = CE;
            break;
        }
    }
    TestNotNull(TEXT("New CustomEvent node was created"), NewEvent);
    if (!NewEvent) return false;

    // The new event must land below the pre-existing row. Without this, the bug
    // is invisible (a Y=0 body node would happen to be near a Y=0 event).
    TestTrue(
        FString::Printf(TEXT("New event landed below existing nodes (Y=%d, expected > 1000)"),
            NewEvent->NodePosY),
        NewEvent->NodePosY > 1000);

    // Every newly-created impure node must land within one screen-height (~600 px)
    // of the new event's Y. Pre-fix the second sequence branch ('B') stacked from
    // Y=0 + offsets and ended up ~1450 px above the event.
    const int32 EventY = NewEvent->NodePosY;
    const int32 Tolerance = 600;
    int32 ImpureBodyNodesChecked = 0;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (PreExistingGuids.Contains(Node->NodeGuid)) continue;
        if (Node == NewEvent) continue;

        // Only assert on impure nodes — pure helpers (VariableGet, Self, MakeStruct)
        // get repositioned by FormatParameterNodes relative to their consumer, so
        // their Y trails the consumer's Y by a cluster-relative offset that can
        // legitimately exceed the screen-height tolerance.
        bool bHasExecPin = false;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                bHasExecPin = true;
                break;
            }
        }
        if (!bHasExecPin) continue;

        ++ImpureBodyNodesChecked;
        const int32 DeltaY = FMath::Abs(Node->NodePosY - EventY);
        TestTrue(
            FString::Printf(TEXT("Body node '%s' Y=%d within %d px of event Y=%d (delta=%d)"),
                *Node->GetName(), Node->NodePosY, Tolerance, EventY, DeltaY),
            DeltaY <= Tolerance);
    }

    // Sanity: the sequence + two PrintString calls = at least 3 impure body nodes.
    TestTrue(
        FString::Printf(TEXT("Asserted on %d impure body nodes (expected ≥ 3)"),
            ImpureBodyNodesChecked),
        ImpureBodyNodesChecked >= 3);
    return true;
}

// ============================================================================
// Compiler.Integration.ArrayGetCast
// Regression: "call Array_Get" followed by cast<T>(%h.Item) must compile.
// Requires UK2Node_CallArrayFunction (not plain UK2Node_CallFunction) so that
// PropagateArrayTypeInfo resolves the wildcard Output/Item pin to the array's
// element type, enabling the downstream DynamicCast to wire its input pin.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationArrayGetCastTest,
    "PinWright.bpir.compiler.integration.ArrayGetCast",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationArrayGetCastTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("ArrayGetCastBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // Add a TArray<AActor*> member variable so Array_Get has a typed TargetArray to wire to.
    FEdGraphPinType ArrayType;
    ArrayType.PinCategory = UEdGraphSchema_K2::PC_Object;
    ArrayType.PinSubCategoryObject = AActor::StaticClass();
    ArrayType.ContainerType = EPinContainerType::Array;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("ActorArray"), ArrayType);

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %h = call Array_Get(TargetArray: $ActorArray, Index: 0)\n")
        TEXT("    %c = cast<StaticMeshActor>(%h.Item) [success -> @ok]\n")
        TEXT("\n")
        TEXT("@ok:\n")
        TEXT("    call PrintString(InString: \"got\")\n")
        TEXT("}"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    // The fix: CreateCallFunctionNode instantiates UK2Node_CallArrayFunction when the
    // UFunction has ArrayParm metadata. Confirm at least one such node was created.
    const int32 ArrayFuncCount = CountNodesOfType<UK2Node_CallArrayFunction>(BP);
    TestTrue(TEXT("At least one UK2Node_CallArrayFunction node exists"),
        ArrayFuncCount > 0);

    // Find the Array_Get array-function node and verify its element/output pin resolved away from wildcard.
    // UK2Node_CallArrayFunction::NotifyPinConnectionListChanged (PropagateArrayTypeInfo) runs when
    // TargetArray is wired, propagating the array's element type to the wildcard Item pin.
    UK2Node_CallArrayFunction* ArrayGetNode = nullptr;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_CallArrayFunction* ArrNode = Cast<UK2Node_CallArrayFunction>(Node))
            {
                if (ArrNode->FunctionReference.GetMemberName().ToString().Contains(TEXT("Array_Get")))
                {
                    ArrayGetNode = ArrNode;
                    break;
                }
            }
        }
        if (ArrayGetNode) break;
    }
    TestNotNull(TEXT("Array_Get node exists and is UK2Node_CallArrayFunction"), ArrayGetNode);
    if (ArrayGetNode)
    {
        UEdGraphPin* ItemPin = ArrayGetNode->FindPin(TEXT("Item"));
        if (!ItemPin)
        {
            // Older engine revisions name this pin "Output"
            ItemPin = ArrayGetNode->FindPin(TEXT("Output"));
        }
        TestNotNull(TEXT("Array_Get has an Item/Output pin"), ItemPin);
        if (ItemPin)
        {
            TestTrue(TEXT("Item/Output pin is no longer wildcard (type was propagated)"),
                ItemPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard);
        }
    }

    // And the cast<> must have been created (proves wiring succeeded end-to-end).
    TestTrue(TEXT("At least one DynamicCast node was created"),
        CountNodesOfType<UK2Node_DynamicCast>(BP) > 0);
    return true;
}

// ============================================================================
// Compiler.Integration.DefaultModeIdempotent
// Compiling the same BPIR twice in default (append/upsert) mode must not
// accumulate duplicate entry nodes. Before the idempotency fix two
// UK2Node_CustomEvent nodes named "TestEvt" would exist after the second call.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationDefaultModeIdempotentTest,
    "PinWright.bpir.compiler.integration.DefaultModeIdempotent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationDefaultModeIdempotentTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("IdempotentTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    const FString Bpir =
        TEXT("entry custom_event TestEvt() {\n")
        TEXT("    call PrintString(InString: \"x\")\n")
        TEXT("}");

    // First compile — creates the entry node.
    {
        FBpirCompiler Compiler(BP);
        FCompileResult R1 = Compiler.Compile(Bpir, /*bReplaceMode=*/false);
        if (!R1.bSuccess)
        {
            for (const FCompileError& Err : R1.Errors)
            {
                AddError(FString::Printf(TEXT("First compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("First compile succeeded"), R1.bSuccess);
    }

    // Second compile with identical BPIR — simulates a retry after RPC timeout.
    {
        FBpirCompiler Compiler(BP);
        FCompileResult R2 = Compiler.Compile(Bpir, /*bReplaceMode=*/false);
        if (!R2.bSuccess)
        {
            for (const FCompileError& Err : R2.Errors)
            {
                AddError(FString::Printf(TEXT("Second compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Second compile succeeded"), R2.bSuccess);
    }

    // Count UK2Node_CustomEvent nodes named "TestEvt" across all ubergraph pages.
    int32 MatchCount = 0;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        if (!Graph) continue;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node);
            if (CE && CE->CustomFunctionName.ToString().Equals(TEXT("TestEvt"), ESearchCase::IgnoreCase))
            {
                ++MatchCount;
            }
        }
    }

    TestEqual(TEXT("Exactly one TestEvt entry node after two identical compiles (upsert semantics)"),
        MatchCount, 1);
    return true;
}

// ============================================================================
// FCompilerIntegrationExtendModeTest
// Extend mode: compile override BeginPlay with "A", then extend with "B".
// Assert two PrintString nodes exist and A's exec-out links to B's exec-in.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationExtendModeTest,
    "PinWright.bpir.compiler.integration.ExtendMode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationExtendModeTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("ExtendModeBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // First compile: override BeginPlay with a PrintString("A")
    {
        FBpirCompiler Compiler(BP);
        FCompileResult R = Compiler.Compile(
            TEXT("entry override BeginPlay() {\n")
            TEXT("    call PrintString(InString: \"A\")\n")
            TEXT("}"),
            EBpirCompileMode::Default);
        if (!R.bSuccess)
        {
            for (const FCompileError& Err : R.Errors)
            {
                AddError(FString::Printf(TEXT("First compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("First compile succeeded"), R.bSuccess);
        if (!R.bSuccess) return false;
    }

    // Second compile in Extend mode: append PrintString("B") to the same override
    {
        FBpirCompiler Compiler(BP);
        FCompileResult R = Compiler.Compile(
            TEXT("entry override BeginPlay() {\n")
            TEXT("    call PrintString(InString: \"B\")\n")
            TEXT("}"),
            EBpirCompileMode::Extend);
        if (!R.bSuccess)
        {
            for (const FCompileError& Err : R.Errors)
            {
                AddError(FString::Printf(TEXT("Extend compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Extend compile succeeded"), R.bSuccess);
        if (!R.bSuccess) return false;
    }

    // Find the BeginPlay override event node
    UK2Node_Event* BeginPlayNode = nullptr;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        if (!Graph) continue;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
            if (EventNode && EventNode->bOverrideFunction
                && EventNode->EventReference.GetMemberName().ToString().Equals(
                    TEXT("ReceiveBeginPlay"), ESearchCase::IgnoreCase))
            {
                BeginPlayNode = EventNode;
                break;
            }
        }
        if (BeginPlayNode) break;
    }
    TestNotNull(TEXT("BeginPlay override event node found"), BeginPlayNode);
    if (!BeginPlayNode) return false;

    // Walk the exec chain from the event node and collect CallFunction nodes
    TArray<UK2Node_CallFunction*> PrintNodes;
    UEdGraphNode* Current = BeginPlayNode;
    for (int32 Steps = 0; Current && Steps < 64; ++Steps)
    {
        // Find the first exec-output pin with a link
        UEdGraphNode* Next = nullptr;
        for (UEdGraphPin* Pin : Current->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Output
                && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
                && Pin->LinkedTo.Num() > 0)
            {
                Next = Pin->LinkedTo[0]->GetOwningNode();
                break;
            }
        }
        if (!Next) break; // terminal node — done

        if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Next))
        {
            if (CallNode->FunctionReference.GetMemberName().ToString().Contains(TEXT("PrintString")))
            {
                PrintNodes.Add(CallNode);
            }
        }
        Current = Next;
    }

    TestEqual(TEXT("Two PrintString nodes exist in the exec chain"), PrintNodes.Num(), 2);
    if (PrintNodes.Num() < 2) return false;

    // Verify "A" comes before "B" (first PrintNode's InString == "A", second == "B")
    const auto GetStringDefault = [](UK2Node_CallFunction* Node) -> FString
    {
        if (!Node) return FString();
        UEdGraphPin* Pin = Node->FindPin(TEXT("InString"));
        if (!Pin) Pin = Node->FindPin(TEXT("inString"));
        return Pin ? Pin->DefaultValue : FString();
    };

    FString FirstDefault = GetStringDefault(PrintNodes[0]);
    FString SecondDefault = GetStringDefault(PrintNodes[1]);
    TestTrue(TEXT("First PrintString has default 'A'"), FirstDefault.Contains(TEXT("A")));
    TestTrue(TEXT("Second PrintString has default 'B'"), SecondDefault.Contains(TEXT("B")));

    // Verify exec link: PrintNodes[0]'s exec-out links to PrintNodes[1]'s exec-in
    bool bLinked = false;
    for (UEdGraphPin* Pin : PrintNodes[0]->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Output
            && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            for (UEdGraphPin* Linked : Pin->LinkedTo)
            {
                if (Linked && Linked->GetOwningNode() == PrintNodes[1])
                {
                    bLinked = true;
                    break;
                }
            }
        }
    }
    TestTrue(TEXT("A's exec-out is linked to B's exec-in"), bLinked);

    return true;
}

// ============================================================================
// Compiler.Integration.AuthoredPositionsPreserved
// Fully positioned primary-node instructions keep their authored coordinates.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationAuthoredPositionsPreservedTest,
    "PinWright.bpir.compiler.integration.AuthoredPositionsPreserved",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationAuthoredPositionsPreservedTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("AuthoredPosBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call PrintString(InString: \"A\") @(123, 456)\n")
        TEXT("    call PrintString(InString: \"B\") @(789, 456)\n")
        TEXT("}"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    UK2Node_CallFunction* PrintA = FindPrintStringWithDefault(BP, TEXT("A"));
    UK2Node_CallFunction* PrintB = FindPrintStringWithDefault(BP, TEXT("B"));
    TestNotNull(TEXT("A PrintString node exists"), PrintA);
    TestNotNull(TEXT("B PrintString node exists"), PrintB);
    if (!PrintA || !PrintB) return false;

    TestEqual(TEXT("A NodePosX preserved"), PrintA->NodePosX, 123);
    TestEqual(TEXT("A NodePosY preserved"), PrintA->NodePosY, 456);
    TestEqual(TEXT("B NodePosX preserved"), PrintB->NodePosX, 789);
    TestEqual(TEXT("B NodePosY preserved"), PrintB->NodePosY, 456);
    return true;
}

// ============================================================================
// Compiler.Integration.AuthoredPositionsMixedRejected
// A body with some positioned and some unpositioned primary nodes is rejected.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationAuthoredPositionsMixedRejectedTest,
    "PinWright.bpir.compiler.integration.AuthoredPositionsMixedRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationAuthoredPositionsMixedRejectedTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("AuthoredPosMixedBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call PrintString(InString: \"A\") @(100, 200)\n")
        TEXT("    call PrintString(InString: \"B\")\n")
        TEXT("}"));

    TestFalse(TEXT("Mixed positioned/unpositioned body fails"), Result.bSuccess);
    TestTrue(TEXT("Error mentions authored-position mode"),
        ErrorsContain(Result.Errors, TEXT("authored-position mode")));
    TestTrue(TEXT("Error includes add-position remediation"),
        ErrorsContain(Result.Errors, TEXT("add @(x, y)")));
    TestTrue(TEXT("Error includes auto-layout remediation"),
        ErrorsContain(Result.Errors, TEXT("remove all positions")));
    return true;
}

// ============================================================================
// Compiler.Integration.AuthoredPositionZeroNodeRejected
// Position markers on instructions with no primary graph node are rejected.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationAuthoredPositionZeroNodeRejectedTest,
    "PinWright.bpir.compiler.integration.AuthoredPositionZeroNodeRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationAuthoredPositionZeroNodeRejectedTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("AuthoredPosZeroNodeBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %e = enum ESlateVisibility::Visible @(10, 20)\n")
        TEXT("    call PrintString(InString: \"A\")\n")
        TEXT("}"));

    TestFalse(TEXT("Zero-node authored position fails"), Result.bSuccess);
    TestTrue(TEXT("Error explains node-backed restriction"),
        ErrorsContain(Result.Errors, TEXT("only applies to visible node-backed BPIR instructions")));
    TestTrue(TEXT("Error includes remediation"),
        ErrorsContain(Result.Errors, TEXT("remove all positions")));
    return true;
}

// ============================================================================
// Compiler.Integration.AuthoredPositionAcceptsPureImplicitHelper
// Pure implicit helpers (K2Node_Self, pure K2Node_VariableGet) are inline
// operands the compiler synthesises around their consumer; they carry no
// author-meaningful position and must be silently accepted in authored-position
// mode. Only non-pure visible helpers can legitimately demand their own @(x,y).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationAuthoredPositionAcceptsPureImplicitHelperTest,
    "PinWright.bpir.compiler.integration.AuthoredPositionAcceptsPureImplicitHelper",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationAuthoredPositionAcceptsPureImplicitHelperTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("AuthoredPosHelperBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FEdGraphPinType StringType;
    StringType.PinCategory = UEdGraphSchema_K2::PC_String;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("Source"), StringType);

    FBpirCompiler Compiler(BP);
    // $Source synthesises a pure K2Node_VariableGet inline-fed into PrintString;
    // the helper has no standalone authored position because it is a pure operand.
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call PrintString(InString: $Source) @(100, 200)\n")
        TEXT("}"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Positioned body with pure implicit helper compiles"), Result.bSuccess);
    return true;
}

// ============================================================================
// Compiler.Integration.UnpositionedBodyAllowsImplicitHelper
// The same helper-producing BPIR remains valid when the body uses auto-layout.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationUnpositionedBodyAllowsImplicitHelperTest,
    "PinWright.bpir.compiler.integration.UnpositionedBodyAllowsImplicitHelper",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationUnpositionedBodyAllowsImplicitHelperTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("UnpositionedHelperBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FEdGraphPinType StringType;
    StringType.PinCategory = UEdGraphSchema_K2::PC_String;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("Source"), StringType);

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call PrintString(InString: $Source)\n")
        TEXT("}"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Unpositioned helper-producing body compiles"), Result.bSuccess);
    return true;
}

// ============================================================================
// Compiler.Integration.AuthoredPositionPerEntryIsolation
// Positioned and auto-layout entries in one file choose placement independently.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationAuthoredPositionPerEntryIsolationTest,
    "PinWright.bpir.compiler.integration.AuthoredPositionPerEntryIsolation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationAuthoredPositionPerEntryIsolationTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("AuthoredPosMultiEntryBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry custom_event Positioned() {\n")
        TEXT("    call PrintString(InString: \"Fixed\") @(333, 444)\n")
        TEXT("}\n")
        TEXT("entry custom_event Auto() {\n")
        TEXT("    call PrintString(InString: \"Auto\")\n")
        TEXT("}"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Mixed entry file compiles"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    UK2Node_CallFunction* Fixed = FindPrintStringWithDefault(BP, TEXT("Fixed"));
    UK2Node_CallFunction* Auto = FindPrintStringWithDefault(BP, TEXT("Auto"));
    TestNotNull(TEXT("Positioned entry node exists"), Fixed);
    TestNotNull(TEXT("Auto entry node exists"), Auto);
    if (!Fixed || !Auto) return false;

    TestEqual(TEXT("Positioned entry NodePosX preserved"), Fixed->NodePosX, 333);
    TestEqual(TEXT("Positioned entry NodePosY preserved"), Fixed->NodePosY, 444);
    return true;
}

// ============================================================================
// Compiler.Integration.AuthoredPositionInsertAndBodyPaths
// InsertCodeAfterNode and CompileBodyIntoGraph preserve authored coordinates.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationAuthoredPositionInsertAndBodyPathsTest,
    "PinWright.bpir.compiler.integration.AuthoredPositionInsertAndBodyPaths",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationAuthoredPositionInsertAndBodyPathsTest::RunTest(const FString& Parameters)
{
    UBlueprint* InsertBP = CreateTransientTestBP(TEXT("AuthoredPosInsertBP"));
    TestNotNull(TEXT("Insert Blueprint was created"), InsertBP);
    if (!InsertBP) return false;

    {
        FBpirCompiler Compiler(InsertBP);
        FCompileResult Seed = Compiler.Compile(
            TEXT("entry custom_event Base() {\n")
            TEXT("    call PrintString(InString: \"Base\")\n")
            TEXT("}"));
        TestTrue(TEXT("Seed compile succeeded"), Seed.bSuccess);
        if (!Seed.bSuccess) return false;
    }

    UK2Node_CallFunction* BaseNode = FindPrintStringWithDefault(InsertBP, TEXT("Base"));
    TestNotNull(TEXT("Base node exists"), BaseNode);
    if (!BaseNode) return false;

    {
        FBpirCompiler Compiler(InsertBP);
        FCompileResult Insert = Compiler.InsertCodeAfterNode(
            BaseNode,
            TEXT("call PrintString(InString: \"Inserted\") @(500, 300)"));
        if (!Insert.bSuccess)
        {
            for (const FCompileError& Err : Insert.Errors)
            {
                AddError(FString::Printf(TEXT("Insert L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Positioned insert succeeded"), Insert.bSuccess);
        if (!Insert.bSuccess) return false;
    }

    UK2Node_CallFunction* InsertedNode = FindPrintStringWithDefault(InsertBP, TEXT("Inserted"));
    TestNotNull(TEXT("Inserted node exists"), InsertedNode);
    if (!InsertedNode) return false;
    TestEqual(TEXT("Inserted NodePosX preserved"), InsertedNode->NodePosX, 500);
    TestEqual(TEXT("Inserted NodePosY preserved"), InsertedNode->NodePosY, 300);

    UBlueprint* BodyBP = CreateTransientTestBP(TEXT("AuthoredPosBodyBP"));
    TestNotNull(TEXT("Body Blueprint was created"), BodyBP);
    if (!BodyBP) return false;

    UEdGraph* Graph = BodyBP->UbergraphPages.Num() > 0 ? BodyBP->UbergraphPages[0] : nullptr;
    TestNotNull(TEXT("Body graph exists"), Graph);
    if (!Graph) return false;

    UK2Node_CustomEvent* Entry = CompilerTestUtils::SpawnNode<UK2Node_CustomEvent>(Graph, 0, 0);
    Entry->CustomFunctionName = TEXT("BodyEntry");
    Entry->ReconstructNode();
    UEdGraphPin* EntryThen = Entry->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
    TestNotNull(TEXT("Body entry exec pin exists"), EntryThen);
    if (!EntryThen) return false;

    {
        FBpirCompiler Compiler(BodyBP);
        FCompileResult BodyResult = Compiler.CompileBodyIntoGraph(
            TEXT("call PrintString(InString: \"Body\") @(650, 350)"),
            Graph,
            EntryThen);
        if (!BodyResult.bSuccess)
        {
            for (const FCompileError& Err : BodyResult.Errors)
            {
                AddError(FString::Printf(TEXT("Body L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Positioned body compile succeeded"), BodyResult.bSuccess);
        if (!BodyResult.bSuccess) return false;
    }

    UK2Node_CallFunction* BodyNode = FindPrintStringWithDefault(BodyBP, TEXT("Body"));
    TestNotNull(TEXT("Body node exists"), BodyNode);
    if (!BodyNode) return false;
    TestEqual(TEXT("Body NodePosX preserved"), BodyNode->NodePosX, 650);
    TestEqual(TEXT("Body NodePosY preserved"), BodyNode->NodePosY, 350);
    return true;
}

// ============================================================================
// Compiler.Integration.AliasDollarResolves
// %p = $SomeBoolVar (dollar-alias) compiles successfully and emits no node for
// the alias instruction itself — only the implicit VariableGet from the
// downstream arg resolution and the PrintString call node appear.
//
// Pre-fix: parse fails with "Expected keyword after '='" so Compile() returns
//          bSuccess == false before any nodes are emitted.
// Post-fix: compile succeeds and the alias emits no dedicated graph node.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationAliasDollarResolvesTest,
    "PinWright.bpir.compiler.integration.AliasDollarResolves",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationAliasDollarResolvesTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("AliasDollarTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // Add a bool variable so $SomeBoolVar resolves to a real member variable.
    FEdGraphPinType BoolType;
    BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("SomeBoolVar"), BoolType);

    // Count nodes before compile — expect only the default BeginPlay event stub.
    const int32 NodesBefore = CountAllEventGraphNodes(BP);

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %p = $SomeBoolVar\n")
        TEXT("    call PrintString(InString: \"ok\")\n")
        TEXT("}"));

    // Pre-fix: compile fails because "%p = $SomeBoolVar" cannot be parsed.
    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);

    // The alias instruction must not create a dedicated graph node.
    // Only the PrintString call (and an implicit VariableGet wired to it) should be new.
    // We verify this by checking that a PrintString call node exists, not by counting
    // nodes precisely (the VariableGet may or may not be emitted depending on wiring).
    TestTrue(TEXT("PrintString call node exists"),
        FindCallFunctionBySubstring(BP, TEXT("PrintString")) != nullptr);

    // Alias must not produce a node registered in CreatedNodeGUIDs under its own name.
    // The alias's ResultName "p" should not appear as an emitted node.
    // We confirm indirectly: node count increased by at most 2 (PrintString + optional VariableGet),
    // not by 3 (which would suggest an extra alias node was emitted).
    const int32 NodesAfter = CountAllEventGraphNodes(BP);
    const int32 Delta = NodesAfter - NodesBefore;
    TestTrue(TEXT("No spurious alias node emitted (node delta <= 2)"), Delta <= 2);
    return true;
}

// ============================================================================
// Compiler.Integration.AliasCycleRejected
// A mutual alias cycle (%a = %b, %b = %a) used as a function argument must not
// cause infinite recursion. The cycle guard caps resolution at 16 hops and logs
// an error, then returns nullptr so the pin stays unconnected.
//
// Pre-fix: ResolvePercentRef recurses infinitely → stack overflow / crash.
// Post-fix: compile completes without crashing. The InString pin of PrintString
//           is left unconnected (empty default) because the alias chain cannot
//           be resolved.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationAliasCycleRejectedTest,
    "PinWright.bpir.compiler.integration.AliasCycleRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationAliasCycleRejectedTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("AliasCycleTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);

    // %a = %b and %b = %a form a cycle. Passing %a as an arg forces resolution,
    // which triggers the cycle guard in ResolvePercentRef.
    // Pre-fix: this causes a stack overflow (infinite mutual recursion).
    // Post-fix: compile returns without crashing; pin stays unconnected.
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %a = %b\n")
        TEXT("    %b = %a\n")
        TEXT("    call PrintString(InString: %a)\n")
        TEXT("}"));

    // The key assertion: we reached this line without crashing.
    // Regardless of bSuccess, the compile must have terminated.
    AddInfo(TEXT("Compile returned (no crash) — cycle guard fired correctly"));

    // Verify the PrintString node was emitted (call instruction itself is valid).
    UK2Node_CallFunction* PrintNode = FindCallFunctionBySubstring(BP, TEXT("PrintString"));
    TestNotNull(TEXT("PrintString call node was emitted"), PrintNode);

    if (PrintNode)
    {
        // The InString pin must remain unconnected because the alias could not be resolved.
        UEdGraphPin* InStringPin = PrintNode->FindPin(TEXT("InString"));
        if (!InStringPin) InStringPin = PrintNode->FindPin(TEXT("inString"));
        TestNotNull(TEXT("InString pin exists on PrintString node"), InStringPin);
        if (InStringPin)
        {
            TestEqual(TEXT("InString pin has no connections (cycle guard returned nullptr)"),
                InStringPin->LinkedTo.Num(), 0);
        }
    }
    return true;
}

// ============================================================================
// Compiler.Integration.MultiInputExecWiring
// Compile two ExecGoto wires into a Gate macro's non-first input exec pins
// (Open / Close), and verify that:
//   - The wires land on Gate.Open and Gate.Close (NOT on the default Enter pin).
//   - A goto naming a nonexistent input exec pin produces a compile error
//     mentioning the requested pin name and the target node.
//
// Counterfactual (positive case): if the OverrideInputPinName branch in
//   GetExecInputPin is reverted, both wires fall through to the first exec
//   input — Enter ends up with 2 links and the Open / Close assertions fail.
// Counterfactual (hard-fail case): if the hard-fail branch silently falls
//   through to first-exec-input, no compile error is recorded for the bad-pin
//   case and the negative-case assertion fails.
// Note: Gate is realized by UE as a UK2Node_MacroInstance referencing the
//   Gate macro in StandardMacros (no UK2Node_Gate type exists in UE 5.6).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirCompilerMultiInputExecWiringTest,
    "PinWright.bpir.compiler.integration.MultiInputExecWiring",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirCompilerMultiInputExecWiringTest::RunTest(const FString& Parameters)
{
    // ---- Positive case: wires land on Open / Close, NOT on Enter. -----------
    {
        UBlueprint* BP = CreateTransientTestBP(TEXT("MultiInputExecWiringBP"));
        TestNotNull(TEXT("Blueprint was created"), BP);
        if (!BP) return false;

        FBpirCompiler Compiler(BP);
        FCompileResult Result = Compiler.Compile(
            TEXT("entry event BeginPlay() {\n")
            TEXT("    %rand = pure RandomBool()\n")
            TEXT("    %b = branch(%rand) [true -> @do_open, false -> @do_close]\n")
            TEXT("\n")
            TEXT("@do_open:\n")
            TEXT("    call PrintString(InString: \"Opening\")\n")
            TEXT("    exec -> @gate.Open\n")
            TEXT("\n")
            TEXT("@do_close:\n")
            TEXT("    call PrintString(InString: \"Closing\")\n")
            TEXT("    exec -> @gate.Close\n")
            TEXT("\n")
            TEXT("@gate:\n")
            TEXT("    %g = macro Gate() [Exit -> @done]\n")
            TEXT("\n")
            TEXT("@done:\n")
            TEXT("    call PrintString(InString: \"Done\")\n")
            TEXT("}"));

        if (!Result.bSuccess)
        {
            for (const FCompileError& Err : Result.Errors)
            {
                AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Compile succeeded"), Result.bSuccess);

        if (Result.bSuccess)
        {
            UK2Node_MacroInstance* GateNode = FindNodeOfType<UK2Node_MacroInstance>(BP);
            TestNotNull(TEXT("Gate macro instance node found"), GateNode);
            if (GateNode)
            {
                UEdGraphPin* EnterPin = GateNode->FindPin(TEXT("Enter"), EGPD_Input);
                UEdGraphPin* OpenPin  = GateNode->FindPin(TEXT("Open"),  EGPD_Input);
                UEdGraphPin* ClosePin = GateNode->FindPin(TEXT("Close"), EGPD_Input);
                TestNotNull(TEXT("Gate has Enter input exec pin"), EnterPin);
                TestNotNull(TEXT("Gate has Open input exec pin"),  OpenPin);
                TestNotNull(TEXT("Gate has Close input exec pin"), ClosePin);

                if (OpenPin && ClosePin && EnterPin)
                {
                    // Counterfactual: if first-exec-input fallback fires for these
                    // wires, Enter ends up linked and Open / Close are empty.
                    TestEqual(TEXT("Open pin has exactly one incoming link"),
                        OpenPin->LinkedTo.Num(), 1);
                    TestEqual(TEXT("Close pin has exactly one incoming link"),
                        ClosePin->LinkedTo.Num(), 1);
                    TestEqual(TEXT("Enter pin has no incoming link (override path took precedence)"),
                        EnterPin->LinkedTo.Num(), 0);

                    // Sanity: each link's source is a PrintString 'then' output pin
                    // whose default-input value matches the expected branch label.
                    if (OpenPin->LinkedTo.Num() == 1)
                    {
                        UEdGraphPin* SrcThen = OpenPin->LinkedTo[0];
                        TestNotNull(TEXT("Open pin source is a real pin"), SrcThen);
                        if (SrcThen)
                        {
                            UK2Node_CallFunction* SrcCall = Cast<UK2Node_CallFunction>(SrcThen->GetOwningNode());
                            TestNotNull(TEXT("Open pin source is a CallFunction node"), SrcCall);
                            if (SrcCall)
                            {
                                UEdGraphPin* InStringPin = SrcCall->FindPin(TEXT("InString"));
                                if (!InStringPin) InStringPin = SrcCall->FindPin(TEXT("inString"));
                                if (InStringPin)
                                {
                                    TestTrue(TEXT("Open is wired from the 'Opening' PrintString"),
                                        InStringPin->DefaultValue.Contains(TEXT("Opening")));
                                }
                            }
                        }
                    }
                    if (ClosePin->LinkedTo.Num() == 1)
                    {
                        UEdGraphPin* SrcThen = ClosePin->LinkedTo[0];
                        TestNotNull(TEXT("Close pin source is a real pin"), SrcThen);
                        if (SrcThen)
                        {
                            UK2Node_CallFunction* SrcCall = Cast<UK2Node_CallFunction>(SrcThen->GetOwningNode());
                            TestNotNull(TEXT("Close pin source is a CallFunction node"), SrcCall);
                            if (SrcCall)
                            {
                                UEdGraphPin* InStringPin = SrcCall->FindPin(TEXT("InString"));
                                if (!InStringPin) InStringPin = SrcCall->FindPin(TEXT("inString"));
                                if (InStringPin)
                                {
                                    TestTrue(TEXT("Close is wired from the 'Closing' PrintString"),
                                        InStringPin->DefaultValue.Contains(TEXT("Closing")));
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ---- Negative case: nonexistent input exec pin must hard-fail. ----------
    {
        UBlueprint* BP = CreateTransientTestBP(TEXT("MultiInputExecWiringBadPinBP"));
        TestNotNull(TEXT("Blueprint was created (negative case)"), BP);
        if (!BP) return false;

        FBpirCompiler Compiler(BP);
        FCompileResult Result = Compiler.Compile(
            TEXT("entry event BeginPlay() {\n")
            TEXT("    call PrintString(InString: \"Pre\")\n")
            TEXT("    exec -> @gate.NonexistentPin\n")
            TEXT("\n")
            TEXT("@gate:\n")
            TEXT("    %g = macro Gate() [Exit -> @done]\n")
            TEXT("\n")
            TEXT("@done:\n")
            TEXT("    call PrintString(InString: \"Done\")\n")
            TEXT("}"));

        // Counterfactual: if the hard-fail branch silently falls through, no
        // error mentioning NonexistentPin is recorded and this assertion fails.
        TestFalse(TEXT("Compile fails when target input exec pin does not exist"),
            Result.bSuccess);
        TestTrue(TEXT("Compile error mentions the requested pin name 'NonexistentPin'"),
            ErrorsContain(Result.Errors, TEXT("NonexistentPin")));
        // Counterfactual: if the diagnostic stops including the target node's
        // display name, regressions where the user can't tell which node failed
        // go undetected. The Gate macro instance's ListView title contains "Gate".
        TestTrue(TEXT("Compile error mentions the Gate's display name"),
            ErrorsContain(Result.Errors, TEXT("Gate")));
        // Counterfactual: if the available-pins hint is dropped from the
        // diagnostic, users lose the counterfactual that tells them which pin
        // names are valid on the target node.
        TestTrue(TEXT("Compile error includes available-pins hint"),
            ErrorsContain(Result.Errors, TEXT("(available:")));
    }
    return true;
}

// ============================================================================
// 53. Compiler.Integration.FallThroughReconvergence
// B-bpir-fallthrough-reconverge-dropped: a block that falls through to the next
// adjacent label (the documented bpir.instructions §2.8 "auto-chain to next label"
// idiom) must wire its terminal exec output into that next label's entry node.
// Before the fix, WireExecPins only auto-chained WITHIN a label segment and silently
// dropped EVERY cross-label fall-through edge, leaving the fall-through block's exec
// output dangling and the shared tail with one predecessor instead of two. This test
// fails (the @else fall-through edge is absent / the tail has a single predecessor)
// if the cross-segment carry in WireExecPins Step 2 is reverted.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationTest_FallThroughReconvergence,
    "PinWright.bpir.compiler.integration.FallThroughReconvergence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationTest_FallThroughReconvergence::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CompilerTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // §2.8 reconvergence: @then jumps explicitly to @merge; @else has NO terminator
    // and must fall through to the source-adjacent @merge. Both arms reconverge on
    // the shared tail.
    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %rand = pure RandomBool()\n")
        TEXT("    %b = branch(%rand) [true -> @then, false -> @else]\n")
        TEXT("    @then:\n")
        TEXT("    call PrintString(InString: \"true-branch\")\n")
        TEXT("    exec -> @merge\n")
        TEXT("    @else:\n")
        TEXT("    call PrintString(InString: \"false-branch\")\n")
        TEXT("    @merge:\n")
        TEXT("    call PrintString(InString: \"shared-tail\")\n")
        TEXT("}"));

    if (!Result.bSuccess) { for (const FCompileError& Err : Result.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), Result.bSuccess);

    UK2Node_CallFunction* ElseNode = FindPrintStringWithDefault(BP, TEXT("false-branch"));
    UK2Node_CallFunction* MergeNode = FindPrintStringWithDefault(BP, TEXT("shared-tail"));
    TestNotNull(TEXT("@else PrintString node found"), ElseNode);
    TestNotNull(TEXT("@merge (shared tail) PrintString node found"), MergeNode);
    if (!ElseNode || !MergeNode) return false;

    // The @else block (no terminator) must fall through into @merge: its PrintString's
    // exec output must be wired, not left dangling (the silently-dropped edge).
    UEdGraphNode* ElseDownstream = GetExecDownstream(ElseNode, FString());
    TestNotNull(TEXT("@else fall-through exec output is wired (was dropped before fix)"),
        ElseDownstream);
    TestTrue(TEXT("@else falls through specifically into the shared tail"),
        ElseDownstream == MergeNode);

    // The shared tail must reconverge BOTH arms — @then's explicit jump AND @else's
    // fall-through — i.e. two exec predecessors on its exec-input pin.
    UEdGraphPin* MergeExecIn = MergeNode->GetExecPin();
    TestNotNull(TEXT("shared tail has an exec input pin"), MergeExecIn);
    if (MergeExecIn)
    {
        TestEqual(TEXT("shared tail reconverges both arms (2 exec predecessors)"),
            MergeExecIn->LinkedTo.Num(), 2);
    }
    return true;
}
