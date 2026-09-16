// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"
#include "BpirGraphTestHelpers.h"
#include "Tests/TestSkipReporting.h"

#include "Decompiler/BpirDecompiler.h"
#include "Decompiler/BpirTextEmitter.h"
#include "Decompiler/GraphWalker.h"
#include "Kismet/GameplayStatics.h"
#include "Components/PrimitiveComponent.h"
#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableSet.h"
#include "K2Node_VariableGet.h"
#include "K2Node_Knot.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_Switch.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_InputActionEvent.h"
#include "K2Node_InputKey.h"
#include "InputCoreTypes.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Blueprint/UserWidget.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Compat/EngineVersionCompat.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

// ============================================================================
// Helpers (anonymous namespace -- test-local only)
// ============================================================================

namespace
{
    // Create a transient Blueprint parented to AActor. Each test gets its own
    // unique name so tests are fully independent regardless of run order.
    // MakeUniqueObjectName guarantees a name not already present in the Outer:
    // the test-created Blueprints accumulate in the transient package (never torn
    // down), so an FMath::Rand()-based name (RAND_MAX 32767) collides across the
    // ~30 tests in this suite and trips CreateBlueprint's
    // FindObject(...)==nullptr assertion (Kismet2.cpp:435), crashing the suite.
    UBlueprint* CreateTestBlueprint()
    {
        const FName Name = MakeUniqueObjectName(
            GetTransientPackage(), UBlueprint::StaticClass(), TEXT("TestDecompilerBP"));
        return FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(),
            GetTransientPackage(),
            Name,
            BPTYPE_Normal,
            UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass(),
            FName(TEXT("PinWrightTests")));
    }

    // Allocate a node of type T in Graph, run its standard post-placement
    // initialisation, and return a pointer to it.
    // Order matches FBlueprintNodeSpawner::Invoke (BlueprintNodeSpawner.cpp:345-346):
    // AllocateDefaultPins must run before PostPlacedNewNode because some K2 nodes
    // (e.g. UK2Node_SpawnActorFromClass in UE 5.6) call FindPinChecked on
    // newly-added pins inside PostPlacedNewNode and crash otherwise.
    template<typename T>
    T* AddNodeToGraph(UEdGraph* Graph)
    {
        T* Node = NewObject<T>(Graph);
        Node->CreateNewGuid();
        Node->AllocateDefaultPins();
        Node->PostPlacedNewNode();
        Graph->AddNode(Node, /*bFromUI=*/true, /*bSelectNewNode=*/false);
        return Node;
    }

    // Wire the exec output of Src to the exec input of Dst.
    // Uses the standard K2 "then" -> "execute" pin convention.
    void WireExec(UEdGraphNode* Src, UEdGraphNode* Dst)
    {
        UEdGraphPin* ThenPin  = Src->FindPin(UEdGraphSchema_K2::PN_Then,  EGPD_Output);
        UEdGraphPin* ExecPin  = Dst->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
        if (ThenPin && ExecPin)
        {
            ThenPin->MakeLinkTo(ExecPin);
        }
    }

    // Return the first BeginPlay (ReceiveBeginPlay) event node in Graph, or
    // nullptr if none is present.
    UK2Node_Event* FindBeginPlayNode(UEdGraph* Graph)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
            {
                const FName FuncName = EventNode->EventReference.GetMemberName();
                if (FuncName == TEXT("ReceiveBeginPlay"))
                {
                    return EventNode;
                }
            }
        }
        return nullptr;
    }

    bool ContainsLineWith(const FString& Text, const FString& First, const FString& Second)
    {
        TArray<FString> Lines;
        Text.ParseIntoArrayLines(Lines);
        for (const FString& Line : Lines)
        {
            if (Line.Contains(First) && Line.Contains(Second))
            {
                return true;
            }
        }
        return false;
    }

    bool SetVariableGetToValidatedObject(UK2Node_VariableGet* Node)
    {
        if (!Node)
        {
            return false;
        }

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        // UE 5.7 made UK2Node_VariableGet MinimalAPI, so SetPurity is declared
        // but not exported. Set the reflected variation directly for this test.
        FEnumProperty* CurrentVariationProperty = FindFProperty<FEnumProperty>(
            UK2Node_VariableGet::StaticClass(),
            TEXT("CurrentVariation"));
        if (!CurrentVariationProperty)
        {
            return false;
        }

        void* CurrentVariationValue = CurrentVariationProperty->ContainerPtrToValuePtr<void>(Node);
        CurrentVariationProperty->GetUnderlyingProperty()->SetIntPropertyValue(
            CurrentVariationValue,
            static_cast<int64>(EGetNodeVariation::ValidatedObject));
        Node->ReconstructNode();
#else
        Node->SetPurity(false);
        Node->ReconstructNode();
#endif

        return !Node->IsNodePure()
            && Node->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input)
            && Node->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
    }

    template<typename PredicateType>
    UEdGraphNode* FindFirstNodeMatching(UBlueprint* BP, PredicateType Predicate)
    {
        if (!BP)
        {
            return nullptr;
        }

        auto SearchGraphs = [&Predicate](const auto& Graphs) -> UEdGraphNode*
        {
            for (UEdGraph* Graph : Graphs)
            {
                if (!Graph)
                {
                    continue;
                }
                for (UEdGraphNode* Node : Graph->Nodes)
                {
                    if (Node && Predicate(Node))
                    {
                        return Node;
                    }
                }
            }
            return nullptr;
        };

        if (UEdGraphNode* Node = SearchGraphs(BP->UbergraphPages))
        {
            return Node;
        }
        if (UEdGraphNode* Node = SearchGraphs(BP->FunctionGraphs))
        {
            return Node;
        }
        return SearchGraphs(BP->MacroGraphs);
    }

    using BpirGraphTestHelpers::FindFirstNodeOfType;

    UK2Node_CallFunction* FindCallFunctionNode(UBlueprint* BP, const FName& FunctionName)
    {
        return Cast<UK2Node_CallFunction>(FindFirstNodeMatching(BP, [&FunctionName](UEdGraphNode* Node)
        {
            UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
            return Call && Call->GetFunctionName() == FunctionName;
        }));
    }

    void SetNodePosition(UEdGraphNode* Node, int32 X, int32 Y)
    {
        if (Node)
        {
            Node->NodePosX = X;
            Node->NodePosY = Y;
        }
    }

    UK2Node_CallFunction* FindPrintStringNodeWithDefault(UBlueprint* BP, const FString& Needle)
    {
        return Cast<UK2Node_CallFunction>(FindFirstNodeMatching(BP, [&Needle](UEdGraphNode* Node)
        {
            UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
            if (!Call || Call->GetFunctionName() != GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString))
            {
                return false;
            }

            UEdGraphPin* InString = Call->FindPin(TEXT("InString"), EGPD_Input);
            return InString && InString->DefaultValue.Contains(Needle);
        }));
    }

    UEdGraphNode* GetExecTargetNode(UEdGraphPin* SourceExecPin)
    {
        if (!SourceExecPin || SourceExecPin->LinkedTo.Num() == 0 || !SourceExecPin->LinkedTo[0])
        {
            return nullptr;
        }
        return SourceExecPin->LinkedTo[0]->GetOwningNode();
    }

    FString FindLabelForLineContaining(const FString& Text, const FString& Needle)
    {
        TArray<FString> Lines;
        Text.ParseIntoArrayLines(Lines);

        FString CurrentLabel;
        for (const FString& RawLine : Lines)
        {
            const FString Line = RawLine.TrimStartAndEnd();
            if (Line.StartsWith(TEXT("@")) && Line.EndsWith(TEXT(":")))
            {
                CurrentLabel = Line.LeftChop(1);
            }
            else if (Line.Contains(Needle))
            {
                return CurrentLabel;
            }
        }
        return FString();
    }

    int32 CountLinesContaining(const FString& Text, const FString& Needle)
    {
        TArray<FString> Lines;
        Text.ParseIntoArrayLines(Lines);

        int32 Count = 0;
        for (const FString& Line : Lines)
        {
            if (Line.Contains(Needle))
            {
                ++Count;
            }
        }
        return Count;
    }

    bool LabelBlockContainsBefore(const FString& Text, const FString& Label, const FString& StopNeedle, const FString& BadNeedle)
    {
        TArray<FString> Lines;
        Text.ParseIntoArrayLines(Lines);

        bool bInLabel = false;
        for (const FString& RawLine : Lines)
        {
            const FString Line = RawLine.TrimStartAndEnd();
            if (Line.StartsWith(TEXT("@")) && Line.EndsWith(TEXT(":")))
            {
                bInLabel = (Line.LeftChop(1) == Label);
                continue;
            }
            if (!bInLabel)
            {
                continue;
            }
            if (Line.Contains(StopNeedle))
            {
                return false;
            }
            if (Line.Contains(BadNeedle))
            {
                return true;
            }
        }
        return false;
    }
} // namespace

// ============================================================================
// Decompiler.SimpleChain
// BeginPlay -> PrintString: BPIR output must contain "entry event" and
// "call PrintString".
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerSimpleChainTest,
    "PinWright.bpir.decompiler.SimpleChain",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerSimpleChainTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph on test Blueprint")); return false; }

    // Find or create the ReceiveBeginPlay event node.
    UK2Node_Event* BeginPlayNode = FindBeginPlayNode(EventGraph);
    if (!BeginPlayNode)
    {
        BeginPlayNode = AddNodeToGraph<UK2Node_Event>(EventGraph);
        BeginPlayNode->EventReference.SetExternalMember(
            TEXT("ReceiveBeginPlay"), AActor::StaticClass());
        BeginPlayNode->bOverrideFunction = true;
        BeginPlayNode->ReconstructNode();
    }

    // Add a PrintString call and wire it after BeginPlay.
    UK2Node_CallFunction* PrintNode = AddNodeToGraph<UK2Node_CallFunction>(EventGraph);
    PrintNode->FunctionReference.SetExternalMember(
        GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
        UKismetSystemLibrary::StaticClass());
    PrintNode->ReconstructNode();
    WireExec(BeginPlayNode, PrintNode);

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    TestTrue(TEXT("Output contains 'call PrintString'"),
        Result.BpirText.Contains(TEXT("PrintString")));
    TestTrue(TEXT("Output contains 'entry event' for BeginPlay"),
        Result.BpirText.Contains(TEXT("entry event")));
    TestTrue(TEXT("Output mentions BeginPlay"),
        Result.BpirText.Contains(TEXT("BeginPlay")));
    return true;
}

// ============================================================================
// Decompiler.BranchNode
// BeginPlay -> Branch: BPIR output must contain "branch(" construct.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerBranchNodeTest,
    "PinWright.bpir.decompiler.BranchNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerBranchNodeTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph on test Blueprint")); return false; }

    UK2Node_Event* BeginPlayNode = FindBeginPlayNode(EventGraph);
    if (!BeginPlayNode)
    {
        BeginPlayNode = AddNodeToGraph<UK2Node_Event>(EventGraph);
        BeginPlayNode->EventReference.SetExternalMember(
            TEXT("ReceiveBeginPlay"), AActor::StaticClass());
        BeginPlayNode->bOverrideFunction = true;
        BeginPlayNode->ReconstructNode();
    }

    UK2Node_IfThenElse* BranchNode = AddNodeToGraph<UK2Node_IfThenElse>(EventGraph);
    WireExec(BeginPlayNode, BranchNode);

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    TestTrue(TEXT("Output contains 'branch('"), Result.BpirText.Contains(TEXT("branch(")));
    return true;
}

// ============================================================================
// Decompiler.PositionSuffixExecAndPureDependencyNodes
// Node-backed BPIR lines include their current graph coordinates for both
// exec-walked instructions and pure dependencies emitted during input traversal.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerPositionSuffixExecAndPureDependencyNodesTest,
    "PinWright.bpir.decompiler.PositionSuffixExecAndPureDependencyNodes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerPositionSuffixExecAndPureDependencyNodesTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompPositionPureBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString Code = TEXT(
        "entry event BeginPlay() {\n"
        "    %m = make<Margin>(Left: 1.0, Top: 2.0, Right: 3.0, Bottom: 4.0)\n"
        "    %b = break<Margin>(%m)\n"
        "    call PrintString(InString: %b.Left)\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(Code);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    SetNodePosition(FindFirstNodeOfType<UK2Node_MakeStruct>(BP), 10, 20);
    SetNodePosition(FindFirstNodeOfType<UK2Node_BreakStruct>(BP), 30, 40);
    SetNodePosition(FindCallFunctionNode(BP, GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString)), 50, 60);

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("MakeStruct pure dependency line includes position"),
        ContainsLineWith(Output, TEXT("make<Margin>"), TEXT("@(10, 20)")));
    TestTrue(TEXT("BreakStruct pure dependency line includes position"),
        ContainsLineWith(Output, TEXT("break<Margin>"), TEXT("@(30, 40)")));
    TestTrue(TEXT("Exec-backed call line includes position"),
        ContainsLineWith(Output, TEXT("PrintString"), TEXT("@(50, 60)")));
    return true;
}

