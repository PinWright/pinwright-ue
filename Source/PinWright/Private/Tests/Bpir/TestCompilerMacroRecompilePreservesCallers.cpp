// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "CompilerTestUtils.h"

// TestSamePtr was added to FAutomationTestBase in UE 5.6.
// On 5.4/5.5 use a TestEqual on void* which is semantically identical.
#if UE_VERSION_OLDER_THAN(5, 6, 0)
#define MCP_TestSamePtr(Description, Actual, Expected) \
    TestEqual(Description, static_cast<const void*>(Actual), static_cast<const void*>(Expected))
#else
#define MCP_TestSamePtr(Description, Actual, Expected) \
    TestSamePtr(Description, Actual, Expected)
#endif

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_MacroInstance.h"
#include "Kismet/KismetSystemLibrary.h"

using namespace CompilerTestUtils;

namespace
{
    UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FGuid& Guid)
    {
        if (!Graph)
        {
            return nullptr;
        }

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && Node->NodeGuid == Guid)
            {
                return Node;
            }
        }
        return nullptr;
    }

    UK2Node_MacroInstance* SpawnMacroCaller(UEdGraph* Graph, UEdGraph* MacroGraph)
    {
        if (!Graph || !MacroGraph)
        {
            return nullptr;
        }

        UK2Node_MacroInstance* Node = NewObject<UK2Node_MacroInstance>(Graph);
        Node->CreateNewGuid();
        Node->SetMacroGraph(MacroGraph);
        Node->PostPlacedNewNode();
        Node->AllocateDefaultPins();
        Graph->AddNode(Node, /*bFromUI=*/true, /*bSelectNewNode=*/false);
        return Node;
    }

    bool OptionalStringMatches(const TOptional<FString>& A, const TOptional<FString>& B)
    {
        if (A.IsSet() != B.IsSet())
        {
            return false;
        }
        return !A.IsSet() || A.GetValue() == B.GetValue();
    }

    bool StringPointerValueMatches(const FString* A, const FString* B)
    {
        if ((A == nullptr) != (B == nullptr))
        {
            return false;
        }
        return !A || *A == *B;
    }

    bool TextIdentityMatches(const FText& A, const FText& B)
    {
        if (A.ToString() != B.ToString())
        {
            return false;
        }

        FName TableA;
        FName TableB;
        FString TableKeyA;
        FString TableKeyB;
        const bool bHasTableA = FTextInspector::GetTableIdAndKey(A, TableA, TableKeyA);
        const bool bHasTableB = FTextInspector::GetTableIdAndKey(B, TableB, TableKeyB);
        if (bHasTableA != bHasTableB)
        {
            return false;
        }
        if (bHasTableA && (TableA != TableB || TableKeyA != TableKeyB))
        {
            return false;
        }

        if (!OptionalStringMatches(FTextInspector::GetNamespace(A), FTextInspector::GetNamespace(B)))
        {
            return false;
        }
        if (!OptionalStringMatches(FTextInspector::GetKey(A), FTextInspector::GetKey(B)))
        {
            return false;
        }

        return StringPointerValueMatches(FTextInspector::GetSourceString(A), FTextInspector::GetSourceString(B));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirMacroRecompilePreservesCallerInstancesTest,
    "PinWright.bpir.compiler.MacroRecompilePreservesCallerInstances",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirMacroRecompilePreservesCallerInstancesTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("MacroRecompilePreservesCallersBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP)
    {
        return false;
    }

    {
        FBpirCompiler Compiler(BP);
        FCompileResult Result = Compiler.Compile(
            TEXT("entry macro TestMacro(float Value, text Label, object<Object> Target) -> (float Result) {\n")
            TEXT("    %clamped = pure FClamp(Value: $Value, Min: 0.0, Max: 10.0)\n")
            TEXT("    return (Result: %clamped)\n")
            TEXT("}"));
        if (!Result.bSuccess)
        {
            for (const FCompileError& Err : Result.Errors)
            {
                AddError(FString::Printf(TEXT("Initial macro compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Initial macro compile succeeded"), Result.bSuccess);
        if (!Result.bSuccess)
        {
            return false;
        }
    }

    UEdGraph* MacroGraph = FindMacroGraph(BP, TEXT("TestMacro"));
    TestNotNull(TEXT("Macro graph exists"), MacroGraph);
    if (!MacroGraph)
    {
        return false;
    }

    UEdGraph* EventGraph = BP->UbergraphPages.Num() > 0 ? BP->UbergraphPages[0] : nullptr;
    TestNotNull(TEXT("Event graph exists"), EventGraph);
    if (!EventGraph)
    {
        return false;
    }

    UK2Node_CustomEvent* CallerEntry = SpawnNode<UK2Node_CustomEvent>(EventGraph, 0, 0);
    CallerEntry->CustomFunctionName = TEXT("CallerEvent");
    CallerEntry->ReconstructNode();

    UK2Node_MacroInstance* CallerMacro = SpawnMacroCaller(EventGraph, MacroGraph);
    TestNotNull(TEXT("Macro caller exists before recompile"), CallerMacro);
    if (!CallerMacro)
    {
        return false;
    }

    UEdGraphPin* CallerThen = CallerEntry->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
    UEdGraphPin* MacroExecIn = GetDefault<UEdGraphSchema_K2>()->FindExecutionPin(*CallerMacro, EGPD_Input);
    TestNotNull(TEXT("Caller then pin exists"), CallerThen);
    TestNotNull(TEXT("Macro caller exec input exists"), MacroExecIn);
    if (!CallerThen || !MacroExecIn)
    {
        return false;
    }

    UEdGraphPin* ValuePin = CallerMacro->FindPin(TEXT("Value"), EGPD_Input);
    UEdGraphPin* LabelPin = CallerMacro->FindPin(TEXT("Label"), EGPD_Input);
    UEdGraphPin* TargetPin = CallerMacro->FindPin(TEXT("Target"), EGPD_Input);
    UEdGraphPin* ResultPin = CallerMacro->FindPin(TEXT("Result"), EGPD_Output);
    TestNotNull(TEXT("Macro caller value pin exists"), ValuePin);
    TestNotNull(TEXT("Macro caller label pin exists"), LabelPin);
    TestNotNull(TEXT("Macro caller target pin exists"), TargetPin);
    TestNotNull(TEXT("Macro caller result pin exists"), ResultPin);
    if (!ValuePin || !LabelPin || !TargetPin || !ResultPin)
    {
        return false;
    }

    ValuePin->DefaultValue = TEXT("5.0");
    const FText OriginalLabelText = FText::ChangeKey(
        TEXT("PinWrightTests"),
        TEXT("MacroCallerLocalizedLabel"),
        FText::FromString(TEXT("Localized caller label")));
    LabelPin->DefaultTextValue = OriginalLabelText;
    TargetPin->DefaultObject = BP;

    UK2Node_CallFunction* FloatSource = SpawnNode<UK2Node_CallFunction>(EventGraph, -300, 120);
    FloatSource->FunctionReference.SetExternalMember(
        GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, MakeLiteralFloat),
        UKismetSystemLibrary::StaticClass());
    FloatSource->ReconstructNode();
    UEdGraphPin* FloatSourceOut = FloatSource->FindPin(UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output);
    TestNotNull(TEXT("Float source output exists"), FloatSourceOut);
    if (!FloatSourceOut)
    {
        return false;
    }
    FloatSourceOut->MakeLinkTo(ValuePin);

    const FEdGraphPinType OriginalValuePinType = ValuePin->PinType;
    const FEdGraphPinType OriginalLabelPinType = LabelPin->PinType;
    const FEdGraphPinType OriginalTargetPinType = TargetPin->PinType;
    const FEdGraphPinType OriginalResultPinType = ResultPin->PinType;

    CallerThen->MakeLinkTo(MacroExecIn);
    const FGuid CallerMacroGuid = CallerMacro->NodeGuid;

    {
        FBpirCompiler Compiler(BP);
        FCompileResult Result = Compiler.Compile(
            TEXT("entry macro TestMacro(float Value, text Label, object<Object> Target) -> (float Result) {\n")
            TEXT("    %clamped = pure FClamp(Value: $Value, Min: 1.0, Max: 9.0)\n")
            TEXT("    return (Result: %clamped)\n")
            TEXT("}"),
            EBpirCompileMode::Replace);
        if (!Result.bSuccess)
        {
            for (const FCompileError& Err : Result.Errors)
            {
                AddError(FString::Printf(TEXT("Macro recompile L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Macro recompile succeeded"), Result.bSuccess);
        if (!Result.bSuccess)
        {
            return false;
        }
    }

    UEdGraph* RecompiledMacroGraph = FindMacroGraph(BP, TEXT("TestMacro"));
    TestEqual(TEXT("Macro graph pointer was preserved"), RecompiledMacroGraph, MacroGraph);

    UK2Node_MacroInstance* ReconstructedCaller = Cast<UK2Node_MacroInstance>(
        FindNodeByGuid(EventGraph, CallerMacroGuid));
    TestNotNull(TEXT("Original caller node GUID still resolves"), ReconstructedCaller);
    if (!ReconstructedCaller)
    {
        return false;
    }

    TestEqual(TEXT("Caller remains bound to the recompiled macro graph"),
        ReconstructedCaller->GetMacroGraph(), RecompiledMacroGraph);

    UEdGraphPin* RecompiledValuePin = ReconstructedCaller->FindPin(TEXT("Value"), EGPD_Input);
    UEdGraphPin* RecompiledLabelPin = ReconstructedCaller->FindPin(TEXT("Label"), EGPD_Input);
    UEdGraphPin* RecompiledTargetPin = ReconstructedCaller->FindPin(TEXT("Target"), EGPD_Input);
    UEdGraphPin* RecompiledResultPin = ReconstructedCaller->FindPin(TEXT("Result"), EGPD_Output);
    TestNotNull(TEXT("Value pin survives successful recompile"), RecompiledValuePin);
    TestNotNull(TEXT("Label pin survives successful recompile"), RecompiledLabelPin);
    TestNotNull(TEXT("Target object pin survives successful recompile"), RecompiledTargetPin);
    TestNotNull(TEXT("Defaultless result pin survives successful recompile"), RecompiledResultPin);
    if (!RecompiledValuePin || !RecompiledLabelPin || !RecompiledTargetPin || !RecompiledResultPin)
    {
        return false;
    }

    TestTrue(TEXT("Value pin type survives successful recompile"),
        RecompiledValuePin->PinType == OriginalValuePinType);
    TestTrue(TEXT("Label pin type survives successful recompile"),
        RecompiledLabelPin->PinType == OriginalLabelPinType);
    TestTrue(TEXT("Target object pin type survives successful recompile"),
        RecompiledTargetPin->PinType == OriginalTargetPinType);
    TestTrue(TEXT("Result pin type survives successful recompile"),
        RecompiledResultPin->PinType == OriginalResultPinType);
    TestEqual(TEXT("Value default survives successful recompile"),
        RecompiledValuePin->DefaultValue,
        FString(TEXT("5.0")));
    TestTrue(TEXT("Label FText identity survives successful recompile"),
        TextIdentityMatches(RecompiledLabelPin->DefaultTextValue, OriginalLabelText));
    MCP_TestSamePtr(TEXT("Target DefaultObject survives successful recompile"),
        RecompiledTargetPin->DefaultObject.Get(), static_cast<UObject*>(BP));
    TestEqual(TEXT("Value source output still has one link after successful recompile"),
        FloatSourceOut->LinkedTo.Num(), 1);
    MCP_TestSamePtr(TEXT("Value source still targets the reconstructed macro value pin"),
        FloatSourceOut->LinkedTo.Num() > 0 ? FloatSourceOut->LinkedTo[0] : nullptr,
        RecompiledValuePin);

    TestEqual(TEXT("Caller upstream exec still has one link"), CallerThen->LinkedTo.Num(), 1);
    UEdGraphPin* RewiredMacroExecIn = CallerThen->LinkedTo.Num() > 0 ? CallerThen->LinkedTo[0] : nullptr;
    TestNotNull(TEXT("Caller upstream exec still points to a macro input pin"), RewiredMacroExecIn);
    if (!RewiredMacroExecIn)
    {
        return false;
    }

    TestEqual(TEXT("Caller upstream exec points at the reconstructed macro node"),
        RewiredMacroExecIn->GetOwningNode(), static_cast<UEdGraphNode*>(ReconstructedCaller));
    TestEqual(TEXT("Caller upstream exec target is still an input pin"),
        RewiredMacroExecIn->Direction, EGPD_Input);
    TestEqual(TEXT("Caller upstream exec target is still exec-typed"),
        RewiredMacroExecIn->PinType.PinCategory, UEdGraphSchema_K2::PC_Exec);

    {
        FBpirCompiler Compiler(BP);
        FCompileResult Result = Compiler.Compile(
            TEXT("entry macro TestMacro(int Value, text Label, object<Object> Target) -> (float Result) {\n")
            TEXT("    %converted = pure Conv_IntToFloat(InInt: $Value)\n")
            TEXT("    return (Result: %converted)\n")
            TEXT("}"),
            EBpirCompileMode::Replace);
        TestFalse(TEXT("Changing an existing caller pin type fails instead of mutating callers"),
            Result.bSuccess);
        if (Result.bSuccess)
        {
            return false;
        }
    }

    UK2Node_MacroInstance* CallerAfterFailedRecompile = Cast<UK2Node_MacroInstance>(
        FindNodeByGuid(EventGraph, CallerMacroGuid));
    TestNotNull(TEXT("Caller node still exists after failed signature recompile"),
        CallerAfterFailedRecompile);
    if (!CallerAfterFailedRecompile)
    {
        return false;
    }

    TestEqual(TEXT("Caller remains bound to restored macro graph after failed recompile"),
        CallerAfterFailedRecompile->GetMacroGraph(), RecompiledMacroGraph);

    UEdGraphPin* RestoredValuePin = CallerAfterFailedRecompile->FindPin(TEXT("Value"), EGPD_Input);
    UEdGraphPin* RestoredLabelPin = CallerAfterFailedRecompile->FindPin(TEXT("Label"), EGPD_Input);
    UEdGraphPin* RestoredTargetPin = CallerAfterFailedRecompile->FindPin(TEXT("Target"), EGPD_Input);
    UEdGraphPin* RestoredResultPin = CallerAfterFailedRecompile->FindPin(TEXT("Result"), EGPD_Output);
    TestNotNull(TEXT("Value pin survives failed signature recompile"), RestoredValuePin);
    TestNotNull(TEXT("Label pin survives failed signature recompile"), RestoredLabelPin);
    TestNotNull(TEXT("Target object pin survives failed signature recompile"), RestoredTargetPin);
    TestNotNull(TEXT("Defaultless result pin survives failed signature recompile"), RestoredResultPin);
    if (!RestoredValuePin || !RestoredLabelPin || !RestoredTargetPin || !RestoredResultPin)
    {
        return false;
    }

    TestTrue(TEXT("Value pin type survives failed signature recompile"),
        RestoredValuePin->PinType == OriginalValuePinType);
    TestTrue(TEXT("Label pin type survives failed signature recompile"),
        RestoredLabelPin->PinType == OriginalLabelPinType);
    TestTrue(TEXT("Target object pin type survives failed signature recompile"),
        RestoredTargetPin->PinType == OriginalTargetPinType);
    TestTrue(TEXT("Result pin type survives failed signature recompile"),
        RestoredResultPin->PinType == OriginalResultPinType);
    TestEqual(TEXT("Value default survives failed signature recompile"),
        RestoredValuePin->DefaultValue,
        FString(TEXT("5.0")));
    TestTrue(TEXT("Label FText identity survives failed signature recompile"),
        TextIdentityMatches(RestoredLabelPin->DefaultTextValue, OriginalLabelText));
    MCP_TestSamePtr(TEXT("Target DefaultObject survives failed signature recompile"),
        RestoredTargetPin->DefaultObject.Get(), static_cast<UObject*>(BP));
    TestEqual(TEXT("Value source output still has one link after failed signature recompile"),
        FloatSourceOut->LinkedTo.Num(), 1);
    MCP_TestSamePtr(TEXT("Value source still targets the restored macro value pin"),
        FloatSourceOut->LinkedTo.Num() > 0 ? FloatSourceOut->LinkedTo[0] : nullptr,
        RestoredValuePin);

    TestEqual(TEXT("Caller upstream exec still has one link after failed signature recompile"),
        CallerThen->LinkedTo.Num(), 1);
    UEdGraphPin* RestoredMacroExecIn = CallerThen->LinkedTo.Num() > 0 ? CallerThen->LinkedTo[0] : nullptr;
    TestNotNull(TEXT("Caller upstream exec still points to a restored macro input pin"),
        RestoredMacroExecIn);
    if (!RestoredMacroExecIn)
    {
        return false;
    }
    TestEqual(TEXT("Caller upstream exec points at restored macro node"),
        RestoredMacroExecIn->GetOwningNode(), static_cast<UEdGraphNode*>(CallerAfterFailedRecompile));
    TestEqual(TEXT("Caller upstream exec target remains input after failed signature recompile"),
        RestoredMacroExecIn->Direction, EGPD_Input);
    TestEqual(TEXT("Caller upstream exec target remains exec-typed after failed signature recompile"),
        RestoredMacroExecIn->PinType.PinCategory, UEdGraphSchema_K2::PC_Exec);

    return true;
}