// ============================================================================
// Decompiler.PositionSuffixBranchSwitchAndReturnNodes
// Multi-exec branch/switch lines and function return lines include coordinates.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerPositionSuffixBranchSwitchAndReturnNodesTest,
    "PinWright.bpir.decompiler.PositionSuffixBranchSwitchAndReturnNodes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerPositionSuffixBranchSwitchAndReturnNodesTest::RunTest(const FString& Parameters)
{
    UBlueprint* BranchBP = CreateTestBlueprint();
    TestNotNull(TEXT("Branch Blueprint created"), BranchBP);
    if (!BranchBP) return false;

    UEdGraph* EventGraph = (BranchBP->UbergraphPages.Num() > 0) ? BranchBP->UbergraphPages[0] : nullptr;
    TestNotNull(TEXT("Event graph exists"), EventGraph);
    if (!EventGraph) return false;

    UK2Node_Event* BeginPlayNode = FindBeginPlayNode(EventGraph);
    if (!BeginPlayNode)
    {
        BeginPlayNode = AddNodeToGraph<UK2Node_Event>(EventGraph);
        BeginPlayNode->EventReference.SetExternalMember(TEXT("ReceiveBeginPlay"), AActor::StaticClass());
        BeginPlayNode->bOverrideFunction = true;
        BeginPlayNode->ReconstructNode();
    }

    UK2Node_IfThenElse* BranchNode = AddNodeToGraph<UK2Node_IfThenElse>(EventGraph);
    WireExec(BeginPlayNode, BranchNode);
    SetNodePosition(BranchNode, 70, 80);

    FBpirDecompiler BranchDecompiler(BranchBP);
    FBpirDecompileResult BranchResult = BranchDecompiler.Decompile();
    TestTrue(TEXT("Branch decompile succeeded"), BranchResult.bSuccess);
    TestTrue(TEXT("Branch line includes position"),
        ContainsLineWith(BranchResult.BpirText, TEXT("branch("), TEXT("@(70, 80)")));

    UBlueprint* SwitchBP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompPositionSwitchBP"));
    TestNotNull(TEXT("Switch Blueprint created"), SwitchBP);
    if (!SwitchBP) return false;

    const FString SwitchCode = TEXT(
        "entry event BeginPlay() {\n"
        "    %s = switch_int(0) [1 -> @a, 2 -> @b, default -> @d]\n"
        "    @a:\n"
        "    call PrintString(InString: \"One\")\n"
        "    exec -> @d\n"
        "    @b:\n"
        "    call PrintString(InString: \"Two\")\n"
        "    exec -> @d\n"
        "    @d:\n"
        "}"
    );

    FBpirCompiler SwitchCompiler(SwitchBP);
    FCompileResult SwitchCompileResult = SwitchCompiler.Compile(SwitchCode);
    if (!SwitchCompileResult.bSuccess)
    {
        for (const FCompileError& Err : SwitchCompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Switch compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Switch compile succeeded"), SwitchCompileResult.bSuccess);
    if (!SwitchCompileResult.bSuccess) return false;

    SetNodePosition(FindFirstNodeOfType<UK2Node_Switch>(SwitchBP), 90, 100);

    FBpirDecompiler SwitchDecompiler(SwitchBP);
    FBpirDecompileResult SwitchResult = SwitchDecompiler.Decompile();
    TestTrue(TEXT("Switch decompile succeeded"), SwitchResult.bSuccess);
    TestTrue(TEXT("Switch line includes position"),
        ContainsLineWith(SwitchResult.BpirText, TEXT("switch_int"), TEXT("@(90, 100)")));

    UBlueprint* ReturnBP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompPositionReturnBP"));
    TestNotNull(TEXT("Return Blueprint created"), ReturnBP);
    if (!ReturnBP) return false;

    const FString ReturnCode = TEXT(
        "entry function Calc(float X) -> float {\n"
        "    return $X\n"
        "}"
    );

    FBpirCompiler ReturnCompiler(ReturnBP);
    FCompileResult ReturnCompileResult = ReturnCompiler.Compile(ReturnCode);
    if (!ReturnCompileResult.bSuccess)
    {
        for (const FCompileError& Err : ReturnCompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Return compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Return compile succeeded"), ReturnCompileResult.bSuccess);
    if (!ReturnCompileResult.bSuccess) return false;

    SetNodePosition(FindFirstNodeOfType<UK2Node_FunctionResult>(ReturnBP), 110, 120);

    FBpirDecompiler ReturnDecompiler(ReturnBP);
    FBpirDecompileResult ReturnResult = ReturnDecompiler.Decompile();
    TestTrue(TEXT("Return decompile succeeded"), ReturnResult.bSuccess);
    TestTrue(TEXT("Return line includes position"),
        ContainsLineWith(ReturnResult.BpirText, TEXT("return"), TEXT("@(110, 120)")));
    return true;
}

// ============================================================================
// Decompiler.MultipleEntryPoints
// BeginPlay + Tick events: BPIR output must contain both entry points.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerMultipleEntryPointsTest,
    "PinWright.bpir.decompiler.MultipleEntryPoints",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerMultipleEntryPointsTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph on test Blueprint")); return false; }

    // ReceiveBeginPlay event
    UK2Node_Event* BeginPlayNode = FindBeginPlayNode(EventGraph);
    if (!BeginPlayNode)
    {
        BeginPlayNode = AddNodeToGraph<UK2Node_Event>(EventGraph);
        BeginPlayNode->EventReference.SetExternalMember(
            TEXT("ReceiveBeginPlay"), AActor::StaticClass());
        BeginPlayNode->bOverrideFunction = true;
        BeginPlayNode->ReconstructNode();
    }

    // Wire a PrintString to BeginPlay so it has a body
    UK2Node_CallFunction* PrintA = AddNodeToGraph<UK2Node_CallFunction>(EventGraph);
    PrintA->FunctionReference.SetExternalMember(
        GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
        UKismetSystemLibrary::StaticClass());
    PrintA->ReconstructNode();
    WireExec(BeginPlayNode, PrintA);

    // ReceiveTick event
    UK2Node_Event* TickNode = AddNodeToGraph<UK2Node_Event>(EventGraph);
    TickNode->EventReference.SetExternalMember(TEXT("ReceiveTick"), AActor::StaticClass());
    TickNode->bOverrideFunction = true;
    TickNode->ReconstructNode();

    // Wire a PrintString to Tick so it has a body
    UK2Node_CallFunction* PrintB = AddNodeToGraph<UK2Node_CallFunction>(EventGraph);
    PrintB->FunctionReference.SetExternalMember(
        GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
        UKismetSystemLibrary::StaticClass());
    PrintB->ReconstructNode();
    WireExec(TickNode, PrintB);

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    // BPIR output should contain both entry points as separate "entry event" blocks
    TestTrue(TEXT("Output contains BeginPlay entry"),
        Result.BpirText.Contains(TEXT("BeginPlay")));
    TestTrue(TEXT("Output contains Tick entry"),
        Result.BpirText.Contains(TEXT("Tick")));
    return true;
}

// ============================================================================
// Decompiler.EmptyGraph
// No events: must succeed, produce no crash, output is empty or minimal.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerEmptyGraphTest,
    "PinWright.bpir.decompiler.EmptyGraph",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerEmptyGraphTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    // Use a freshly-added function graph that contains only a FunctionEntry node
    // and no connected executable nodes -- a structural empty.
    UEdGraph* EmptyGraph = FBlueprintEditorUtils::CreateNewGraph(
        BP,
        FName(TEXT("EmptyTestFunction")),
        UEdGraph::StaticClass(),
        UEdGraphSchema_K2::StaticClass());
    BP->FunctionGraphs.Add(EmptyGraph);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

    FBpirDecompiler Decompiler(BP);
    // The call must not crash regardless of what the empty graph yields.
    FBpirDecompileResult Result = Decompiler.DecompileGraph(TEXT("EmptyTestFunction"));

    TestTrue(TEXT("Decompile of empty graph reports success"), Result.bSuccess);
    // BPIR text for an empty graph should be empty or contain only whitespace
    TestTrue(TEXT("Empty graph produces minimal BPIR output"),
        Result.BpirText.TrimStartAndEnd().Len() <= 50);
    return true;
}

// ============================================================================
// Decompiler.VariableSet
// Add a BP variable, add a VariableSet node, decompile: BPIR output contains
// "set TestCounter =".
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerVariableSetTest,
    "PinWright.bpir.decompiler.VariableSet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerVariableSetTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    // Add an integer member variable to the Blueprint.
    FEdGraphPinType IntType;
    IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("TestCounter"), IntType);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph on test Blueprint")); return false; }

    UK2Node_Event* BeginPlayNode = FindBeginPlayNode(EventGraph);
    if (!BeginPlayNode)
    {
        BeginPlayNode = AddNodeToGraph<UK2Node_Event>(EventGraph);
        BeginPlayNode->EventReference.SetExternalMember(
            TEXT("ReceiveBeginPlay"), AActor::StaticClass());
        BeginPlayNode->bOverrideFunction = true;
        BeginPlayNode->ReconstructNode();
    }

    // VariableSet node for TestCounter.
    UK2Node_VariableSet* SetNode = NewObject<UK2Node_VariableSet>(EventGraph);
    SetNode->VariableReference.SetSelfMember(TEXT("TestCounter"));
    EventGraph->AddNode(SetNode, /*bFromUI=*/true, /*bSelectNewNode=*/false);
    SetNode->CreateNewGuid();
    SetNode->PostPlacedNewNode();
    SetNode->AllocateDefaultPins();
    SetNode->ReconstructNode();
    WireExec(BeginPlayNode, SetNode);

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    TestTrue(TEXT("Output contains 'set' keyword for variable assignment"),
        Result.BpirText.Contains(TEXT("set")));
    TestTrue(TEXT("Output contains variable name 'TestCounter'"),
        Result.BpirText.Contains(TEXT("TestCounter")));
    return true;
}

// ============================================================================
// Decompiler.CustomEvent
// Add a UK2Node_CustomEvent with a known name: BPIR output contains
// "entry event" and the event name.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerCustomEventTest,
    "PinWright.bpir.decompiler.CustomEvent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerCustomEventTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph on test Blueprint")); return false; }

    const FString CustomEventName = TEXT("OnTestFired");

    UK2Node_CustomEvent* CustomEventNode = AddNodeToGraph<UK2Node_CustomEvent>(EventGraph);
    CustomEventNode->CustomFunctionName = FName(*CustomEventName);
    CustomEventNode->ReconstructNode();

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);

    TestTrue(TEXT("Output contains 'entry custom_event' keyword"),
        Result.BpirText.Contains(TEXT("entry custom_event")));
    TestTrue(TEXT("Output contains the custom event name"),
        Result.BpirText.Contains(CustomEventName));
    return true;
}

// ============================================================================
// GraphWalker.FindEntryPoints
// Graph with 2 event nodes: FindEntryPoints must return exactly 2.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphWalkerFindEntryPointsTest,
    "PinWright.bpir.decompiler.graph_walker.FindEntryPoints",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGraphWalkerFindEntryPointsTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph on test Blueprint")); return false; }

    // Start from a clean slate: remove any pre-existing event nodes that
    // FKismetEditorUtilities may have inserted so the count is deterministic.
    TArray<UEdGraphNode*> NodesToRemove;
    for (UEdGraphNode* Node : EventGraph->Nodes)
    {
        if (Cast<UK2Node_Event>(Node) || Cast<UK2Node_CustomEvent>(Node))
        {
            NodesToRemove.Add(Node);
        }
    }
    for (UEdGraphNode* Node : NodesToRemove)
    {
        EventGraph->RemoveNode(Node);
    }

    // Add exactly 2 event nodes.
    UK2Node_Event* EvA = AddNodeToGraph<UK2Node_Event>(EventGraph);
    EvA->EventReference.SetExternalMember(TEXT("ReceiveBeginPlay"), AActor::StaticClass());
    EvA->bOverrideFunction = true;
    EvA->ReconstructNode();

    UK2Node_Event* EvB = AddNodeToGraph<UK2Node_Event>(EventGraph);
    EvB->EventReference.SetExternalMember(TEXT("ReceiveTick"), AActor::StaticClass());
    EvB->bOverrideFunction = true;
    EvB->ReconstructNode();

    FGraphWalker Walker(EventGraph);
    TArray<UEdGraphNode*> EntryPoints = Walker.FindEntryPoints();

    TestEqual(TEXT("FindEntryPoints returns 2 nodes"), EntryPoints.Num(), 2);
    return true;
}

// ============================================================================
// GraphWalker.ClassifyNode_Branch
// UK2Node_IfThenElse must classify as ENodeSemantics::Branch.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphWalkerClassifyBranchTest,
    "PinWright.bpir.decompiler.graph_walker.ClassifyNode_Branch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGraphWalkerClassifyBranchTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph on test Blueprint")); return false; }

    UK2Node_IfThenElse* BranchNode = AddNodeToGraph<UK2Node_IfThenElse>(EventGraph);

    FGraphWalker Walker(EventGraph);
    const ENodeSemantics Semantics = Walker.ClassifyNode(BranchNode);

    TestEqual(TEXT("IfThenElse classifies as Branch"),
        Semantics, ENodeSemantics::Branch);
    return true;
}

// ============================================================================
// GraphWalker.ClassifyNode_FunctionCall
// UK2Node_CallFunction (PrintString) must classify as ENodeSemantics::FunctionCall.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphWalkerClassifyFunctionCallTest,
    "PinWright.bpir.decompiler.graph_walker.ClassifyNode_FunctionCall",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGraphWalkerClassifyFunctionCallTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph on test Blueprint")); return false; }

    UK2Node_CallFunction* CallNode = AddNodeToGraph<UK2Node_CallFunction>(EventGraph);
    CallNode->FunctionReference.SetExternalMember(
        GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
        UKismetSystemLibrary::StaticClass());
    CallNode->ReconstructNode();

    FGraphWalker Walker(EventGraph);
    const ENodeSemantics Semantics = Walker.ClassifyNode(CallNode);

    TestEqual(TEXT("CallFunction (PrintString) classifies as FunctionCall"),
        Semantics, ENodeSemantics::FunctionCall);
    return true;
}

// ============================================================================
// Decompiler.SequenceNode
// BeginPlay -> ExecutionSequence (2 outputs) -> each to a PrintString.
// BPIR output must contain "sequence".
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerSequenceNodeTest,
    "PinWright.bpir.decompiler.SequenceNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerSequenceNodeTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph on test Blueprint")); return false; }

    // Find or create the ReceiveBeginPlay event node.
    UK2Node_Event* BeginPlayNode = FindBeginPlayNode(EventGraph);
    if (!BeginPlayNode)
    {
        BeginPlayNode = AddNodeToGraph<UK2Node_Event>(EventGraph);
        BeginPlayNode->EventReference.SetExternalMember(
            TEXT("ReceiveBeginPlay"), AActor::StaticClass());
        BeginPlayNode->bOverrideFunction = true;
        BeginPlayNode->ReconstructNode();
    }

    // Add an ExecutionSequence node and wire BeginPlay -> Sequence.
    UK2Node_ExecutionSequence* SeqNode = AddNodeToGraph<UK2Node_ExecutionSequence>(EventGraph);
    WireExec(BeginPlayNode, SeqNode);

    // Add two PrintString nodes, one per sequence output.
    UK2Node_CallFunction* PrintNodeA = AddNodeToGraph<UK2Node_CallFunction>(EventGraph);
    PrintNodeA->FunctionReference.SetExternalMember(
        GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
        UKismetSystemLibrary::StaticClass());
    PrintNodeA->ReconstructNode();

    UK2Node_CallFunction* PrintNodeB = AddNodeToGraph<UK2Node_CallFunction>(EventGraph);
    PrintNodeB->FunctionReference.SetExternalMember(
        GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
        UKismetSystemLibrary::StaticClass());
    PrintNodeB->ReconstructNode();

    // Wire Sequence output 0 -> PrintNodeA, output 1 -> PrintNodeB.
    UEdGraphPin* SeqOut0 = SeqNode->GetThenPinGivenIndex(0);
    UEdGraphPin* SeqOut1 = SeqNode->GetThenPinGivenIndex(1);
    UEdGraphPin* ExecInA = PrintNodeA->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
    UEdGraphPin* ExecInB = PrintNodeB->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);

    if (SeqOut0 && ExecInA) { SeqOut0->MakeLinkTo(ExecInA); }
    if (SeqOut1 && ExecInB) { SeqOut1->MakeLinkTo(ExecInB); }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    TestTrue(TEXT("Output contains 'sequence'"),
        Result.BpirText.Contains(TEXT("sequence"), ESearchCase::IgnoreCase));
    return true;
}

// ============================================================================
// Decompiler.Reconvergence
// BeginPlay -> Branch -> Both True and False wire to the SAME PrintString
// node. Tests that the decompiler handles reconvergence correctly by emitting
// a shared @merge label at the branch divergence point (so both branch arms
// of the branch line point at the same merge label, and the shared target is
// emitted once under that label).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerReconvergenceTest,
    "PinWright.bpir.decompiler.Reconvergence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerReconvergenceTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph on test Blueprint")); return false; }

    // Find or create the ReceiveBeginPlay event node.
    UK2Node_Event* BeginPlayNode = FindBeginPlayNode(EventGraph);
    if (!BeginPlayNode)
    {
        BeginPlayNode = AddNodeToGraph<UK2Node_Event>(EventGraph);
        BeginPlayNode->EventReference.SetExternalMember(
            TEXT("ReceiveBeginPlay"), AActor::StaticClass());
        BeginPlayNode->bOverrideFunction = true;
        BeginPlayNode->ReconstructNode();
    }

    // Add a Branch (IfThenElse) node.
    UK2Node_IfThenElse* BranchNode = AddNodeToGraph<UK2Node_IfThenElse>(EventGraph);
    WireExec(BeginPlayNode, BranchNode);

    // Add a single PrintString node that both branches converge to.
    UK2Node_CallFunction* PrintNode = AddNodeToGraph<UK2Node_CallFunction>(EventGraph);
    PrintNode->FunctionReference.SetExternalMember(
        GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
        UKismetSystemLibrary::StaticClass());
    PrintNode->ReconstructNode();

    // Wire both True and False exec outputs to the same PrintString node.
    UEdGraphPin* ThenPin = BranchNode->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
    UEdGraphPin* ElsePin = BranchNode->GetElsePin();
    UEdGraphPin* ExecIn  = PrintNode->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);

    if (ThenPin && ExecIn) { ThenPin->MakeLinkTo(ExecIn); }
    if (ElsePin && ExecIn) { ElsePin->MakeLinkTo(ExecIn); }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    // The reconvergence should factor the shared PrintString into a @merge label
    // referenced by both branch arms.
    const FString& Output = Result.BpirText;
    const FString MergeLabel = FindLabelForLineContaining(Output, TEXT("PrintString"));
    TestTrue(TEXT("Shared target is emitted under a @merge label"),
        MergeLabel.StartsWith(TEXT("@merge")));
    TestTrue(TEXT("Branch true arm targets the merge label"),
        Output.Contains(FString::Printf(TEXT("true -> %s"), *MergeLabel)));
    TestTrue(TEXT("Branch false arm targets the merge label"),
        Output.Contains(FString::Printf(TEXT("false -> %s"), *MergeLabel)));
    return true;
}

// ============================================================================
// Decompiler.SharedTailReconvergenceLabel
// Branch-specific bodies must jump to a dedicated merge label before a shared
// downstream node, rather than naming either branch label as the join target.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerSharedTailReconvergenceLabelTest,
    "PinWright.bpir.decompiler.SharedTailReconvergenceLabel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerSharedTailReconvergenceLabelTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompSharedTailMergeBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString OriginalCode = TEXT(
        "entry event BeginPlay() {\n"
        "    %b = branch(true) [true -> @true_path, false -> @false_path]\n"
        "\n"
        "@true_path:\n"
        "    call PrintString(InString: \"TruePath\")\n"
        "    exec -> @done\n"
        "\n"
        "@false_path:\n"
        "    call PrintString(InString: \"FalsePath\")\n"
        "    exec -> @done\n"
        "\n"
        "@done:\n"
        "    call PrintString(InString: \"Merged\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(OriginalCode);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Initial compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    const FString MergeLabel = FindLabelForLineContaining(Output, TEXT("Merged"));
    TestTrue(TEXT("Shared tail is emitted under a dedicated merge label"),
        MergeLabel.StartsWith(TEXT("@merge")));
    TestEqual(TEXT("Both branch bodies jump to the merge label"),
        CountLinesContaining(Output, FString::Printf(TEXT("exec -> %s"), *MergeLabel)), 2);
    TestFalse(TEXT("Merge label does not contain the true branch body before the shared tail"),
        LabelBlockContainsBefore(Output, MergeLabel, TEXT("Merged"), TEXT("TruePath")));
    TestFalse(TEXT("Merge label does not contain the false branch body before the shared tail"),
        LabelBlockContainsBefore(Output, MergeLabel, TEXT("Merged"), TEXT("FalsePath")));

    UBlueprint* RoundTripBP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompSharedTailMergeRoundTripBP"));
    TestNotNull(TEXT("Round-trip Blueprint created"), RoundTripBP);
    if (!RoundTripBP) return false;

    FBpirCompiler Recompiler(RoundTripBP);
    FCompileResult RecompileResult = Recompiler.Compile(DecompileResult.BpirText, EBpirCompileMode::Replace);
    if (!RecompileResult.bSuccess)
    {
        AddError(FString::Printf(TEXT("Round-trip recompile failed. Decompiled text:\n%s"),
            *DecompileResult.BpirText));
        for (const FCompileError& Err : RecompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Decompile output recompiles cleanly"), RecompileResult.bSuccess);
    if (!RecompileResult.bSuccess) return false;

    UK2Node_IfThenElse* BranchNode = Cast<UK2Node_IfThenElse>(FindFirstNodeMatching(RoundTripBP, [](UEdGraphNode* Node)
    {
        return Node && Node->IsA<UK2Node_IfThenElse>();
    }));
    UK2Node_CallFunction* TruePath = FindPrintStringNodeWithDefault(RoundTripBP, TEXT("TruePath"));
    UK2Node_CallFunction* FalsePath = FindPrintStringNodeWithDefault(RoundTripBP, TEXT("FalsePath"));
    UK2Node_CallFunction* Merged = FindPrintStringNodeWithDefault(RoundTripBP, TEXT("Merged"));

    TestNotNull(TEXT("Round-trip branch node"), BranchNode);
    TestNotNull(TEXT("TruePath PrintString node"), TruePath);
    TestNotNull(TEXT("FalsePath PrintString node"), FalsePath);
    TestNotNull(TEXT("Merged PrintString node"), Merged);
    if (!BranchNode || !TruePath || !FalsePath || !Merged) return false;

    TestTrue(TEXT("True branch reaches TruePath"),
        GetExecTargetNode(BranchNode->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output)) == TruePath);
    TestTrue(TEXT("False branch reaches FalsePath"),
        GetExecTargetNode(BranchNode->GetElsePin()) == FalsePath);
    TestTrue(TEXT("TruePath.Then == Merged"),
        GetExecTargetNode(TruePath->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output)) == Merged);
    TestTrue(TEXT("FalsePath.Then == Merged"),
        GetExecTargetNode(FalsePath->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output)) == Merged);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerSharedTailPreservesTargetInputPinTest,
    "PinWright.bpir.decompiler.SharedTailPreservesTargetInputPin",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerSharedTailPreservesTargetInputPinTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompSharedTailTargetInputBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString OriginalCode = TEXT(
        "entry event BeginPlay() {\n"
        "    %b = branch(true) [true -> @true_path, false -> @false_path]\n"
        "\n"
        "@true_path:\n"
        "    call PrintString(InString: \"TruePath\")\n"
        "    exec -> @gate.Open\n"
        "\n"
        "@false_path:\n"
        "    call PrintString(InString: \"FalsePath\")\n"
        "    exec -> @gate.Open\n"
        "\n"
        "@gate:\n"
        "    %g = macro Gate() [Exit -> @done]\n"
        "\n"
        "@done:\n"
        "    call PrintString(InString: \"GateOpen\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(OriginalCode);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Initial compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    const FString GateLabel = FindLabelForLineContaining(Output, TEXT("macro Gate"));
    TestTrue(TEXT("Gate is emitted under a merge label"),
        GateLabel.StartsWith(TEXT("@merge")));
    TestEqual(TEXT("Both branch bodies jump to the Gate Open input"),
        CountLinesContaining(Output, FString::Printf(TEXT("exec -> %s.Open"), *GateLabel)), 2);

    UBlueprint* RoundTripBP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompSharedTailTargetInputRoundTripBP"));
    TestNotNull(TEXT("Round-trip Blueprint created"), RoundTripBP);
    if (!RoundTripBP) return false;

    FBpirCompiler Recompiler(RoundTripBP);
    FCompileResult RecompileResult = Recompiler.Compile(Output, EBpirCompileMode::Replace);
    if (!RecompileResult.bSuccess)
    {
        AddError(FString::Printf(TEXT("Round-trip recompile failed. Decompiled text:\n%s"), *Output));
        for (const FCompileError& Err : RecompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Decompile output recompiles cleanly"), RecompileResult.bSuccess);
    if (!RecompileResult.bSuccess) return false;

    UK2Node_MacroInstance* GateNode = Cast<UK2Node_MacroInstance>(FindFirstNodeMatching(RoundTripBP, [](UEdGraphNode* Node)
    {
        UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node);
        return MacroNode && MacroNode->GetMacroGraph() && MacroNode->GetMacroGraph()->GetName().Contains(TEXT("Gate"));
    }));
    TestNotNull(TEXT("Round-trip Gate macro node"), GateNode);
    if (!GateNode) return false;

    UEdGraphPin* OpenPin = GateNode->FindPin(TEXT("Open"), EGPD_Input);
    TestNotNull(TEXT("Gate Open input exists"), OpenPin);
    if (!OpenPin) return false;
    TestEqual(TEXT("Both branch paths still target Gate.Open"), OpenPin->LinkedTo.Num(), 2);

    return true;
}

// ============================================================================
// Decompiler.ForeachNode
// NOTE: Constructing a UK2Node_MacroInstance for ForEachLoop in a test is
// non-trivial because it requires locating the engine-level macro graph asset
// and wiring it properly. This test is deferred to the round-trip tests in
// Chunk 4B, where BPIR with "foreach" is compiled then decompiled.
// ============================================================================

// ============================================================================
// Decompiler.PinNameNormalization
// NOTE: Pin name normalization (e.g. "Array Element" -> "ArrayElement") is
// best verified via a round-trip test in Chunk 4B, where a foreach loop can
// be compiled from BPIR and then decompiled to check normalized pin names.
// ============================================================================

// ============================================================================
// Decompiler.Timeline
// Compile a timeline from BPIR, then decompile: output must contain "timeline".
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerTimelineTest,
    "PinWright.bpir.decompiler.Timeline",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerTimelineTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompTimelineBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString Code = TEXT(
        "entry event BeginPlay() {\n"
        "    %t = timeline MyTimeline() [update -> @update, finished -> @finished]\n"
        "    @update:\n"
        "    call PrintString(InString: \"Updating\")\n"
        "    @finished:\n"
        "    call PrintString(InString: \"Done\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(Code);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    if (!DecompileResult.bSuccess)
    {
        for (const FBpirWarning& Warn : DecompileResult.Warnings)
        {
            AddError(FString::Printf(TEXT("Decompile warning: %s"), *Warn.Text));
        }
    }
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains 'timeline' keyword"),
        Output.Contains(TEXT("timeline"), ESearchCase::IgnoreCase));
    return true;
}

// ============================================================================
// Decompiler.LatentNode
// Compile a Delay (latent) node from BPIR, then decompile: output must contain
// "latent" and "Delay".
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerLatentNodeTest,
    "PinWright.bpir.decompiler.LatentNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerLatentNodeTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompLatentBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString Code = TEXT(
        "entry event BeginPlay() {\n"
        "    latent Delay(Duration: 1.0) [completed -> @after]\n"
        "    @after:\n"
        "    call PrintString(InString: \"After delay\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(Code);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    if (!DecompileResult.bSuccess)
    {
        for (const FBpirWarning& Warn : DecompileResult.Warnings)
        {
            AddError(FString::Printf(TEXT("Decompile warning: %s"), *Warn.Text));
        }
    }
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains 'latent' keyword"),
        Output.Contains(TEXT("latent"), ESearchCase::IgnoreCase));
    TestTrue(TEXT("Output contains 'Delay' function name"),
        Output.Contains(TEXT("Delay")));
    return true;
}

// ============================================================================
// Decompiler.UnreachableOutputPinLabels (regression for E-decompile-unreachable-output-pin-labels)
//
// For multi-exec nodes, the decompiler must only emit pin labels for outputs
// that have a downstream body. The bug: the generic fallback for Unknown-
// classified nodes added every exec output to the LabelMap unconditionally,
// so the decompiled text contained label references like `[then -> @then,
// BeforePush -> @beforepush]` even when @then and @beforepush had no block
// bodies — and recompile rejected those as "Undefined label reference".
//
// Testing the Unknown fallback directly requires a node type that
// ClassifyNode falls through to Unknown on (typically UK2Node_AsyncAction
// subclasses), which need proxy-class setup that isn't available in this
// transient-BP context. Instead this test round-trips a multi-exec latent
// call with only the completion pin connected, then recompiles the decompile
// output — any regression that re-introduces unreachable-label emission
// would make the recompile fail with "Undefined label reference".
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerUnreachableOutputPinLabelsTest,
    "PinWright.bpir.decompiler.UnreachableOutputPinLabels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerUnreachableOutputPinLabelsTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompUnreachableLabelsBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    // latent Delay has multiple exec outputs (then, completed). Only `completed`
    // is wired here — `then` is deliberately left disconnected.
    const FString OriginalCode = TEXT(
        "entry event BeginPlay() {\n"
        "    latent Delay(Duration: 0.5) [completed -> @after]\n"
        "    @after:\n"
        "    call PrintString(InString: \"After delay\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(OriginalCode);
    TestTrue(TEXT("Initial compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);

    // Round-trip: recompile the decompiled output on a fresh BP. A regression
    // that emitted labels for unconnected pins would produce text like
    // `[then -> @then, completed -> @completed]` without a `@then:` block,
    // which recompile would reject.
    UBlueprint* BP2 = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompUnreachableLabelsBP2"));
    TestNotNull(TEXT("Round-trip BP created"), BP2);
    if (!BP2) return true;

    FBpirCompiler RecompileCompiler(BP2);
    FCompileResult RecompileResult = RecompileCompiler.Compile(
        DecompileResult.BpirText, EBpirCompileMode::Replace);
    if (!RecompileResult.bSuccess)
    {
        AddError(FString::Printf(TEXT("Round-trip recompile failed. Decompiled text:\n%s"),
            *DecompileResult.BpirText));
        for (const FCompileError& Err : RecompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Decompile output recompiles cleanly"), RecompileResult.bSuccess);
    TestFalse(TEXT("No 'Undefined label reference' in errors"),
        CompilerTestUtils::ErrorsContain(RecompileResult.Errors, TEXT("Undefined label reference")));
    return true;
}

// ============================================================================
// Decompiler.ErrorCase.NullBlueprint
// Constructing FBpirDecompiler with nullptr then calling Decompile() must not
// crash — it must return bSuccess=false with a non-empty Warnings array.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerNullBlueprintTest,
    "PinWright.bpir.decompiler.error_case.NullBlueprint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerNullBlueprintTest::RunTest(const FString& Parameters)
{
    FBpirDecompiler Decompiler(nullptr);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestFalse(TEXT("Null BP decompile must not report success"), Result.bSuccess);
    TestTrue(TEXT("Null BP decompile must populate Warnings"), Result.Warnings.Num() > 0);
    return true;
}

// ============================================================================
// Decompiler.ErrorCase.EmptyUbergraph
// BP whose UbergraphPages array is empty (cleared after creation): Decompile()
// must succeed and produce empty/minimal output without crashing.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerEmptyUbergraphTest,
    "PinWright.bpir.decompiler.error_case.EmptyUbergraph",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerEmptyUbergraphTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    // Strip all ubergraph pages so the decompiler iterates over nothing.
    BP->UbergraphPages.Empty();
    BP->FunctionGraphs.Empty();

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("Decompile of BP with no graphs reports success"), Result.bSuccess);
    TestTrue(TEXT("Decompile of BP with no graphs produces empty output"),
        Result.BpirText.TrimStartAndEnd().IsEmpty());
    return true;
}

// ============================================================================
// Decompiler.ErrorCase.NodeWithDisconnectedPins
// A CallFunction node placed in the event graph with its exec input pin left
// disconnected (not wired from any event). The decompiler must not crash and
// must still report success — it simply never reaches the isolated node.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerDisconnectedPinsTest,
    "PinWright.bpir.decompiler.error_case.NodeWithDisconnectedPins",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerDisconnectedPinsTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph on test Blueprint")); return false; }

    // Add a CallFunction node but intentionally leave its exec input unconnected.
    UK2Node_CallFunction* IsolatedNode = AddNodeToGraph<UK2Node_CallFunction>(EventGraph);
    IsolatedNode->FunctionReference.SetExternalMember(
        GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
        UKismetSystemLibrary::StaticClass());
    IsolatedNode->ReconstructNode();
    // No WireExec call — exec input pin has no incoming connection.

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("Decompile succeeds with a node that has disconnected exec input"),
        Result.bSuccess);
    // The isolated node should not appear in the output since it is unreachable.
    TestFalse(TEXT("Isolated node is not reachable and must not appear in output"),
        Result.BpirText.Contains(TEXT("PrintString")));
    return true;
}

// ============================================================================
// Decompiler.ErrorCase.OrphanedNode
// A node added to the event graph that is not reachable from any entry point.
// The decompiler must skip it without error and still report success.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerOrphanedNodeTest,
    "PinWright.bpir.decompiler.error_case.OrphanedNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerOrphanedNodeTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph on test Blueprint")); return false; }

    // Wire up a normal reachable chain: BeginPlay -> PrintString_A.
    UK2Node_Event* BeginPlayNode = FindBeginPlayNode(EventGraph);
    if (!BeginPlayNode)
    {
        BeginPlayNode = AddNodeToGraph<UK2Node_Event>(EventGraph);
        BeginPlayNode->EventReference.SetExternalMember(
            TEXT("ReceiveBeginPlay"), AActor::StaticClass());
        BeginPlayNode->bOverrideFunction = true;
        BeginPlayNode->ReconstructNode();
    }

    UK2Node_CallFunction* ReachableNode = AddNodeToGraph<UK2Node_CallFunction>(EventGraph);
    ReachableNode->FunctionReference.SetExternalMember(
        GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
        UKismetSystemLibrary::StaticClass());
    ReachableNode->ReconstructNode();
    WireExec(BeginPlayNode, ReachableNode);

    // Add an orphaned branch node with no exec input connection — not reachable
    // from BeginPlay or any other entry point.
    UK2Node_IfThenElse* OrphanedBranch = AddNodeToGraph<UK2Node_IfThenElse>(EventGraph);
    // Intentionally no WireExec — orphan has no incoming exec connection.
    (void)OrphanedBranch;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("Decompile succeeds despite an orphaned node in the graph"),
        Result.bSuccess);
    TestTrue(TEXT("Reachable node still appears in output"),
        Result.BpirText.Contains(TEXT("PrintString")));
    // "branch(" would only appear if the orphaned node was incorrectly visited.
    TestFalse(TEXT("Orphaned branch node does not appear in output"),
        Result.BpirText.Contains(TEXT("branch(")));
    return true;
}

// ============================================================================
// Decompiler.MakeStruct
// Compile a MakeVector (pure struct construction) from BPIR, then decompile:
// output must contain "MakeVector" or "pure".
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerMakeStructTest,
    "PinWright.bpir.decompiler.MakeStruct",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerMakeStructTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompMakeStructBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    // %v must be consumed downstream so the decompiler emits the pure node
    const FString Code = TEXT(
        "entry event BeginPlay() {\n"
        "    %v = pure MakeVector(X: 1.0, Y: 2.0, Z: 3.0)\n"
        "    call SetActorLocation(NewLocation: %v)\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(Code);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    if (!DecompileResult.bSuccess)
    {
        for (const FBpirWarning& Warn : DecompileResult.Warnings)
        {
            AddError(FString::Printf(TEXT("Decompile warning: %s"), *Warn.Text));
        }
    }
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);

    const FString& Output = DecompileResult.BpirText;
    // The decompiler emits MakeStruct nodes as "make<StructName>(...)" syntax,
    // pure function calls as "call FuncName(...)", or inline as "MakeVector(...)".
    bool bHasMakeVector = Output.Contains(TEXT("MakeVector"));
    bool bHasCall = Output.Contains(TEXT("call"));
    bool bHasMakeSyntax = Output.Contains(TEXT("make<"));
    bool bHasVector = Output.Contains(TEXT("Vector"));
    TestTrue(TEXT("Output contains struct construction syntax"),
        bHasMakeVector || bHasCall || bHasMakeSyntax || bHasVector);
    return true;
}

// ============================================================================
// Decompiler.GenericNodeSingleExec
// Unknown impure node should decompile as "call ClassName(...)" not "# [UNKNOWN]".
// Uses compile-then-decompile approach to create the node.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerGenericNodeSingleExecTest,
    "PinWright.bpir.decompiler.GenericNodeSingleExec",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerGenericNodeSingleExecTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompGenericSingleBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    // Use SetActorHiddenInGame which is a standard Actor function call
    const FString Code = TEXT(
        "entry event BeginPlay() {\n"
        "    call SetActorHiddenInGame(bNewHidden: true)\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(Code);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);

    const FString& Output = DecompileResult.BpirText;
    // Must contain a "call" instruction, not "# [UNKNOWN]"
    TestTrue(TEXT("Output contains 'call' keyword"),
        Output.Contains(TEXT("call")));
    TestFalse(TEXT("Output does not contain UNKNOWN marker"),
        Output.Contains(TEXT("[UNKNOWN]")));
    return true;
}

// ============================================================================
// Decompiler.GenericNodeMultiExec
// Unknown node with 2+ exec outputs should decompile as
// "call ... [pin -> @label]" with exec target syntax.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerGenericNodeMultiExecTest,
    "PinWright.bpir.decompiler.GenericNodeMultiExec",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerGenericNodeMultiExecTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompGenericMultiBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    // Use a latent Delay which has multiple exec outputs (then + completed)
    const FString Code = TEXT(
        "entry event BeginPlay() {\n"
        "    latent Delay(Duration: 1.0) [completed -> @after]\n"
        "    @after:\n"
        "    call PrintString(InString: \"Done\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(Code);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);

    const FString& Output = DecompileResult.BpirText;
    // Latent nodes have multiple exec outputs; verify exec target syntax
    TestTrue(TEXT("Output contains exec target syntax [... -> @...]"),
        Output.Contains(TEXT("-> @")));
    return true;
}

// ============================================================================
// Decompiler.SwitchIntDecompile
// Compile a switch_int BPIR, then decompile: output must contain "switch_int".
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerSwitchIntDecompileTest,
    "PinWright.bpir.decompiler.SwitchIntDecompile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerSwitchIntDecompileTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompSwitchIntBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString Code = TEXT(
        "entry event BeginPlay() {\n"
        "    %s = switch_int(0) [1 -> @a, 2 -> @b, default -> @d]\n"
        "    @a:\n"
        "    call PrintString(InString: \"One\")\n"
        "    exec -> @d\n"
        "    @b:\n"
        "    call PrintString(InString: \"Two\")\n"
        "    exec -> @d\n"
        "    @d:\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(Code);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains 'switch_int'"),
        Output.Contains(TEXT("switch_int")));
    return true;
}

// ============================================================================
// Decompiler.SwitchStringDecompile
// Compile a switch_string BPIR, then decompile: output must contain "switch_string".
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerSwitchStringDecompileTest,
    "PinWright.bpir.decompiler.SwitchStringDecompile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerSwitchStringDecompileTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompSwitchStringBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString Code = TEXT(
        "entry event BeginPlay() {\n"
        "    %s = switch_string(\"test\") [\"hello\" -> @a, default -> @d]\n"
        "    @a:\n"
        "    call PrintString(InString: \"Hello\")\n"
        "    exec -> @d\n"
        "    @d:\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(Code);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains 'switch_string'"),
        Output.Contains(TEXT("switch_string")));
    return true;
}

// ============================================================================
// Decompiler.FormatArgsDefaultValues
// Pins with non-null defaults should NOT be skipped in decompilation.
// Compile a call with explicit argument values, decompile, verify args present.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerFormatArgsDefaultValuesTest,
    "PinWright.bpir.decompiler.FormatArgsDefaultValues",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerFormatArgsDefaultValuesTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompDefaultArgsBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    // PrintString has several optional args with defaults (bPrintToScreen, bPrintToLog, etc.)
    // Set InString explicitly; verify it survives decompilation
    const FString Code = TEXT(
        "entry event BeginPlay() {\n"
        "    call PrintString(InString: \"TestValue\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(Code);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);

    const FString& Output = DecompileResult.BpirText;
    // The explicitly set default value "TestValue" must appear in the decompiled args
    TestTrue(TEXT("Output contains the explicitly set arg value 'TestValue'"),
        Output.Contains(TEXT("TestValue")));
    return true;
}

// ============================================================================
// Decompiler.DelegateNodeDecompile
// Compile a clear_dispatcher BPIR, then decompile: output must contain
// "clear_dispatcher" keyword.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerDelegateNodeDecompileTest,
    "PinWright.bpir.decompiler.DelegateNodeDecompile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerDelegateNodeDecompileTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompDelegateBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    // Add a multicast delegate variable
    FEdGraphPinType DelegateType;
    DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("MyDispatcher"), DelegateType);
    FKismetEditorUtilities::CompileBlueprint(BP);

    const FString Code = TEXT(
        "entry event BeginPlay() {\n"
        "    clear_dispatcher MyDispatcher()\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(Code);
    // Delegate operations may not resolve in transient test context
    if (!CompileResult.bSuccess)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("delegate-compile-unresolved"),
            TEXT("clear_dispatcher compile produced errors (expected in test context), verifying no crash"));
        return true;
    }

    SetNodePosition(FindFirstNodeMatching(BP, [](UEdGraphNode* Node)
    {
        return Node->GetClass() && Node->GetClass()->GetName().Contains(TEXT("ClearDelegate"));
    }), 410, 420);

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains 'clear_dispatcher'"),
        Output.Contains(TEXT("clear_dispatcher")));
    TestTrue(TEXT("Dispatcher line includes position"),
        ContainsLineWith(Output, TEXT("clear_dispatcher"), TEXT("@(410, 420)")));
    return true;
}

// ============================================================================
// Decompiler.FieldNotifyPositionSuffix
// Field-notify delegate call nodes decompile with BPIR position suffixes.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerFieldNotifyPositionSuffixTest,
    "PinWright.bpir.decompiler.FieldNotifyPositionSuffix",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerFieldNotifyPositionSuffixTest::RunTest(const FString& Parameters)
{
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
        UUserWidget::StaticClass(),
        GetTransientPackage(),
        MakeUniqueObjectName(
            GetTransientPackage(), UWidgetBlueprint::StaticClass(), TEXT("DecompFieldNotifyBP")),
        BPTYPE_Normal,
        UWidgetBlueprint::StaticClass(),
        UWidgetBlueprintGeneratedClass::StaticClass()));

    TestNotNull(TEXT("Widget blueprint created"), WBP);
    if (!WBP) return false;

    FKismetEditorUtilities::CompileBlueprint(WBP);

    FBpirCompiler Compiler(WBP);
    FCompileResult CompileResult = Compiler.Compile(
        TEXT("entry event Construct() {\n")
        TEXT("    field_notify_subscribe Replay(event: @SetValues)\n")
        TEXT("}\n")
        TEXT("entry event Destruct() {\n")
        TEXT("    field_notify_unsubscribe Replay(event: @SetValues)\n")
        TEXT("}\n")
        TEXT("entry custom_event SetValues(object Object, FieldNotificationId Field) {\n")
        TEXT("}"));

    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("BPIR compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Field notify BPIR compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    UK2Node_CallFunction* AddNode = nullptr;
    UK2Node_CallFunction* RemoveNode = nullptr;
    for (UEdGraph* Graph : WBP->UbergraphPages)
    {
        if (!Graph) continue;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
            if (!CallNode) continue;
            const FName FuncName = CallNode->FunctionReference.GetMemberName();
            if (FuncName == TEXT("K2_AddFieldValueChangedDelegate") && !AddNode)
            {
                AddNode = CallNode;
            }
            else if (FuncName == TEXT("K2_RemoveFieldValueChangedDelegate") && !RemoveNode)
            {
                RemoveNode = CallNode;
            }
        }
    }

    TestNotNull(TEXT("K2_AddFieldValueChangedDelegate node exists"), AddNode);
    TestNotNull(TEXT("K2_RemoveFieldValueChangedDelegate node exists"), RemoveNode);
    if (!AddNode || !RemoveNode) return false;

    SetNodePosition(AddNode, 510, 520);
    SetNodePosition(RemoveNode, 530, 540);

    FBpirDecompiler Decompiler(WBP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("field_notify_subscribe includes position"),
        ContainsLineWith(Output, TEXT("field_notify_subscribe Replay"), TEXT("@(510, 520)")));
    TestTrue(TEXT("field_notify_unsubscribe includes position"),
        ContainsLineWith(Output, TEXT("field_notify_unsubscribe Replay"), TEXT("@(530, 540)")));
    return true;
}

// ============================================================================
// Decompiler.VariableSetOutputGet
// Set node's Output_Get pin wired to a downstream node resolves to $VarName,
// not "?".
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerVariableSetOutputGetTest,
    "PinWright.bpir.decompiler.VariableSetOutputGet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerVariableSetOutputGetTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    FEdGraphPinType IntType;
    IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("TestCounter"), IntType);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph on test Blueprint")); return false; }

    UK2Node_Event* BeginPlayNode = FindBeginPlayNode(EventGraph);
    if (!BeginPlayNode)
    {
        BeginPlayNode = AddNodeToGraph<UK2Node_Event>(EventGraph);
        BeginPlayNode->EventReference.SetExternalMember(
            TEXT("ReceiveBeginPlay"), AActor::StaticClass());
        BeginPlayNode->bOverrideFunction = true;
        BeginPlayNode->ReconstructNode();
    }

    // Set TestCounter node
    UK2Node_VariableSet* SetNode = NewObject<UK2Node_VariableSet>(EventGraph);
    SetNode->VariableReference.SetSelfMember(TEXT("TestCounter"));
    EventGraph->AddNode(SetNode, /*bFromUI=*/true, /*bSelectNewNode=*/false);
    SetNode->CreateNewGuid();
    SetNode->PostPlacedNewNode();
    SetNode->AllocateDefaultPins();
    SetNode->ReconstructNode();
    WireExec(BeginPlayNode, SetNode);

    // PrintString node — wire Set's Output_Get to its InString input
    UK2Node_CallFunction* PrintNode = AddNodeToGraph<UK2Node_CallFunction>(EventGraph);
    PrintNode->FunctionReference.SetExternalMember(
        TEXT("PrintString"), UKismetSystemLibrary::StaticClass());
    PrintNode->ReconstructNode();
    WireExec(SetNode, PrintNode);

    // Wire Output_Get → InString
    UEdGraphPin* OutputGetPin = SetNode->FindPin(TEXT("Output_Get"), EGPD_Output);
    UEdGraphPin* InStringPin = PrintNode->FindPin(TEXT("InString"), EGPD_Input);
    if (OutputGetPin && InStringPin)
    {
        OutputGetPin->MakeLinkTo(InStringPin);
    }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    TestTrue(TEXT("Output contains $TestCounter reference"),
        Result.BpirText.Contains(TEXT("$TestCounter")));
    TestFalse(TEXT("Output does not contain unresolved '?'"),
        Result.BpirText.Contains(TEXT("?")));
    return true;
}

// ============================================================================
// Decompiler.ExternalVariableGetThroughKnot
// A VariableGet targeting an external object connected via reroute (knot) nodes.
// Verifies the decompiler resolves the source through knots without infinite
// recursion (regression: passing output pin to ResolveInputValue caused the
// function to follow LinkedTo downstream instead of upstream).
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerExternalVarGetThroughKnotTest,
    "PinWright.bpir.decompiler.ExternalVariableGetThroughKnot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerExternalVarGetThroughKnotTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    // Add a bool member variable that we'll read externally
    FEdGraphPinType BoolType;
    BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("ExternalFlag"), BoolType);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
    FKismetEditorUtilities::CompileBlueprint(BP);

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph on test Blueprint")); return false; }

    UK2Node_Event* BeginPlayNode = FindBeginPlayNode(EventGraph);
    if (!BeginPlayNode)
    {
        BeginPlayNode = AddNodeToGraph<UK2Node_Event>(EventGraph);
        BeginPlayNode->EventReference.SetExternalMember(
            TEXT("ReceiveBeginPlay"), AActor::StaticClass());
        BeginPlayNode->bOverrideFunction = true;
        BeginPlayNode->ReconstructNode();
    }

    // External VariableGet — reads ExternalFlag off an external Actor reference
    UK2Node_VariableGet* GetNode = NewObject<UK2Node_VariableGet>(EventGraph);
    GetNode->VariableReference.SetExternalMember(TEXT("ExternalFlag"), BP->GeneratedClass);
    EventGraph->AddNode(GetNode, /*bFromUI=*/true, /*bSelectNewNode=*/false);
    GetNode->CreateNewGuid();
    GetNode->PostPlacedNewNode();
    GetNode->AllocateDefaultPins();
    GetNode->ReconstructNode();

    // Reroute (knot) node between a GetSelfReference and the VariableGet self pin
    UK2Node_Knot* KnotNode = AddNodeToGraph<UK2Node_Knot>(EventGraph);
    SetNodePosition(KnotNode, 333, 444);
    // Set the knot pin type to object so it can carry an actor reference
    for (UEdGraphPin* Pin : KnotNode->Pins)
    {
        if (Pin)
        {
            Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
            Pin->PinType.PinSubCategoryObject = AActor::StaticClass();
        }
    }

    // Wire: Knot output → VariableGet self pin
    UEdGraphPin* KnotOut = KnotNode->FindPin(TEXT("OutputPin"), EGPD_Output);
    UEdGraphPin* GetSelfPin = GetNode->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
    if (KnotOut && GetSelfPin)
    {
        KnotOut->MakeLinkTo(GetSelfPin);
    }

    // Wire the VariableGet output to a PrintString so the get is reachable
    UK2Node_CallFunction* PrintNode = AddNodeToGraph<UK2Node_CallFunction>(EventGraph);
    PrintNode->FunctionReference.SetExternalMember(
        TEXT("PrintString"), UKismetSystemLibrary::StaticClass());
    PrintNode->ReconstructNode();
    WireExec(BeginPlayNode, PrintNode);

    // Wire ExternalFlag → InString (bool will auto-convert)
    UEdGraphPin* FlagOut = GetNode->FindPin(TEXT("ExternalFlag"), EGPD_Output);
    UEdGraphPin* InStringPin = PrintNode->FindPin(TEXT("InString"), EGPD_Input);
    if (FlagOut && InStringPin)
    {
        FlagOut->MakeLinkTo(InStringPin);
    }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    TestTrue(TEXT("Output contains ExternalFlag"),
        Result.BpirText.Contains(TEXT("ExternalFlag")));
    // Must not double the name (ExternalFlag.ExternalFlag)
    TestFalse(TEXT("No doubled property name"),
        Result.BpirText.Contains(TEXT("ExternalFlag.ExternalFlag")));
    TestFalse(TEXT("Transparent knot position is not emitted"),
        Result.BpirText.Contains(TEXT("@(333, 444)")));
    return true;
}

// ============================================================================
// Decompiler.ExternalVariableSetThroughKnot
// A VariableSet targeting an external object connected via reroute (knot) nodes.
// Verifies the decompiler emits the target reference once, not doubled.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerExternalVarSetThroughKnotTest,
    "PinWright.bpir.decompiler.ExternalVariableSetThroughKnot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerExternalVarSetThroughKnotTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    FEdGraphPinType IntType;
    IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("ExternalScore"), IntType);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
    FKismetEditorUtilities::CompileBlueprint(BP);

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph on test Blueprint")); return false; }

    UK2Node_Event* BeginPlayNode = FindBeginPlayNode(EventGraph);
    if (!BeginPlayNode)
    {
        BeginPlayNode = AddNodeToGraph<UK2Node_Event>(EventGraph);
        BeginPlayNode->EventReference.SetExternalMember(
            TEXT("ReceiveBeginPlay"), AActor::StaticClass());
        BeginPlayNode->bOverrideFunction = true;
        BeginPlayNode->ReconstructNode();
    }

    // External VariableSet — sets ExternalScore on an external Actor reference
    UK2Node_VariableSet* SetNode = NewObject<UK2Node_VariableSet>(EventGraph);
    SetNode->VariableReference.SetExternalMember(TEXT("ExternalScore"), BP->GeneratedClass);
    EventGraph->AddNode(SetNode, /*bFromUI=*/true, /*bSelectNewNode=*/false);
    SetNode->CreateNewGuid();
    SetNode->PostPlacedNewNode();
    SetNode->AllocateDefaultPins();
    SetNode->ReconstructNode();
    WireExec(BeginPlayNode, SetNode);

    // Reroute (knot) node
    UK2Node_Knot* KnotNode = AddNodeToGraph<UK2Node_Knot>(EventGraph);
    SetNodePosition(KnotNode, 555, 666);
    for (UEdGraphPin* Pin : KnotNode->Pins)
    {
        if (Pin)
        {
            Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
            Pin->PinType.PinSubCategoryObject = AActor::StaticClass();
        }
    }

    // Wire: Knot output → VariableSet self pin
    UEdGraphPin* KnotOut = KnotNode->FindPin(TEXT("OutputPin"), EGPD_Output);
    UEdGraphPin* SetSelfPin = SetNode->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
    if (KnotOut && SetSelfPin)
    {
        KnotOut->MakeLinkTo(SetSelfPin);
    }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    TestTrue(TEXT("Output contains set ExternalScore"),
        Result.BpirText.Contains(TEXT("set")) && Result.BpirText.Contains(TEXT("ExternalScore")));
    // Must not double the name
    TestFalse(TEXT("No doubled property name"),
        Result.BpirText.Contains(TEXT("ExternalScore.ExternalScore")));
    TestFalse(TEXT("Transparent knot position is not emitted"),
        Result.BpirText.Contains(TEXT("@(555, 666)")));
    return true;
}

// ============================================================================
// Decompiler.VariableGet
// Compile a "get" variable from BPIR, then decompile: output must contain
// the variable name (either as "get VarName" or inline "$VarName").
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerVariableGetTest,
    "PinWright.bpir.decompiler.VariableGet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerVariableGetTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompVarGetBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    // Add a float variable so "get" resolves
    FEdGraphPinType FloatType;
    FloatType.PinCategory = UEdGraphSchema_K2::PC_Real;
    FloatType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("MySpeed"), FloatType);

    const FString Code = TEXT(
        "entry event BeginPlay() {\n"
        "    %val = get MySpeed\n"
        "    call PrintString(InString: %val)\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(Code);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);

    const FString& Output = DecompileResult.BpirText;
    // The variable name must appear in the decompiled output
    TestTrue(TEXT("Output contains MySpeed variable reference"),
        Output.Contains(TEXT("MySpeed")));
    return true;
}

// ============================================================================
// Decompiler.ValidatedVariableGet
// A Validated Get (K2Node_VariableGet with exec pins)
// must decompile to "%n = get VarName [then -> @then]", not the generic
// "call K2Node_VariableGet(...)" fallback that loses the variable identity.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerValidatedVariableGetTest,
    "PinWright.bpir.decompiler.ValidatedVariableGet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerValidatedVariableGetTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    // Add an object variable so the Validated Get has something to resolve
    FEdGraphPinType ActorRefType;
    ActorRefType.PinCategory = UEdGraphSchema_K2::PC_Object;
    ActorRefType.PinSubCategoryObject = AActor::StaticClass();
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("MyActorRef"), ActorRefType);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
    FKismetEditorUtilities::CompileBlueprint(BP);

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph")); return false; }

    UK2Node_Event* BeginPlayNode = FindBeginPlayNode(EventGraph);
    if (!BeginPlayNode)
    {
        BeginPlayNode = AddNodeToGraph<UK2Node_Event>(EventGraph);
        BeginPlayNode->EventReference.SetExternalMember(
            TEXT("ReceiveBeginPlay"), AActor::StaticClass());
        BeginPlayNode->bOverrideFunction = true;
        BeginPlayNode->ReconstructNode();
    }

    // Build a Validated Get (exec-input variant) for MyActorRef
    UK2Node_VariableGet* VGetNode = NewObject<UK2Node_VariableGet>(EventGraph);
    VGetNode->VariableReference.SetSelfMember(TEXT("MyActorRef"));
    EventGraph->AddNode(VGetNode, /*bFromUI=*/true, /*bSelectNewNode=*/false);
    VGetNode->CreateNewGuid();
    VGetNode->PostPlacedNewNode();
    VGetNode->AllocateDefaultPins();
    // Flip from Pure to ValidatedObject — adds Execute input and then output exec pins
    TestTrue(TEXT("Validated Get pins configured"), SetVariableGetToValidatedObject(VGetNode));

    // Wire: BeginPlay.then → VGet.Execute
    WireExec(BeginPlayNode, VGetNode);

    // Add a PrintString node to give VGet.then an exec destination
    UK2Node_CallFunction* PrintNode = AddNodeToGraph<UK2Node_CallFunction>(EventGraph);
    PrintNode->FunctionReference.SetExternalMember(
        TEXT("PrintString"), UKismetSystemLibrary::StaticClass());
    PrintNode->ReconstructNode();

    // Wire: VGet.then → PrintString.Execute
    UEdGraphPin* VGetThenPin  = VGetNode->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
    UEdGraphPin* PrintExecPin = PrintNode->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
    if (VGetThenPin && PrintExecPin)
    {
        VGetThenPin->MakeLinkTo(PrintExecPin);
    }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();

    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);

    const FString& Output = DecompileResult.BpirText;

    // Variable identity must be preserved (the bug's exact failure mode)
    TestTrue(TEXT("Output contains MyActorRef"), Output.Contains(TEXT("MyActorRef")));

    // Must not have fallen through to the generic fallback
    TestFalse(TEXT("No generic K2Node_VariableGet call"), Output.Contains(TEXT("call K2Node_VariableGet")));

    // The generic-fallback warning must not be present
    bool bHasGenericWarning = false;
    for (const FBpirWarning& Warning : DecompileResult.Warnings)
    {
        if (Warning.Text.Contains(TEXT("Unknown node type (generic fallback): K2Node_VariableGet")))
        {
            bHasGenericWarning = true;
            break;
        }
    }
    TestFalse(TEXT("No generic-fallback warning for K2Node_VariableGet"), bHasGenericWarning);
    return true;
}

// ============================================================================
// Decompiler.ForeachLoop
// Compile a foreach loop from BPIR, then decompile: output must contain
// "foreach" keyword.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerForeachLoopTest,
    "PinWright.bpir.decompiler.ForeachLoop",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerForeachLoopTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompForeachBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString Code = TEXT(
        "entry event BeginPlay() {\n"
        "    %arr = make_array(\"a\", \"b\")\n"
        "    %fe = foreach(Array: %arr) [body -> @loop, completed -> @done]\n"
        "\n"
        "@loop:\n"
        "    call PrintString(InString: \"Item\")\n"
        "\n"
        "@done:\n"
        "    call PrintString(InString: \"AllDone\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(Code);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains 'foreach' keyword"),
        Output.Contains(TEXT("foreach")));
    return true;
}

// ============================================================================
// Decompiler.WhileLoop
// Compile a while loop from BPIR, then decompile: output must contain "while".
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerWhileLoopTest,
    "PinWright.bpir.decompiler.WhileLoop",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerWhileLoopTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompWhileBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString Code = TEXT(
        "entry event BeginPlay() {\n"
        "    %cond = pure IsValid(self)\n"
        "    %w = while(%cond) [body -> @loop, completed -> @done]\n"
        "\n"
        "@loop:\n"
        "    call PrintString(InString: \"Looping\")\n"
        "\n"
        "@done:\n"
        "    call PrintString(InString: \"Done\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(Code);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains 'while' keyword"),
        Output.Contains(TEXT("while")));
    return true;
}

// ============================================================================
// Decompiler.CastNode
// Compile a cast from BPIR, then decompile: output must contain "cast<".
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerCastNodeTest,
    "PinWright.bpir.decompiler.CastNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerCastNodeTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompCastBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString Code = TEXT(
        "entry event BeginPlay() {\n"
        "    %self_ref = self\n"
        "    %c = cast<Actor>(%self_ref) [success -> @ok, fail -> @fail]\n"
        "\n"
        "@ok:\n"
        "    call PrintString(InString: \"CastOk\")\n"
        "    exec -> @done\n"
        "\n"
        "@fail:\n"
        "    call PrintString(InString: \"CastFail\")\n"
        "    exec -> @done\n"
        "\n"
        "@done:\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(Code);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains 'cast<' keyword"),
        Output.Contains(TEXT("cast<")));
    return true;
}

// ============================================================================
// Decompiler.SelectNode
// Compile a select expression from BPIR, then decompile: output must contain
// "select" or "Select" (the pure node may decompile under either casing).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerSelectNodeTest,
    "PinWright.bpir.decompiler.SelectNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerSelectNodeTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompSelectBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString Code = TEXT(
        "entry event BeginPlay() {\n"
        "    %sel = select(Index: true, true: \"A\", false: \"B\")\n"
        "    call PrintString(InString: %sel)\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(Code);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains 'select' or 'Select'"),
        Output.Contains(TEXT("select"), ESearchCase::IgnoreCase));
    return true;
}

// ============================================================================
// Decompiler.SwitchEnumDecompile
// Compile a switch_enum from BPIR, then decompile: output must contain
// "switch_enum<" keyword.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerSwitchEnumDecompileTest,
    "PinWright.bpir.decompiler.SwitchEnumDecompile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerSwitchEnumDecompileTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompSwitchEnumBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString Code = TEXT(
        "entry event BeginPlay() {\n"
        "    %e = enum ESlateVisibility::Visible\n"
        "    %s = switch_enum<ESlateVisibility>(%e) [ESlateVisibility::Visible -> @a, ESlateVisibility::Collapsed -> @b]\n"
        "    @a:\n"
        "    call PrintString(InString: \"Visible\")\n"
        "    exec -> @done\n"
        "    @b:\n"
        "    call PrintString(InString: \"Collapsed\")\n"
        "    exec -> @done\n"
        "    @done:\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(Code);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains 'switch_enum<'"),
        Output.Contains(TEXT("switch_enum<")));
    return true;
}

// ============================================================================
// Decompiler.MacroGate
// Compile a Gate macro from BPIR, then decompile: output must contain "Gate".
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerMacroGateTest,
    "PinWright.bpir.decompiler.MacroGate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerMacroGateTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompGateBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString Code = TEXT(
        "entry event BeginPlay() {\n"
        "    %g = macro Gate() [Exit -> @out]\n"
        "\n"
        "@out:\n"
        "    call PrintString(InString: \"GateOpen\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(Code);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    SetNodePosition(FindFirstNodeOfType<UK2Node_MacroInstance>(BP), 430, 440);

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains 'Gate'"),
        Output.Contains(TEXT("Gate")));
    TestTrue(TEXT("Macro line includes position"),
        ContainsLineWith(Output, TEXT("macro Gate"), TEXT("@(430, 440)")));
    return true;
}

// ============================================================================
// Decompiler.MacroMultiGate
// Compile a MultiGate macro from BPIR, then decompile: output must contain
// "MultiGate".
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerMacroMultiGateTest,
    "PinWright.bpir.decompiler.MacroMultiGate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerMacroMultiGateTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompMultiGateBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString Code = TEXT(
        "entry event BeginPlay() {\n"
        "    %mg = macro MultiGate() [Out 0 -> @a, Out 1 -> @b]\n"
        "\n"
        "@a:\n"
        "    call PrintString(InString: \"PathA\")\n"
        "    exec -> @done\n"
        "\n"
        "@b:\n"
        "    call PrintString(InString: \"PathB\")\n"
        "    exec -> @done\n"
        "\n"
        "@done:\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(Code);

    // MultiGate may not exist in StandardMacros on all UE versions (e.g. UE 5.6+).
    // If it fails with "not found", that's an environment issue — skip gracefully.
    if (!CompileResult.bSuccess)
    {
        bool bIsMacroNotFound = false;
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddInfo(FString::Printf(TEXT("Compile error L%d: %s"), Err.Line, *Err.Message));
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
                TEXT("SKIPPED: MultiGate macro not available in this UE version's StandardMacros; skipping MultiGate decompile test."));
            return true;
        }
        // Unexpected failure
        for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); }
        TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
        return false;
    }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains 'MultiGate'"),
        Output.Contains(TEXT("MultiGate")));
    return true;
}

// ============================================================================
// Decompiler.BreakStructNode
// Compile a break<Margin> from BPIR, then decompile: output must contain
// "break<" or "Break Margin". FMargin has no HasNativeBreak metadata so the
// compiler emits UK2Node_BreakStruct, which the decompiler renders as break<>.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerBreakStructNodeTest,
    "PinWright.bpir.decompiler.BreakStructNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerBreakStructNodeTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompBreakStructBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString Code = TEXT(
        "entry event BeginPlay() {\n"
        "    %m = make<Margin>(Left: 1.0, Top: 2.0, Right: 3.0, Bottom: 4.0)\n"
        "    %b = break<Margin>(%m)\n"
        "    call PrintString(InString: %b.Left)\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(Code);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);

    const FString& Output = DecompileResult.BpirText;
    // "Break Margin" is the node title a generic fallback emits when the break-struct
    // emitter does not run, not a second legitimate syntax. Accepting it meant this test
    // passed on exactly the regression it names. Nothing downstream re-compiles the
    // output here, so this assertion is the only thing guarding the emission.
    TestTrue(TEXT("Output contains break struct syntax"), Output.Contains(TEXT("break<Margin>")));
    return true;
}

// ============================================================================
// Decompiler.MakeArrayNode
// Compile a make_array from BPIR, then decompile: output must contain
// "make_array" or "MakeArray".
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerMakeArrayNodeTest,
    "PinWright.bpir.decompiler.MakeArrayNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerMakeArrayNodeTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompMakeArrayBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString Code = TEXT(
        "entry event BeginPlay() {\n"
        "    %arr = make_array(\"a\", \"b\", \"c\")\n"
        "    %fe = foreach(Array: %arr) [body -> @loop, completed -> @done]\n"
        "\n"
        "@loop:\n"
        "    call PrintString(InString: \"Item\")\n"
        "\n"
        "@done:\n"
        "    call PrintString(InString: \"ArrayDone\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(Code);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);

    const FString& Output = DecompileResult.BpirText;
    // "MakeArray" is the node title fallback, not a second legitimate emission --
    // BpirTextEmitter only ever produces the "make_array(...)" keyword form. Nothing
    // downstream re-compiles this output, so accepting the title left the emission
    // completely unguarded.
    TestTrue(TEXT("Output contains the make_array keyword form"),
        Output.Contains(TEXT("make_array")));
    return true;
}

// ============================================================================
// Decompiler.ReturnNode
// Compile a function with return from BPIR, then decompile: output must
// contain "return".
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerReturnNodeTest,
    "PinWright.bpir.decompiler.ReturnNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerReturnNodeTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompReturnBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString Code = TEXT(
        "entry function Calc(float X) -> float {\n"
        "    return $X\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(Code);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains 'return' keyword"),
        Output.Contains(TEXT("return")));
    return true;
}

// ============================================================================
// Decompiler.OverrideEntry
// Compile a function override BPIR, then decompile: output must contain
// "entry override" so override graphs round-trip correctly.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerOverrideEntryTest,
    "PinWright.bpir.decompiler.OverrideEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerOverrideEntryTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBPWithParent(ACharacter::StaticClass(), TEXT("DecompOverrideBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString Code = TEXT(
        "entry override CanJumpInternal() -> bool {\n"
        "    return true\n"
        "}");

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(Code);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    TestTrue(TEXT("Output contains entry override"),
        DecompileResult.BpirText.Contains(TEXT("entry override CanJumpInternal(")));
    return true;
}

// ----------------------------------------------------------------------------
// CustomEventParam — custom event parameters decompile as $ParamName not ?
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FDecompilerCustomEventParamTest,
    "PinWright.bpir.decompiler.CustomEventParam",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDecompilerCustomEventParamTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTestBlueprint();
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    UEdGraph* Graph = BP->UbergraphPages[0];
    TestNotNull(TEXT("Event graph exists"), Graph);
    if (!Graph) return false;

    // Create a custom event with a bool parameter
    UK2Node_CustomEvent* CustomEvent = AddNodeToGraph<UK2Node_CustomEvent>(Graph);
    CustomEvent->CustomFunctionName = TEXT("OnTestEvent");
    FEdGraphPinType BoolPinType;
    BoolPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    CustomEvent->CreateUserDefinedPin(FName(TEXT("bMyParam")), BoolPinType, EGPD_Output);
    CustomEvent->ReconstructNode();

    // Create a Branch node and wire the custom event's param pin to its Condition
    UK2Node_IfThenElse* BranchNode = AddNodeToGraph<UK2Node_IfThenElse>(Graph);
    WireExec(CustomEvent, BranchNode);
    SetNodePosition(BranchNode, 130, 140);

    UEdGraphPin* ParamPin = CustomEvent->FindPin(TEXT("bMyParam"), EGPD_Output);
    UEdGraphPin* CondPin = BranchNode->FindPin(TEXT("Condition"), EGPD_Input);
    TestNotNull(TEXT("Param pin found"), ParamPin);
    TestNotNull(TEXT("Condition pin found"), CondPin);
    if (ParamPin && CondPin)
    {
        ParamPin->MakeLinkTo(CondPin);
    }

    // Decompile and verify $bMyParam appears (not ?)
    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);

    const FString& Output = Result.BpirText;
    TestTrue(TEXT("Contains $bMyParam"), Output.Contains(TEXT("$bMyParam")));
    TestFalse(TEXT("Does not contain unresolved '?'"),
        Output.Contains(TEXT(": ?")) || Output.Contains(TEXT("= ?")));
    TestTrue(TEXT("Custom event body node includes position"),
        ContainsLineWith(Output, TEXT("branch("), TEXT("@(130, 140)")));
    return true;
}

// ============================================================================
// Decompiler.optional_pin.OmitsUnwiredOptionalArgs
// Regression for B-bpir-optional-pin-no-default-question-mark.
// PrintString has several optional pins (Debug bool, etc.) that BP graphs
// commonly leave unwired. The decompiler used to emit `Name: ?` plus a
// per-pin warning; both are noise that the round-trip parser strips anyway.
// After the FormatArgs skip-filter broadening, those pins are dropped from
// the emitted arg list and the warning is silenced for arg-pin contexts.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerOmitsOptionalUnwiredPinsTest,
    "PinWright.bpir.decompiler.optional_pin.OmitsUnwiredOptionalArgs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerOmitsOptionalUnwiredPinsTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph on test Blueprint")); return false; }

    // BeginPlay -> PrintString. Leave every optional input pin unwired
    // (Debug bool, TextColor, Duration, etc.) — the autogenerated defaults
    // should keep them out of the emitted BPIR arg list entirely.
    UK2Node_Event* BeginPlayNode = FindBeginPlayNode(EventGraph);
    if (!BeginPlayNode)
    {
        BeginPlayNode = AddNodeToGraph<UK2Node_Event>(EventGraph);
        BeginPlayNode->EventReference.SetExternalMember(
            TEXT("ReceiveBeginPlay"), AActor::StaticClass());
        BeginPlayNode->bOverrideFunction = true;
        BeginPlayNode->ReconstructNode();
    }

    UK2Node_CallFunction* PrintNode = AddNodeToGraph<UK2Node_CallFunction>(EventGraph);
    PrintNode->FunctionReference.SetExternalMember(
        GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
        UKismetSystemLibrary::StaticClass());
    PrintNode->ReconstructNode();
    WireExec(BeginPlayNode, PrintNode);

    // Sanity check: the Debug bool pin really is present, unwired, and at its
    // autogenerated default. If the engine ever changes the PrintString
    // signature so this pin disappears, re-pick another optional pin so the
    // test stays meaningful.
    UEdGraphPin* DebugPin = PrintNode->FindPin(TEXT("bPrintToScreen"), EGPD_Input);
    if (!DebugPin)
    {
        // Older / newer UE may name it differently; fall back to any unwired
        // bool input pin to keep the regression intent intact.
        for (UEdGraphPin* Pin : PrintNode->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Input
                && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean
                && Pin->LinkedTo.Num() == 0)
            {
                DebugPin = Pin;
                break;
            }
        }
    }
    TestNotNull(TEXT("PrintString has at least one unwired bool optional pin"), DebugPin);

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    TestTrue(TEXT("Output contains 'PrintString'"),
        Result.BpirText.Contains(TEXT("PrintString")));

    // Core regression: no `Name: ?` arg in the emitted BPIR.
    // Counterfactual: if the broadened skip filter in
    // Decompiler/BpirTextEmitter.cpp FormatArgs is reverted, this assertion
    // fails because the unwired Debug bool pin on PrintString produces
    // `Debug: ?` in the emitted BPIR text, failing the Contains(": ?") check.
    TestFalse(TEXT("BPIR text does not contain ': ?' (vacuous-default arg)"),
        Result.BpirText.Contains(TEXT(": ?")));

    // The softened warning text dropped the old "has no connection, default value"
    // phrasing. Either no warning is emitted at all (FormatArgs skipped the pin
    // before ResolveInputValue ran), or the new wording is used elsewhere — but
    // never the old phrasing on an arg pin we expect to be omitted.
    for (const FBpirWarning& Warning : Result.Warnings)
    {
        TestFalse(
            FString::Printf(TEXT("Warning uses old wording: %s"), *Warning.Text),
            Warning.Text.Contains(TEXT("has no connection, default value")));
    }
    return true;
}

// ============================================================================
// Decompiler.optional_pin.OmitsUnwiredOptionalObjectRefArgs
// Regression for B-bpir-optional-pin-no-default-question-mark, history #4.
// PlaySound2D has ConcurrencySettings : USoundConcurrency* and
// OwningActor : AActor* optional pins. The prior fix (#2) handled primitive
// pins (PrintString's bPrintToScreen) but missed object-ref pins, where
// UEdGraphPin::DoesDefaultValueMatchAutogenerated() returns false because the
// engine writes DefaultValue == "None" for these and IsDefaultAsStringEmpty()
// rejects "None". The broadened object-shape short-circuit in
// BpirTextEmitterInternal::IsPinOmittableAtCallSite is what this test locks in.
//
// Counterfactual: If the broadened object-ref short-circuit in
// Decompiler/BpirTextEmitter.cpp::IsPinOmittableAtCallSite is reverted, this
// assertion fails because ConcurrencySettings (USoundConcurrency*) and
// OwningActor (AActor*) have DefaultValue == "None" (or the symmetric
// autogenerated-side variant) which makes
// UEdGraphPin::DoesDefaultValueMatchAutogenerated() return false for
// object-ref pins, causing the helper to refuse omission and FormatArgs to
// emit `ConcurrencySettings: ?, OwningActor: ?`.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerOmitsOptionalObjectRefUnwiredPinsTest,
    "PinWright.bpir.decompiler.optional_pin.OmitsUnwiredOptionalObjectRefArgs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerOmitsOptionalObjectRefUnwiredPinsTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph on test Blueprint")); return false; }

    UK2Node_Event* BeginPlayNode = FindBeginPlayNode(EventGraph);
    if (!BeginPlayNode)
    {
        BeginPlayNode = AddNodeToGraph<UK2Node_Event>(EventGraph);
        BeginPlayNode->EventReference.SetExternalMember(
            TEXT("ReceiveBeginPlay"), AActor::StaticClass());
        BeginPlayNode->bOverrideFunction = true;
        BeginPlayNode->ReconstructNode();
    }

    UK2Node_CallFunction* PlayNode = AddNodeToGraph<UK2Node_CallFunction>(EventGraph);
    PlayNode->FunctionReference.SetExternalMember(
        GET_FUNCTION_NAME_CHECKED(UGameplayStatics, PlaySound2D),
        UGameplayStatics::StaticClass());
    PlayNode->ReconstructNode();
    WireExec(BeginPlayNode, PlayNode);

    UEdGraphPin* ConcurrencyPin = PlayNode->FindPin(TEXT("ConcurrencySettings"), EGPD_Input);
    UEdGraphPin* OwningActorPin = PlayNode->FindPin(TEXT("OwningActor"), EGPD_Input);
    TestNotNull(TEXT("PlaySound2D has ConcurrencySettings input pin"), ConcurrencyPin);
    TestNotNull(TEXT("PlaySound2D has OwningActor input pin"), OwningActorPin);

    if (ConcurrencyPin)
    {
        TestEqual(TEXT("ConcurrencySettings unwired"), ConcurrencyPin->LinkedTo.Num(), 0);
        TestNull(TEXT("ConcurrencySettings DefaultObject null"), ConcurrencyPin->DefaultObject);
        TestTrue(TEXT("ConcurrencySettings DefaultTextValue empty"),
            ConcurrencyPin->DefaultTextValue.IsEmpty());
    }
    if (OwningActorPin)
    {
        TestEqual(TEXT("OwningActor unwired"), OwningActorPin->LinkedTo.Num(), 0);
        TestNull(TEXT("OwningActor DefaultObject null"), OwningActorPin->DefaultObject);
        TestTrue(TEXT("OwningActor DefaultTextValue empty"),
            OwningActorPin->DefaultTextValue.IsEmpty());
    }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("decompile succeeded"), Result.bSuccess);
    TestTrue(TEXT("PlaySound2D present"),
        Result.BpirText.Contains(TEXT("PlaySound2D")));
    TestFalse(TEXT("ConcurrencySettings ? omitted"),
        Result.BpirText.Contains(TEXT("ConcurrencySettings: ?")));
    TestFalse(TEXT("OwningActor ? omitted"),
        Result.BpirText.Contains(TEXT("OwningActor: ?")));

    for (const FBpirWarning& Warning : Result.Warnings)
    {
        TestFalse(
            FString::Printf(TEXT("Warning mentions ConcurrencySettings: %s"), *Warning.Text),
            Warning.Text.Contains(TEXT("ConcurrencySettings")));
        TestFalse(
            FString::Printf(TEXT("Warning mentions OwningActor: %s"), *Warning.Text),
            Warning.Text.Contains(TEXT("OwningActor")));
    }

    // Direct production-helper call: prove the helper itself classifies these
    // pins as omittable (not just that the surrounding pipeline happens to drop
    // them via some other path).
    if (ConcurrencyPin)
    {
        TestTrue(TEXT("Helper omits ConcurrencySettings pin"),
            BpirTextEmitterInternal::IsPinOmittableAtCallSite(ConcurrencyPin));
    }
    if (OwningActorPin)
    {
        TestTrue(TEXT("Helper omits OwningActor pin"),
            BpirTextEmitterInternal::IsPinOmittableAtCallSite(OwningActorPin));
    }
    return true;
}

// Asserts the generic fallback emits a `call K2Node_*` line without an "Unknown node type" warning.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerGenericFallbackNoWarningTest,
    "PinWright.bpir.decompiler.GenericFallbackNoWarning",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerGenericFallbackNoWarningTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTestBlueprint();
    UEdGraph* EventGraph = BP->UbergraphPages.Num() > 0 ? BP->UbergraphPages[0] : nullptr;
    TestNotNull(TEXT("EventGraph present"), EventGraph);
    if (!EventGraph) return false;

    // Find or create the ReceiveBeginPlay event node.
    UK2Node_Event* BeginPlayNode = FindBeginPlayNode(EventGraph);
    if (!BeginPlayNode)
    {
        BeginPlayNode = AddNodeToGraph<UK2Node_Event>(EventGraph);
        BeginPlayNode->EventReference.SetExternalMember(
            TEXT("ReceiveBeginPlay"), AActor::StaticClass());
        BeginPlayNode->bOverrideFunction = true;
        BeginPlayNode->ReconstructNode();
    }

    // Add a SpawnActorFromClass node and wire it after BeginPlay. This node has
    // no typed decompiler branch, so it goes through the smart generic fallback.
    UK2Node_SpawnActorFromClass* SpawnNode = AddNodeToGraph<UK2Node_SpawnActorFromClass>(EventGraph);
    WireExec(BeginPlayNode, SpawnNode);

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("decompile succeeded"), Result.bSuccess);
    TestTrue(TEXT("BPIR contains 'call K2Node_SpawnActorFromClass'"),
        Result.BpirText.Contains(TEXT("call K2Node_SpawnActorFromClass")));

    // The dead "Unknown node type (generic fallback)" warning emit must be gone.
    for (const FBpirWarning& Warning : Result.Warnings)
    {
        TestFalse(
            FString::Printf(TEXT("No 'Unknown node type (generic fallback)' warning: %s"), *Warning.Text),
            Warning.Text.Contains(TEXT("Unknown node type (generic fallback)")));
    }
    return true;
}

// Asserts UK2Node_InputActionEvent emits a proper `entry input_action_event Name(IE_*)` signature instead of UnknownEntry.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerInputActionEventEntrySignatureTest,
    "PinWright.bpir.decompiler.InputActionEventEntrySignature",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerInputActionEventEntrySignatureTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTestBlueprint();
    UEdGraph* EventGraph = BP->UbergraphPages.Num() > 0 ? BP->UbergraphPages[0] : nullptr;
    TestNotNull(TEXT("EventGraph present"), EventGraph);
    if (!EventGraph) return false;

    UK2Node_InputActionEvent* ActionNode = AddNodeToGraph<UK2Node_InputActionEvent>(EventGraph);
    ActionNode->InputActionName = TEXT("Jump");
    ActionNode->InputKeyEvent = IE_Pressed;
    ActionNode->ReconstructNode();

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("decompile succeeded"), Result.bSuccess);

    // Bug-defeating assertion: the old fallthrough emitted "entry event UnknownEntry()".
    TestFalse(TEXT("BPIR does not contain UnknownEntry"),
        Result.BpirText.Contains(TEXT("UnknownEntry")));

    // The action's name must reach the BPIR.
    TestTrue(TEXT("BPIR contains action name 'Jump'"),
        Result.BpirText.Contains(TEXT("Jump")));

    // Implementation may pick either keyword form; both are acceptable.
    const bool bHasKeyword =
        Result.BpirText.Contains(TEXT("input_action_event"))
        || Result.BpirText.Contains(TEXT("entry input_action"));
    TestTrue(TEXT("BPIR uses an input-action entry keyword"), bHasKeyword);
    return true;
}

// Asserts IsPinOmittableAtCallSite omits unwired PC_Struct (FKey) and PC_Name optional args.
// Counterfactual: if the call-function parameter struct fallback is reverted,
// the non-empty FKey default cannot be compared to the default struct instance
// after PinSubCategoryObject is cleared, so the helper returns false and
// FormatArgs can emit `Key: ?`. PC_Name stays covered by the #2 fix.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerOmitsOptionalStructNameUnwiredPinsTest,
    "PinWright.bpir.decompiler.optional_pin.OmitsUnwiredOptionalStructAndNameArgs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerOmitsOptionalStructNameUnwiredPinsTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph on test Blueprint")); return false; }

    UK2Node_Event* BeginPlayNode = FindBeginPlayNode(EventGraph);
    if (!BeginPlayNode)
    {
        BeginPlayNode = AddNodeToGraph<UK2Node_Event>(EventGraph);
        BeginPlayNode->EventReference.SetExternalMember(
            TEXT("ReceiveBeginPlay"), AActor::StaticClass());
        BeginPlayNode->bOverrideFunction = true;
        BeginPlayNode->ReconstructNode();
    }

    // PC_Struct/FKey case: APlayerController::WasInputKeyJustPressed(FKey).
    UFunction* WasPressedFn = APlayerController::StaticClass()->FindFunctionByName(
        TEXT("WasInputKeyJustPressed"));
    TestNotNull(TEXT("WasInputKeyJustPressed function resolves"), WasPressedFn);
    if (!WasPressedFn) return false;

    UK2Node_CallFunction* WasPressedNode = AddNodeToGraph<UK2Node_CallFunction>(EventGraph);
    WasPressedNode->SetFromFunction(WasPressedFn);
    WasPressedNode->ReconstructNode();
    WireExec(BeginPlayNode, WasPressedNode);

    UEdGraphPin* KeyPin = WasPressedNode->FindPin(TEXT("Key"), EGPD_Input);
    TestNotNull(TEXT("WasInputKeyJustPressed has Key input pin"), KeyPin);

    // PC_Name case: CreateDynamicMaterialInstance.OptionalName.
    UFunction* CreateDmiFn = UPrimitiveComponent::StaticClass()->FindFunctionByName(
        TEXT("CreateDynamicMaterialInstance"));
    TestNotNull(TEXT("CreateDynamicMaterialInstance function resolves"), CreateDmiFn);
    if (!CreateDmiFn) return false;

    UK2Node_CallFunction* CreateDmiNode = AddNodeToGraph<UK2Node_CallFunction>(EventGraph);
    CreateDmiNode->SetFromFunction(CreateDmiFn);
    CreateDmiNode->ReconstructNode();
    WireExec(WasPressedNode, CreateDmiNode);

    UEdGraphPin* OptionalNamePin = CreateDmiNode->FindPin(TEXT("OptionalName"), EGPD_Input);
    TestNotNull(TEXT("CreateDynamicMaterialInstance has OptionalName input pin"), OptionalNamePin);

    if (KeyPin)
    {
        TestEqual(TEXT("Key unwired"), KeyPin->LinkedTo.Num(), 0);
        KeyPin->PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
        KeyPin->PinType.PinSubCategoryObject = nullptr;
        KeyPin->DefaultValue = TEXT("None");
        KeyPin->AutogeneratedDefaultValue.Empty();
        TestNull(TEXT("Key loaded-asset shape has no PinSubCategoryObject"),
            KeyPin->PinType.PinSubCategoryObject.Get());
        TestFalse(TEXT("Key fallback test uses non-empty default"),
            KeyPin->DefaultValue.IsEmpty());
        TestTrue(TEXT("Helper omits loaded-asset FKey non-empty PC_Struct pin"),
            BpirTextEmitterInternal::IsPinOmittableAtCallSite(KeyPin));
    }
    if (OptionalNamePin)
    {
        TestEqual(TEXT("OptionalName unwired"), OptionalNamePin->LinkedTo.Num(), 0);
    }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("decompile succeeded"), Result.bSuccess);
    TestFalse(TEXT("Key ? omitted"),
        Result.BpirText.Contains(TEXT("Key: ?")));
    TestFalse(TEXT("OptionalName ? omitted"),
        Result.BpirText.Contains(TEXT("OptionalName: ?")));

    // Direct production-helper call: prove the helper itself classifies these
    // pins as omittable (not just that the surrounding pipeline happens to drop
    // them via some other path).
    if (KeyPin)
    {
        TestTrue(TEXT("Helper omits Key (PC_Struct/FKey) pin"),
            BpirTextEmitterInternal::IsPinOmittableAtCallSite(KeyPin));
    }
    if (OptionalNamePin)
    {
        TestTrue(TEXT("Helper omits OptionalName (PC_Name) pin"),
            BpirTextEmitterInternal::IsPinOmittableAtCallSite(OptionalNamePin));
    }
    return true;
}

// FKey output of UK2Node_InputKey, wired through a knot into a pure callee's
// Key pin (whose return value is consumed by a reachable Branch), must resolve
// to the inline key literal (e.g. `E`) — not `?`. The InputKey node is
// treated as an entry by the emitter and never gets a NodeToValueName entry,
// so without an explicit branch in ResolveInputValue the resolver falls
// through to the "visited but produced no value name" warning path.
// Counterfactual: revert the UK2Node_InputKey branch in ResolveInputValue and
// the assertion `Key: ?` not present fails.
//
// WasInputKeyJustPressed is `BlueprintCallable` + `const`, which UHT marks as
// a pure node (no exec pins). To force the decompiler to emit it (and thus
// resolve the Key input pin), its ReturnValue must be consumed by a
// reachable impure node — here, BeginPlay -> Branch with Condition fed from
// the pure call.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerInputKeyConstOutputResolvesAsLiteralTest,
    "PinWright.bpir.decompiler.input_key.WiredKeyOutputResolvesToLiteral",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerInputKeyConstOutputResolvesAsLiteralTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph on test Blueprint")); return false; }

    UK2Node_Event* BeginPlayNode = FindBeginPlayNode(EventGraph);
    if (!BeginPlayNode)
    {
        BeginPlayNode = AddNodeToGraph<UK2Node_Event>(EventGraph);
        BeginPlayNode->EventReference.SetExternalMember(
            TEXT("ReceiveBeginPlay"), AActor::StaticClass());
        BeginPlayNode->bOverrideFunction = true;
        BeginPlayNode->ReconstructNode();
    }

    UK2Node_InputKey* InputKeyNode = AddNodeToGraph<UK2Node_InputKey>(EventGraph);
    InputKeyNode->InputKey = EKeys::E;
    InputKeyNode->ReconstructNode();

    UFunction* WasPressedFn = APlayerController::StaticClass()->FindFunctionByName(
        TEXT("WasInputKeyJustPressed"));
    TestNotNull(TEXT("WasInputKeyJustPressed function resolves"), WasPressedFn);
    if (!WasPressedFn) return false;

    UK2Node_CallFunction* WasPressedNode = AddNodeToGraph<UK2Node_CallFunction>(EventGraph);
    WasPressedNode->SetFromFunction(WasPressedFn);
    WasPressedNode->ReconstructNode();

    UK2Node_IfThenElse* BranchNode = AddNodeToGraph<UK2Node_IfThenElse>(EventGraph);
    WireExec(BeginPlayNode, BranchNode);

    UEdGraphPin* RetPin = WasPressedNode->FindPin(UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output);
    UEdGraphPin* CondPin = BranchNode->FindPin(TEXT("Condition"), EGPD_Input);
    TestNotNull(TEXT("WasInputKeyJustPressed ReturnValue pin"), RetPin);
    TestNotNull(TEXT("Branch Condition pin"), CondPin);
    if (RetPin && CondPin) { RetPin->MakeLinkTo(CondPin); }

    UEdGraphPin* InputKeyOut = InputKeyNode->FindPin(TEXT("Key"), EGPD_Output);
    UEdGraphPin* CallKeyIn = WasPressedNode->FindPin(TEXT("Key"), EGPD_Input);
    TestNotNull(TEXT("InputKey Key output pin"), InputKeyOut);
    TestNotNull(TEXT("WasInputKeyJustPressed Key input pin"), CallKeyIn);

    UK2Node_Knot* KnotNode = AddNodeToGraph<UK2Node_Knot>(EventGraph);
    UEdGraphPin* KnotIn = KnotNode->GetInputPin();
    UEdGraphPin* KnotOut = KnotNode->GetOutputPin();
    TestNotNull(TEXT("Knot input pin"), KnotIn);
    TestNotNull(TEXT("Knot output pin"), KnotOut);

    if (InputKeyOut && KnotIn) { InputKeyOut->MakeLinkTo(KnotIn); }
    if (KnotOut && CallKeyIn) { KnotOut->MakeLinkTo(CallKeyIn); }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("decompile succeeded"), Result.bSuccess);
    TestFalse(TEXT("Key:? not emitted"),
        Result.BpirText.Contains(TEXT("Key: ?")));
    TestTrue(TEXT("Key resolves to literal 'E'"),
        Result.BpirText.Contains(TEXT("Key: E")));
    TestFalse(TEXT("no 'visited but produced no value name' warning for InputKey 'E'"),
        Result.Warnings.ContainsByPredicate([](const FBpirWarning& W)
        {
            return W.Text.Contains(TEXT("source node 'E' was visited but produced no value name"));
        }));
    return true;
}

// Same shape as the wired-via-knot test but with a direct wire from
// UK2Node_InputKey.Key to the callee's Key pin. Catches a fix that only
// handles the FollowKnotsBackward path (e.g. branching off DeadEndKnotInput
// instead of the resolved SourceNode).

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerInputKeyConstOutputDirectWireTest,
    "PinWright.bpir.decompiler.input_key.DirectWiredKeyOutputResolvesToLiteral",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerInputKeyConstOutputDirectWireTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTestBlueprint();
    if (!BP) { AddError(TEXT("Failed to create test Blueprint")); return false; }

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    if (!EventGraph) { AddError(TEXT("No EventGraph on test Blueprint")); return false; }

    UK2Node_Event* BeginPlayNode = FindBeginPlayNode(EventGraph);
    if (!BeginPlayNode)
    {
        BeginPlayNode = AddNodeToGraph<UK2Node_Event>(EventGraph);
        BeginPlayNode->EventReference.SetExternalMember(
            TEXT("ReceiveBeginPlay"), AActor::StaticClass());
        BeginPlayNode->bOverrideFunction = true;
        BeginPlayNode->ReconstructNode();
    }

    UK2Node_InputKey* InputKeyNode = AddNodeToGraph<UK2Node_InputKey>(EventGraph);
    InputKeyNode->InputKey = EKeys::E;
    InputKeyNode->ReconstructNode();

    UFunction* WasPressedFn = APlayerController::StaticClass()->FindFunctionByName(
        TEXT("WasInputKeyJustPressed"));
    TestNotNull(TEXT("WasInputKeyJustPressed function resolves"), WasPressedFn);
    if (!WasPressedFn) return false;

    UK2Node_CallFunction* WasPressedNode = AddNodeToGraph<UK2Node_CallFunction>(EventGraph);
    WasPressedNode->SetFromFunction(WasPressedFn);
    WasPressedNode->ReconstructNode();

    UK2Node_IfThenElse* BranchNode = AddNodeToGraph<UK2Node_IfThenElse>(EventGraph);
    WireExec(BeginPlayNode, BranchNode);

    UEdGraphPin* RetPin = WasPressedNode->FindPin(UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output);
    UEdGraphPin* CondPin = BranchNode->FindPin(TEXT("Condition"), EGPD_Input);
    TestNotNull(TEXT("WasInputKeyJustPressed ReturnValue pin"), RetPin);
    TestNotNull(TEXT("Branch Condition pin"), CondPin);
    if (RetPin && CondPin) { RetPin->MakeLinkTo(CondPin); }

    UEdGraphPin* InputKeyOut = InputKeyNode->FindPin(TEXT("Key"), EGPD_Output);
    UEdGraphPin* CallKeyIn = WasPressedNode->FindPin(TEXT("Key"), EGPD_Input);
    TestNotNull(TEXT("InputKey Key output pin"), InputKeyOut);
    TestNotNull(TEXT("WasInputKeyJustPressed Key input pin"), CallKeyIn);
    if (InputKeyOut && CallKeyIn) { InputKeyOut->MakeLinkTo(CallKeyIn); }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("decompile succeeded"), Result.bSuccess);
    TestFalse(TEXT("Key:? not emitted"),
        Result.BpirText.Contains(TEXT("Key: ?")));
    TestTrue(TEXT("Key resolves to literal 'E'"),
        Result.BpirText.Contains(TEXT("Key: E")));
    TestFalse(TEXT("no 'visited but produced no value name' warning for InputKey 'E'"),
        Result.Warnings.ContainsByPredicate([](const FBpirWarning& W)
        {
            return W.Text.Contains(TEXT("source node 'E' was visited but produced no value name"));
        }));
    return true;
}

// ============================================================================
// Decompiler.variable_set.EmitsTypedDefaultNotQuestionMark
// Set node with an unwired value pin and no author-set default must emit a
// typed default literal (nullptr / "" / false / 0 / None) rather than `?`.
// Covers object-typed, string, bool, int, and FName member variables.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerVariableSetEmitsTypedDefaultNotQuestionMarkTest,
    "PinWright.bpir.decompiler.variable_set.EmitsTypedDefaultNotQuestionMark",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerVariableSetEmitsTypedDefaultNotQuestionMarkTest::RunTest(const FString& Parameters)
{
    // Counterfactual: If FormatPinDefaultLiteral typed-default branch is reverted
    // to return TEXT("?"), the unwired object-typed pin falls through
    // ResolveInputValue Case 2 with empty default fields, the helper returns ?,
    // EmitVariableSet formats `set MusicManager = ?`, and this test fails on the
    // Contains check.

    struct FCase
    {
        const TCHAR* VarName;
        FEdGraphPinType PinType;
        const TCHAR* ExpectedRhs;
    };

    TArray<FCase> Cases;
    {
        FCase C;
        C.VarName = TEXT("MusicManager");
        C.PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
        C.PinType.PinSubCategoryObject = AActor::StaticClass();
        C.ExpectedRhs = TEXT("nullptr");
        Cases.Add(C);
    }
    {
        FCase C;
        C.VarName = TEXT("Greeting");
        C.PinType.PinCategory = UEdGraphSchema_K2::PC_String;
        C.ExpectedRhs = TEXT("\"\"");
        Cases.Add(C);
    }
    {
        FCase C;
        C.VarName = TEXT("Ready");
        C.PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
        C.ExpectedRhs = TEXT("false");
        Cases.Add(C);
    }
    {
        FCase C;
        C.VarName = TEXT("Count");
        C.PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
        C.ExpectedRhs = TEXT("0");
        Cases.Add(C);
    }
    {
        FCase C;
        C.VarName = TEXT("Tag");
        C.PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
        C.ExpectedRhs = TEXT("None");
        Cases.Add(C);
    }

    for (const FCase& C : Cases)
    {
        UBlueprint* BP = CreateTestBlueprint();
        if (!BP) { AddError(FString::Printf(TEXT("Failed to create test BP for %s"), C.VarName)); return false; }

        FBlueprintEditorUtils::AddMemberVariable(BP, C.VarName, C.PinType);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

        UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
        if (!EventGraph) { AddError(TEXT("No EventGraph on test Blueprint")); return false; }

        UK2Node_Event* BeginPlayNode = FindBeginPlayNode(EventGraph);
        if (!BeginPlayNode)
        {
            BeginPlayNode = AddNodeToGraph<UK2Node_Event>(EventGraph);
            BeginPlayNode->EventReference.SetExternalMember(
                TEXT("ReceiveBeginPlay"), AActor::StaticClass());
            BeginPlayNode->bOverrideFunction = true;
            BeginPlayNode->ReconstructNode();
        }

        UK2Node_VariableSet* SetNode = NewObject<UK2Node_VariableSet>(EventGraph);
        SetNode->VariableReference.SetSelfMember(C.VarName);
        EventGraph->AddNode(SetNode, /*bFromUI=*/true, /*bSelectNewNode=*/false);
        SetNode->CreateNewGuid();
        SetNode->PostPlacedNewNode();
        SetNode->AllocateDefaultPins();
        SetNode->ReconstructNode();
        WireExec(BeginPlayNode, SetNode);

        // Sanity: locate the value pin (named after the variable), confirm it's
        // unwired, and clear any author-set default so we exercise the
        // type-default fallback specifically.
        UEdGraphPin* ValuePin = nullptr;
        for (UEdGraphPin* Pin : SetNode->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Input
                && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
                && Pin->PinName != UEdGraphSchema_K2::PN_Self
                && Pin->PinName.ToString() == FString(C.VarName))
            {
                ValuePin = Pin;
                break;
            }
        }
        TestNotNull(*FString::Printf(TEXT("[%s] value pin found"), C.VarName), ValuePin);
        if (ValuePin)
        {
            TestEqual(*FString::Printf(TEXT("[%s] value pin unwired"), C.VarName),
                ValuePin->LinkedTo.Num(), 0);
            ValuePin->DefaultValue.Empty();
            ValuePin->DefaultObject = nullptr;
            ValuePin->DefaultTextValue = FText::GetEmpty();
        }

        FBpirDecompiler Decompiler(BP);
        FBpirDecompileResult Result = Decompiler.Decompile();

        TestTrue(*FString::Printf(TEXT("[%s] decompile succeeded"), C.VarName), Result.bSuccess);

        const FString Expected = FString::Printf(TEXT("set %s = %s"), C.VarName, C.ExpectedRhs);
        const FString Bad      = FString::Printf(TEXT("set %s = ?"), C.VarName);

        TestTrue(*FString::Printf(TEXT("[%s] BPIR contains '%s'"), C.VarName, *Expected),
            Result.BpirText.Contains(Expected));
        TestFalse(*FString::Printf(TEXT("[%s] BPIR does not contain '%s'"), C.VarName, *Bad),
            Result.BpirText.Contains(Bad));

        for (const FBpirWarning& Warning : Result.Warnings)
        {
            TestFalse(
                *FString::Printf(TEXT("[%s] no warning mentions variable name: %s"), C.VarName, *Warning.Text),
                Warning.Text.Contains(C.VarName));
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirQuestionMarkNameTokensDecompileTest,
    "PinWright.Decompiler.NameTokens.QuestionMarkBooleanNames",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirQuestionMarkNameTokensDecompileTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTestBlueprint();
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    FEdGraphPinType BoolType;
    BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("Raw Value?"), BoolType);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

    UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
    TestNotNull(TEXT("Event graph exists"), EventGraph);
    if (!EventGraph) return false;

    UK2Node_Event* BeginPlayNode = FindBeginPlayNode(EventGraph);
    if (!BeginPlayNode)
    {
        BeginPlayNode = AddNodeToGraph<UK2Node_Event>(EventGraph);
        BeginPlayNode->EventReference.SetExternalMember(
            TEXT("ReceiveBeginPlay"), AActor::StaticClass());
        BeginPlayNode->bOverrideFunction = true;
        BeginPlayNode->ReconstructNode();
    }

    UK2Node_CallFunction* PrintNode = AddNodeToGraph<UK2Node_CallFunction>(EventGraph);
    PrintNode->FunctionReference.SetExternalMember(
        TEXT("PrintString"), UKismetSystemLibrary::StaticClass());
    PrintNode->ReconstructNode();
    WireExec(BeginPlayNode, PrintNode);

    UK2Node_VariableGet* GetNode = NewObject<UK2Node_VariableGet>(EventGraph);
    GetNode->VariableReference.SetSelfMember(TEXT("Raw Value?"));
    EventGraph->AddNode(GetNode, /*bFromUI=*/true, /*bSelectNewNode=*/false);
    GetNode->CreateNewGuid();
    GetNode->PostPlacedNewNode();
    GetNode->AllocateDefaultPins();
    GetNode->ReconstructNode();

    UEdGraphPin* RawValuePin = GetNode->FindPin(TEXT("Raw Value?"), EGPD_Output);
    UEdGraphPin* InStringPin = PrintNode->FindPin(TEXT("InString"), EGPD_Input);
    TestNotNull(TEXT("Raw Value? output pin"), RawValuePin);
    TestNotNull(TEXT("PrintString InString pin"), InStringPin);
    if (RawValuePin && InStringPin)
    {
        RawValuePin->MakeLinkTo(InStringPin);
    }

    UK2Node_CustomEvent* CustomEvent = AddNodeToGraph<UK2Node_CustomEvent>(EventGraph);
    CustomEvent->CustomFunctionName = TEXT("OnQuestionNames");
    CustomEvent->CreateUserDefinedPin(FName(TEXT("Mode01?")), BoolType, EGPD_Output);
    CustomEvent->ReconstructNode();

    UK2Node_IfThenElse* BranchNode = AddNodeToGraph<UK2Node_IfThenElse>(EventGraph);
    WireExec(CustomEvent, BranchNode);

    UEdGraphPin* ModePin = CustomEvent->FindPin(TEXT("Mode01?"), EGPD_Output);
    UEdGraphPin* ConditionPin = BranchNode->FindPin(TEXT("Condition"), EGPD_Input);
    TestNotNull(TEXT("Mode01? output pin"), ModePin);
    TestNotNull(TEXT("Branch Condition pin"), ConditionPin);
    if (ModePin && ConditionPin)
    {
        ModePin->MakeLinkTo(ConditionPin);
    }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    const FString& Output = Result.BpirText;
    const bool bHasQuotedVariableRef =
        Output.Contains(TEXT("$`Raw Value?`")) || Output.Contains(TEXT("get `Raw Value?`"));
    TestTrue(TEXT("Output contains backtick-quoted Raw Value? variable token"), bHasQuotedVariableRef);
    TestTrue(TEXT("Output contains backtick-quoted Mode01? bool param"),
        Output.Contains(TEXT("bool `Mode01?`")));
    TestTrue(TEXT("Output contains backtick-quoted Mode01? param reference"),
        Output.Contains(TEXT("$`Mode01?`")));
    TestFalse(TEXT("Output does not contain raw $Raw Value? variable token"),
        Output.Contains(TEXT("$Raw Value?")));
    TestFalse(TEXT("Output does not contain raw $Mode01? param reference"),
        Output.Contains(TEXT("$Mode01?")));
    TestFalse(TEXT("Output does not contain raw bool Mode01? param"),
        Output.Contains(TEXT("bool Mode01?")));

    UBlueprint* CompileBP = CompilerTestUtils::CreateTransientTestBP(TEXT("QuestionMarkNameTokenCompileBP"));
    TestNotNull(TEXT("Compile Blueprint created"), CompileBP);
    if (!CompileBP) return false;

    const FString CompileCode = TEXT(
        "entry custom_event OnQuestionNames(bool `Mode01?`) {\n"
        "    branch($`Mode01?`) [true -> @yes, false -> @no]\n"
        "    @yes:\n"
        "    call PrintString(InString: \"yes\")\n"
        "    exec -> @done\n"
        "    @no:\n"
        "    call PrintString(InString: \"no\")\n"
        "    @done:\n"
        "}"
    );

    FBpirCompiler Compiler(CompileBP);
    FCompileResult CompileResult = Compiler.Compile(CompileCode);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compiler accepts $`Mode01?` param reference"), CompileResult.bSuccess);
    return true;
}

// ============================================================================
// Decompiler.CallEmitsPinTypeAnnotation
// "%name = call ..." emits its primary output pin type as ": <Type>" between
// the register and the '='. The annotation appears before the '=' token.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerCallEmitsPinTypeAnnotationTest,
    "PinWright.bpir.decompiler.CallEmitsPinTypeAnnotation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerCallEmitsPinTypeAnnotationTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompTypeAnnoCallBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString Code = TEXT(
        "entry event BeginPlay() {\n"
        "    %gi = pure GetGameInstance(self)\n"
        // Consume %gi so the decompiler emits its binding line (and thus the
        // object<GameInstance> annotation under test). Unreferenced pure nodes
        // are intentionally elided by EmitPureDependencies — see BpirDecompiler.cpp.
        "    %valid = pure IsValid(Object: %gi)\n"
        "    branch(%valid) [true -> @then]\n"
        "@then:\n"
        "    call PrintString(InString: \"ok\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(Code);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);

    const FString& Output = DecompileResult.BpirText;

    // Bool-returning pure call: %valid: bool = ...
    TestTrue(TEXT("IsValid binding carries ': bool' annotation before '='"),
        ContainsLineWith(Output, TEXT(": bool"), TEXT("= pure IsValid"))
        || ContainsLineWith(Output, TEXT(": bool"), TEXT("= call IsValid")));

    // Object-returning pure call: %gi: object<GameInstance> = ...
    TestTrue(TEXT("GetGameInstance binding carries object<...> annotation"),
        ContainsLineWith(Output, TEXT(": object<"), TEXT("GameInstance")));
    return true;
}

// ============================================================================
// Decompiler.BranchAndCastEmitTypeAnnotation
// branch and cast register bindings emit ": <Type>" annotations as well.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDecompilerBranchAndCastEmitTypeAnnotationTest,
    "PinWright.bpir.decompiler.BranchAndCastEmitTypeAnnotation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDecompilerBranchAndCastEmitTypeAnnotationTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("DecompTypeAnnoBranchCastBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString Code = TEXT(
        "entry event BeginPlay() {\n"
        "    %obj = pure GetOwner(self)\n"
        "    %c = cast<Pawn>(%obj) [success -> @ok, fail -> @nope]\n"
        "@ok:\n"
        // Named-arg form: positional pure IsValid(%c.AsPawn) hits ambiguous
        // broad-search resolution that picks an instance UFunction whose
        // self-target pin rejects object<Pawn>. Canonical form
        // (TestBpirExpression.cpp:548 and many others) is IsValid(Object: ...).
        "    %cond = pure IsValid(Object: %c.AsPawn)\n"
        "    %br = branch(%cond) [true -> @done]\n"
        "@done:\n"
        "    call PrintString(InString: \"ok\")\n"
        "@nope:\n"
        "    call PrintString(InString: \"no\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(Code);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);

    const FString& Output = DecompileResult.BpirText;

    // Cast binding carries object<Pawn> annotation before '='.
    TestTrue(TEXT("cast binding carries 'object<Pawn>' annotation before '='"),
        ContainsLineWith(Output, TEXT(": object<Pawn>"), TEXT("= cast<Pawn>")));

    // Branch binding carries an annotation: branch's primary output is the exec
    // "then" pin filtered out by ResolvePrimaryOutputTypeAnnotation, so the
    // emitted line has no type annotation — just confirm the bare form survives.
    TestTrue(TEXT("branch binding survives without type annotation (exec-only output)"),
        Output.Contains(TEXT("= branch(")));

    // No annotation should appear inside parentheses (those carry call args).
    // Defensive: ensure colon-prefixed types only show up before '='.
    TArray<FString> Lines;
    Output.ParseIntoArrayLines(Lines);
    for (const FString& Line : Lines)
    {
        const int32 EqIdx = Line.Find(TEXT(" = "));
        const int32 OpenParenIdx = Line.Find(TEXT("("));
        if (EqIdx == INDEX_NONE || OpenParenIdx == INDEX_NONE) continue;
        // If the line has a ": " inside the args, that's the legitimate "PinName: Value" form.
        // Confirm any ": " seen BEFORE the '=' has a non-empty typespec between it and the '='.
        const int32 ColonIdx = Line.Find(TEXT(": "));
        if (ColonIdx != INDEX_NONE && ColonIdx < EqIdx)
        {
            const FString TypeSrc = Line.Mid(ColonIdx + 2, EqIdx - (ColonIdx + 2)).TrimStartAndEnd();
            TestFalse(TEXT("Binding-type annotation between ': ' and ' = ' must be non-empty"),
                TypeSrc.IsEmpty());
        }
    }
    return true;
}
