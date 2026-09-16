// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBpirExpression.cpp - Unit tests for UK2Node_BpirExpression (BPIR expression node)

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "K2Node_BpirExpression.h"
#include "Compiler/BpirSubgraphCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "K2Node_Tunnel.h"
#include "K2Node_FunctionResult.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"

using namespace CompilerTestUtils;

// ============================================================================
// Helpers
// ============================================================================

namespace
{
    // Create a BpirExpression node inside the first ubergraph page of the
    // given Blueprint.  PostPlacedNewNode() sets up BoundGraph + tunnels.
    UK2Node_BpirExpression* CreateBpirExpressionNode(UBlueprint* BP)
    {
        if (!BP || BP->UbergraphPages.Num() == 0)
        {
            return nullptr;
        }

        UEdGraph* Graph = BP->UbergraphPages[0];
        UK2Node_BpirExpression* Node = NewObject<UK2Node_BpirExpression>(Graph);
        Node->CreateNewGuid();
        Node->PostPlacedNewNode();
        Node->AllocateDefaultPins();
        Graph->AddNode(Node, /*bFromUI=*/true, /*bSelectNewNode=*/false);
        return Node;
    }

    // Count outer pins matching a specific category and direction.
    int32 CountPins(UK2Node_BpirExpression* Node, const FName& Category, EEdGraphPinDirection Dir)
    {
        int32 Count = 0;
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->PinType.PinCategory == Category && Pin->Direction == Dir)
            {
                Count++;
            }
        }
        return Count;
    }

    // Find an outer pin by name and direction, returns nullptr if not found.
    UEdGraphPin* FindOuterPin(UK2Node_BpirExpression* Node, const FString& PinName, EEdGraphPinDirection Dir)
    {
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->PinName.ToString() == PinName && Pin->Direction == Dir)
            {
                return Pin;
            }
        }
        return nullptr;
    }
} // namespace

// ============================================================================
// 1. BpirExpression.PureArithmetic
// Pure expression with two float inputs and one output — no exec pins.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirExpressionPureArithmeticTest,
    "PinWright.bpir.expression.PureArithmetic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirExpressionPureArithmeticTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirExprTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    UK2Node_BpirExpression* Node = CreateBpirExpressionNode(BP);
    TestNotNull(TEXT("BpirExpression node was created"), Node);
    if (!Node) return false;

    // Verify BoundGraph and tunnels were created by PostPlacedNewNode
    TestNotNull(TEXT("BoundGraph exists"), Node->BoundGraph.Get());
    TestNotNull(TEXT("Entry tunnel exists"), Node->GetEntryNode());
    TestNotNull(TEXT("Exit tunnel exists"), Node->GetExitNode());

    Node->BpirText = TEXT(
        "input X: float\n"
        "input Y: float\n"
        "output Result\n"
        "---\n"
        "%Result = pure Add_FloatFloat(A: $X, B: $Y)");

    Node->ReconstructNode();

    // Check node is pure (no exec pins)
    TestTrue(TEXT("Node is pure"), Node->IsNodePure());
    TestFalse(TEXT("No compiler error"), Node->bHasCompilerMessage);

    // Check outer pins: should have 2 float (PC_Real) input data pins and 1 output data pin
    int32 InputDataPins = CountPins(Node, UEdGraphSchema_K2::PC_Real, EGPD_Input);
    int32 OutputDataPins = CountPins(Node, UEdGraphSchema_K2::PC_Real, EGPD_Output)
        + CountPins(Node, UEdGraphSchema_K2::PC_Wildcard, EGPD_Output);
    int32 ExecInputPins = CountPins(Node, UEdGraphSchema_K2::PC_Exec, EGPD_Input);
    int32 ExecOutputPins = CountPins(Node, UEdGraphSchema_K2::PC_Exec, EGPD_Output);

    TestEqual(TEXT("2 input data pins"), InputDataPins, 2);
    TestTrue(TEXT("At least 1 output data pin"), OutputDataPins >= 1);
    TestEqual(TEXT("No exec input pins"), ExecInputPins, 0);
    TestEqual(TEXT("No exec output pins"), ExecOutputPins, 0);

    // Verify pin names
    TestNotNull(TEXT("Input pin X exists"), FindOuterPin(Node, TEXT("X"), EGPD_Input));
    TestNotNull(TEXT("Input pin Y exists"), FindOuterPin(Node, TEXT("Y"), EGPD_Input));
    TestNotNull(TEXT("Output pin Result exists"), FindOuterPin(Node, TEXT("Result"), EGPD_Output));
    return true;
}

// ============================================================================
// 2. BpirExpression.ImpureWithExec
// Impure expression with exec flow — should have exec pins.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirExpressionImpureWithExecTest,
    "PinWright.bpir.expression.ImpureWithExec",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirExpressionImpureWithExecTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirExprTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    UK2Node_BpirExpression* Node = CreateBpirExpressionNode(BP);
    TestNotNull(TEXT("BpirExpression node was created"), Node);
    if (!Node) return false;

    Node->BpirText = TEXT(
        "input Message: FString\n"
        "output Result\n"
        "---\n"
        "call PrintString(InString: $Message)");

    Node->ReconstructNode();

    // Node should be impure (has exec pins)
    TestFalse(TEXT("Node is NOT pure"), Node->IsNodePure());

    // Should have exec in + exec out
    int32 ExecInputPins = CountPins(Node, UEdGraphSchema_K2::PC_Exec, EGPD_Input);
    int32 ExecOutputPins = CountPins(Node, UEdGraphSchema_K2::PC_Exec, EGPD_Output);
    TestTrue(TEXT("Has exec input pin"), ExecInputPins >= 1);
    TestTrue(TEXT("Has exec output pin"), ExecOutputPins >= 1);

    // Should have at least 1 data input pin (Message)
    TestNotNull(TEXT("Input pin Message exists"), FindOuterPin(Node, TEXT("Message"), EGPD_Input));
    return true;
}

// ============================================================================
// 3. BpirExpression.EmptyText
// Empty BpirText — only default tunnel pins, no errors.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirExpressionEmptyTextTest,
    "PinWright.bpir.expression.EmptyText",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirExpressionEmptyTextTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirExprTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    UK2Node_BpirExpression* Node = CreateBpirExpressionNode(BP);
    TestNotNull(TEXT("BpirExpression node was created"), Node);
    if (!Node) return false;

    Node->BpirText = TEXT("");
    Node->ReconstructNode();

    // No error for empty text
    TestFalse(TEXT("No compiler error"), Node->bHasCompilerMessage);
    TestTrue(TEXT("ErrorMsg is empty"), Node->ErrorMsg.IsEmpty());

    // No user-defined data or exec pins on outer node
    int32 ExecPins = CountPins(Node, UEdGraphSchema_K2::PC_Exec, EGPD_Input)
        + CountPins(Node, UEdGraphSchema_K2::PC_Exec, EGPD_Output);
    TestEqual(TEXT("No exec pins"), ExecPins, 0);
    return true;
}

// ============================================================================
// 4. BpirExpression.ParseError
// Invalid BPIR text — node should have error state.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirExpressionParseErrorTest,
    "PinWright.bpir.expression.ParseError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirExpressionParseErrorTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirExprTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    UK2Node_BpirExpression* Node = CreateBpirExpressionNode(BP);
    TestNotNull(TEXT("BpirExpression node was created"), Node);
    if (!Node) return false;

    Node->BpirText = TEXT("invalid garbage @@@@");
    Node->ReconstructNode();

    // Node should report an error
    TestTrue(TEXT("Has compiler error"), Node->bHasCompilerMessage);
    TestFalse(TEXT("ErrorMsg is not empty"), Node->ErrorMsg.IsEmpty());
    return true;
}

// ============================================================================
// 5. BpirExpression.TextUpdate
// Changing BpirText and rebuilding should update pin count.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirExpressionTextUpdateTest,
    "PinWright.bpir.expression.TextUpdate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirExpressionTextUpdateTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirExprTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    UK2Node_BpirExpression* Node = CreateBpirExpressionNode(BP);
    TestNotNull(TEXT("BpirExpression node was created"), Node);
    if (!Node) return false;

    // First expression: 1 input, 1 output
    Node->BpirText = TEXT(
        "input X: float\n"
        "output Result\n"
        "---\n"
        "%Result = pure Abs(A: $X)");
    Node->ReconstructNode();

    int32 PinCount1 = Node->Pins.Num();

    // Second expression: 2 inputs, 2 outputs — different pin count
    Node->BpirText = TEXT(
        "input X: float\n"
        "input Y: float\n"
        "output Sum\n"
        "output Product\n"
        "---\n"
        "%Sum = pure Add_FloatFloat(A: $X, B: $Y)\n"
        "%Product = pure Multiply_FloatFloat(A: $X, B: $Y)");
    Node->ReconstructNode();

    int32 PinCount2 = Node->Pins.Num();

    TestNotEqual(TEXT("Pin count changed after updating BpirText"), PinCount1, PinCount2);
    return true;
}

// ============================================================================
// 6. BpirExpression.MultipleOutputs
// Expression with two output pins.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirExpressionMultipleOutputsTest,
    "PinWright.bpir.expression.MultipleOutputs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirExpressionMultipleOutputsTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirExprTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    UK2Node_BpirExpression* Node = CreateBpirExpressionNode(BP);
    TestNotNull(TEXT("BpirExpression node was created"), Node);
    if (!Node) return false;

    Node->BpirText = TEXT(
        "input X: float\n"
        "input Y: float\n"
        "output Sum\n"
        "output Product\n"
        "---\n"
        "%Sum = pure Add_FloatFloat(A: $X, B: $Y)\n"
        "%Product = pure Multiply_FloatFloat(A: $X, B: $Y)");

    Node->ReconstructNode();

    // Should have output data pins named "Sum" and "Product"
    UEdGraphPin* SumPin = FindOuterPin(Node, TEXT("Sum"), EGPD_Output);
    UEdGraphPin* ProductPin = FindOuterPin(Node, TEXT("Product"), EGPD_Output);

    TestNotNull(TEXT("Output pin Sum exists"), SumPin);
    TestNotNull(TEXT("Output pin Product exists"), ProductPin);
    return true;
}

// ============================================================================
// 7. BpirExpression.DefaultValues
// Input pin with a default value specified in the header.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirExpressionDefaultValuesTest,
    "PinWright.bpir.expression.DefaultValues",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirExpressionDefaultValuesTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirExprTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    UK2Node_BpirExpression* Node = CreateBpirExpressionNode(BP);
    TestNotNull(TEXT("BpirExpression node was created"), Node);
    if (!Node) return false;

    Node->BpirText = TEXT(
        "input X: float = 10.0\n"
        "output Result\n"
        "---\n"
        "%Result = pure Multiply_FloatFloat(A: $X, B: 2.0)");

    Node->ReconstructNode();

    // The default value should be set on the entry tunnel's pin
    UK2Node_Tunnel* Entry = Node->GetEntryNode();
    TestNotNull(TEXT("Entry tunnel exists"), Entry);
    if (Entry)
    {
        UEdGraphPin* XPin = Entry->FindPin(FName(TEXT("X")), EGPD_Output);
        TestNotNull(TEXT("Entry tunnel has X pin"), XPin);
        if (XPin)
        {
            TestEqual(TEXT("Default value is 10.0"), XPin->DefaultValue, TEXT("10.0"));
        }
    }

    // Also check the outer pin exists
    TestNotNull(TEXT("Outer input pin X exists"), FindOuterPin(Node, TEXT("X"), EGPD_Input));
    return true;
}

// ============================================================================
// 8. BpirExpression.DeclarationParsing
// Test declaration parsing through the full node — verifies header parsing
// produces correct tunnel pin names, types, and direction.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirExpressionDeclarationParsingTest,
    "PinWright.bpir.expression.DeclarationParsing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirExpressionDeclarationParsingTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirExprTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    UK2Node_BpirExpression* Node = CreateBpirExpressionNode(BP);
    TestNotNull(TEXT("BpirExpression node was created"), Node);
    if (!Node) return false;

    Node->BpirText = TEXT(
        "input Foo: float\n"
        "output Bar: bool\n"
        "input Baz: FVector = 1,2,3\n"
        "---\n"
        "%Bar = pure IsNearlyZero(Value: $Foo)");

    Node->ReconstructNode();

    UK2Node_Tunnel* Entry = Node->GetEntryNode();
    UK2Node_Tunnel* Exit = Node->GetExitNode();
    TestNotNull(TEXT("Entry tunnel"), Entry);
    TestNotNull(TEXT("Exit tunnel"), Exit);

    if (Entry)
    {
        // "input Foo: float" -> entry tunnel output pin named "Foo" with float type
        UEdGraphPin* FooPin = Entry->FindPin(FName(TEXT("Foo")), EGPD_Output);
        TestNotNull(TEXT("Entry has Foo pin"), FooPin);
        if (FooPin)
        {
            TestEqual(TEXT("Foo pin is real (float)"), FooPin->PinType.PinCategory, UEdGraphSchema_K2::PC_Real);
        }

        // "input Baz: FVector = 1,2,3" -> entry tunnel output pin with default
        UEdGraphPin* BazPin = Entry->FindPin(FName(TEXT("Baz")), EGPD_Output);
        TestNotNull(TEXT("Entry has Baz pin"), BazPin);
        if (BazPin)
        {
            TestEqual(TEXT("Baz default value"), BazPin->DefaultValue, TEXT("1,2,3"));
        }
    }

    if (Exit)
    {
        // "output Bar: bool" -> exit tunnel input pin named "Bar" with bool type
        UEdGraphPin* BarPin = Exit->FindPin(FName(TEXT("Bar")), EGPD_Input);
        TestNotNull(TEXT("Exit has Bar pin"), BarPin);
        if (BarPin)
        {
            TestEqual(TEXT("Bar pin is bool"), BarPin->PinType.PinCategory, UEdGraphSchema_K2::PC_Boolean);
        }
    }

    // Verify outer pins reflect the declarations
    TestNotNull(TEXT("Outer input Foo"), FindOuterPin(Node, TEXT("Foo"), EGPD_Input));
    TestNotNull(TEXT("Outer output Bar"), FindOuterPin(Node, TEXT("Bar"), EGPD_Output));
    TestNotNull(TEXT("Outer input Baz"), FindOuterPin(Node, TEXT("Baz"), EGPD_Input));
    return true;
}

// ============================================================================
// 9. BpirExpression.NodeTitle
// GetNodeTitle should contain "BPIR Expression".
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirExpressionNodeTitleTest,
    "PinWright.bpir.expression.NodeTitle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirExpressionNodeTitleTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirExprTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    UK2Node_BpirExpression* Node = CreateBpirExpressionNode(BP);
    TestNotNull(TEXT("BpirExpression node was created"), Node);
    if (!Node) return false;

    // Empty text — title should contain "BPIR Expression"
    Node->BpirText = TEXT("");
    FText EmptyTitle = Node->GetNodeTitle(ENodeTitleType::ListView);
    TestTrue(TEXT("Empty title contains 'BPIR Expression'"),
        EmptyTitle.ToString().Contains(TEXT("BPIR Expression")));

    // Non-empty text — full title should contain "BPIR Expression"
    Node->BpirText = TEXT(
        "input X: float\n"
        "---\n"
        "%Result = pure Abs(A: $X)");

    FText FullTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle);
    TestTrue(TEXT("Full title contains 'BPIR Expression'"),
        FullTitle.ToString().Contains(TEXT("BPIR Expression")));
    return true;
}

// ============================================================================
// 10. BpirExpression.CannotCreateUserPin
// User-defined pin creation should be disallowed — pins are managed by BPIR text.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirExpressionCannotCreateUserPinTest,
    "PinWright.bpir.expression.CannotCreateUserPin",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirExpressionCannotCreateUserPinTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirExprTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    UK2Node_BpirExpression* Node = CreateBpirExpressionNode(BP);
    TestNotNull(TEXT("BpirExpression node was created"), Node);
    if (!Node) return false;

    FEdGraphPinType FloatPinType;
    FloatPinType.PinCategory = UEdGraphSchema_K2::PC_Real;

    FText ErrorMessage;
    bool bCanCreate = Node->CanCreateUserDefinedPin(FloatPinType, EGPD_Input, ErrorMessage);

    TestFalse(TEXT("Cannot create user-defined pin"), bCanCreate);
    TestFalse(TEXT("Error message is not empty"), ErrorMessage.IsEmpty());
    return true;
}

// ============================================================================
// 11. BpirExpression.TypedInputSoftObject
// Declaration "input Ref: softobject<AActor>" creates a PC_SoftObject input pin.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirExpressionTypedSoftObjectTest,
    "PinWright.bpir.expression.TypedInputSoftObject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirExpressionTypedSoftObjectTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirExprTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    UK2Node_BpirExpression* Node = CreateBpirExpressionNode(BP);
    TestNotNull(TEXT("BpirExpression node was created"), Node);
    if (!Node) return false;

    // Body uses IsValid(self) — a known-working pure call that doesn't
    // reference the soft object input. The test is about pin type creation, not
    // BPIR body compilation of soft object operations.
    Node->BpirText = TEXT(
        "input Ref: softobject<AActor>\n"
        "output Result\n"
        "---\n"
        "%Result = pure IsValid(Object: self)");

    Node->ReconstructNode();

    TestFalse(TEXT("No compiler error"), Node->bHasCompilerMessage);

    // Verify the outer input pin exists
    UEdGraphPin* RefPin = FindOuterPin(Node, TEXT("Ref"), EGPD_Input);
    TestNotNull(TEXT("Input pin Ref exists"), RefPin);
    if (RefPin)
    {
        TestEqual(TEXT("Ref pin is PC_SoftObject"), RefPin->PinType.PinCategory, UEdGraphSchema_K2::PC_SoftObject);
    }

    // Verify the entry tunnel also has the typed pin
    UK2Node_Tunnel* Entry = Node->GetEntryNode();
    TestNotNull(TEXT("Entry tunnel exists"), Entry);
    if (Entry)
    {
        UEdGraphPin* EntryRefPin = Entry->FindPin(FName(TEXT("Ref")), EGPD_Output);
        TestNotNull(TEXT("Entry tunnel has Ref pin"), EntryRefPin);
        if (EntryRefPin)
        {
            TestEqual(TEXT("Entry Ref pin is PC_SoftObject"), EntryRefPin->PinType.PinCategory, UEdGraphSchema_K2::PC_SoftObject);
        }
    }
    return true;
}

// ============================================================================
// 12. BpirExpression.TypedInputInterface
// Declaration "input Iface: interface<UBlendableInterface>" creates a PC_Interface
// input pin. Must use a real UInterface subclass, not a regular UClass like AActor.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirExpressionTypedInterfaceTest,
    "PinWright.bpir.expression.TypedInputInterface",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirExpressionTypedInterfaceTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirExprTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    UK2Node_BpirExpression* Node = CreateBpirExpressionNode(BP);
    TestNotNull(TEXT("BpirExpression node was created"), Node);
    if (!Node) return false;

    // Use UBlendableInterface — a real UInterface subclass (AActor is not an interface)
    Node->BpirText = TEXT(
        "input Iface: interface<UBlendableInterface>\n"
        "output Result\n"
        "---\n"
        "%Result = pure IsValid(Object: self)");

    Node->ReconstructNode();

    TestFalse(TEXT("No compiler error"), Node->bHasCompilerMessage);

    UEdGraphPin* IfacePin = FindOuterPin(Node, TEXT("Iface"), EGPD_Input);
    TestNotNull(TEXT("Input pin Iface exists"), IfacePin);
    if (IfacePin)
    {
        TestEqual(TEXT("Iface pin is PC_Interface"), IfacePin->PinType.PinCategory, UEdGraphSchema_K2::PC_Interface);
    }
    return true;
}

// ============================================================================
// 13. BpirExpression.WildcardNoType
// Declaration "input X" with no type annotation creates a PC_Wildcard pin.
// This is the wildcard type behavior — empty TypeString maps to PC_Wildcard.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirExpressionWildcardNoTypeTest,
    "PinWright.bpir.expression.WildcardNoType",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirExpressionWildcardNoTypeTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirExprTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    UK2Node_BpirExpression* Node = CreateBpirExpressionNode(BP);
    TestNotNull(TEXT("BpirExpression node was created"), Node);
    if (!Node) return false;

    Node->BpirText = TEXT(
        "input X\n"
        "output Result\n"
        "---\n"
        "%Result = pure IsValid(Object: self)");

    Node->ReconstructNode();

    TestFalse(TEXT("No compiler error"), Node->bHasCompilerMessage);

    // Input with no type should be wildcard — body doesn't reference $X so
    // the pin stays wildcard (no connection to retype it).
    UEdGraphPin* XPin = FindOuterPin(Node, TEXT("X"), EGPD_Input);
    TestNotNull(TEXT("Input pin X exists"), XPin);
    if (XPin)
    {
        TestEqual(TEXT("X pin is PC_Wildcard"), XPin->PinType.PinCategory, UEdGraphSchema_K2::PC_Wildcard);
    }

    // Output with no type starts as wildcard but gets retyped to match the
    // source node's output during output wiring (IsValid returns bool).
    UEdGraphPin* ResultPin = FindOuterPin(Node, TEXT("Result"), EGPD_Output);
    TestNotNull(TEXT("Output pin Result exists"), ResultPin);
    if (ResultPin)
    {
        TestEqual(TEXT("Result pin is PC_Boolean (retyped from wildcard)"),
            ResultPin->PinType.PinCategory, UEdGraphSchema_K2::PC_Boolean);
    }
    return true;
}

// ============================================================================
// 14. BpirExpression.TypedInputFRotator
// Declaration "input Rot: FRotator" creates a PC_Struct pin with FRotator type.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirExpressionTypedFRotatorTest,
    "PinWright.bpir.expression.TypedInputFRotator",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirExpressionTypedFRotatorTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirExprTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    UK2Node_BpirExpression* Node = CreateBpirExpressionNode(BP);
    TestNotNull(TEXT("BpirExpression node was created"), Node);
    if (!Node) return false;

    // Body uses IsValid(self) — a known-working pure call that doesn't
    // reference the struct input. The test is about pin type creation, not
    // BPIR body compilation of struct operations.
    Node->BpirText = TEXT(
        "input Rot: FRotator\n"
        "output Result\n"
        "---\n"
        "%Result = pure IsValid(Object: self)");

    Node->ReconstructNode();

    TestFalse(TEXT("No compiler error"), Node->bHasCompilerMessage);

    UEdGraphPin* RotPin = FindOuterPin(Node, TEXT("Rot"), EGPD_Input);
    TestNotNull(TEXT("Input pin Rot exists"), RotPin);
    if (RotPin)
    {
        TestEqual(TEXT("Rot pin is PC_Struct"), RotPin->PinType.PinCategory, UEdGraphSchema_K2::PC_Struct);
        TestTrue(TEXT("Rot SubCategoryObject is FRotator"),
            RotPin->PinType.PinSubCategoryObject.Get() == TBaseStructure<FRotator>::Get());
    }
    return true;
}

// ============================================================================
// 15. BpirExpression.TypedInputFLinearColor
// Declaration "input Col: FLinearColor" creates a PC_Struct pin with FLinearColor type.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirExpressionTypedFLinearColorTest,
    "PinWright.bpir.expression.TypedInputFLinearColor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirExpressionTypedFLinearColorTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirExprTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    UK2Node_BpirExpression* Node = CreateBpirExpressionNode(BP);
    TestNotNull(TEXT("BpirExpression node was created"), Node);
    if (!Node) return false;

    // Body uses IsValid(self) — a known-working pure call that doesn't
    // reference the struct input. The test is about pin type creation, not
    // BPIR body compilation of struct operations.
    Node->BpirText = TEXT(
        "input Col: FLinearColor\n"
        "output Result\n"
        "---\n"
        "%Result = pure IsValid(Object: self)");

    Node->ReconstructNode();

    TestFalse(TEXT("No compiler error"), Node->bHasCompilerMessage);

    UEdGraphPin* ColPin = FindOuterPin(Node, TEXT("Col"), EGPD_Input);
    TestNotNull(TEXT("Input pin Col exists"), ColPin);
    if (ColPin)
    {
        TestEqual(TEXT("Col pin is PC_Struct"), ColPin->PinType.PinCategory, UEdGraphSchema_K2::PC_Struct);
        TestTrue(TEXT("Col SubCategoryObject is FLinearColor"),
            ColPin->PinType.PinSubCategoryObject.Get() == TBaseStructure<FLinearColor>::Get());
    }
    return true;
}

// ============================================================================
// 16. BpirExpression.TypedOutputBool
// Declaration "output Flag: bool" creates a PC_Boolean output pin.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirExpressionTypedOutputBoolTest,
    "PinWright.bpir.expression.TypedOutputBool",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirExpressionTypedOutputBoolTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirExprTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    UK2Node_BpirExpression* Node = CreateBpirExpressionNode(BP);
    TestNotNull(TEXT("BpirExpression node was created"), Node);
    if (!Node) return false;

    // Use IsValid(self) as the body — returns bool and is known to compile.
    // IsNearlyZero is not a valid Blueprint function name and causes compilation
    // errors. The test is about output pin type creation, not BPIR body semantics.
    Node->BpirText = TEXT(
        "input X: float\n"
        "output Flag: bool\n"
        "---\n"
        "%Flag = pure IsValid(Object: self)");

    Node->ReconstructNode();

    TestFalse(TEXT("No compiler error"), Node->bHasCompilerMessage);

    UEdGraphPin* FlagPin = FindOuterPin(Node, TEXT("Flag"), EGPD_Output);
    TestNotNull(TEXT("Output pin Flag exists"), FlagPin);
    if (FlagPin)
    {
        TestEqual(TEXT("Flag pin is PC_Boolean"), FlagPin->PinType.PinCategory, UEdGraphSchema_K2::PC_Boolean);
    }
    return true;
}

// ============================================================================
// 17. BpirExpression.TypedInputDelegate
// Declaration "input Del: delegate" creates a PC_Delegate input pin.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirExpressionTypedDelegateTest,
    "PinWright.bpir.expression.TypedInputDelegate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirExpressionTypedDelegateTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirExprTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    UK2Node_BpirExpression* Node = CreateBpirExpressionNode(BP);
    TestNotNull(TEXT("BpirExpression node was created"), Node);
    if (!Node) return false;

    Node->BpirText = TEXT(
        "input Del: delegate\n"
        "output Result\n"
        "---\n"
        "%Result = pure IsValid(Object: self)");

    Node->ReconstructNode();

    TestFalse(TEXT("No compiler error"), Node->bHasCompilerMessage);

    UEdGraphPin* DelPin = FindOuterPin(Node, TEXT("Del"), EGPD_Input);
    TestNotNull(TEXT("Input pin Del exists"), DelPin);
    if (DelPin)
    {
        TestEqual(TEXT("Del pin is PC_Delegate"), DelPin->PinType.PinCategory, UEdGraphSchema_K2::PC_Delegate);
    }
    return true;
}

// ============================================================================
// 18. BpirExpression.TypedInputMCDelegate
// Declaration "input Evt: mcdelegate" creates a PC_MCDelegate input pin.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirExpressionTypedMCDelegateTest,
    "PinWright.bpir.expression.TypedInputMCDelegate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirExpressionTypedMCDelegateTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirExprTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    UK2Node_BpirExpression* Node = CreateBpirExpressionNode(BP);
    TestNotNull(TEXT("BpirExpression node was created"), Node);
    if (!Node) return false;

    Node->BpirText = TEXT(
        "input Evt: mcdelegate\n"
        "output Result\n"
        "---\n"
        "%Result = pure IsValid(Object: self)");

    Node->ReconstructNode();

    TestFalse(TEXT("No compiler error"), Node->bHasCompilerMessage);

    UEdGraphPin* EvtPin = FindOuterPin(Node, TEXT("Evt"), EGPD_Input);
    TestNotNull(TEXT("Input pin Evt exists"), EvtPin);
    if (EvtPin)
    {
        TestEqual(TEXT("Evt pin is PC_MCDelegate"), EvtPin->PinType.PinCategory, UEdGraphSchema_K2::PC_MCDelegate);
    }
    return true;
}

// ============================================================================
// 19. BpirExpression.TypedInputSoftClass
// Declaration "input Cls: softclass<AActor>" creates a PC_SoftClass input pin.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirExpressionTypedSoftClassTest,
    "PinWright.bpir.expression.TypedInputSoftClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirExpressionTypedSoftClassTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirExprTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    UK2Node_BpirExpression* Node = CreateBpirExpressionNode(BP);
    TestNotNull(TEXT("BpirExpression node was created"), Node);
    if (!Node) return false;

    Node->BpirText = TEXT(
        "input Cls: softclass<AActor>\n"
        "output Result\n"
        "---\n"
        "%Result = pure IsValid(Object: self)");

    Node->ReconstructNode();

    TestFalse(TEXT("No compiler error"), Node->bHasCompilerMessage);

    UEdGraphPin* ClsPin = FindOuterPin(Node, TEXT("Cls"), EGPD_Input);
    TestNotNull(TEXT("Input pin Cls exists"), ClsPin);
    if (ClsPin)
    {
        TestEqual(TEXT("Cls pin is PC_SoftClass"), ClsPin->PinType.PinCategory, UEdGraphSchema_K2::PC_SoftClass);
    }
    return true;
}

// ============================================================================
// 20. BpirExpression.MultipleTypedDeclarations
// Multiple different types in a single expression — verifies that the
// declaration parser and type converter handle mixed types correctly.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirExpressionMultiTypedDeclsTest,
    "PinWright.bpir.expression.MultipleTypedDeclarations",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirExpressionMultiTypedDeclsTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirExprTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    UK2Node_BpirExpression* Node = CreateBpirExpressionNode(BP);
    TestNotNull(TEXT("BpirExpression node was created"), Node);
    if (!Node) return false;

    // Body uses IsValid(self) — a known-working pure call that doesn't
    // reference any typed input. The test is about pin type creation for
    // multiple declarations, not BPIR body compilation.
    Node->BpirText = TEXT(
        "input Pos: FVector\n"
        "input Rot: FRotator\n"
        "input Col: FLinearColor\n"
        "input Name: FName\n"
        "input Label: FText\n"
        "input Ref: object<AActor>\n"
        "output Result\n"
        "---\n"
        "%Result = pure IsValid(Object: self)");

    Node->ReconstructNode();

    TestFalse(TEXT("No compiler error"), Node->bHasCompilerMessage);

    // Verify each input pin exists and has the correct category
    UEdGraphPin* PosPin = FindOuterPin(Node, TEXT("Pos"), EGPD_Input);
    TestNotNull(TEXT("Input pin Pos exists"), PosPin);
    if (PosPin)
    {
        TestEqual(TEXT("Pos is PC_Struct"), PosPin->PinType.PinCategory, UEdGraphSchema_K2::PC_Struct);
    }

    UEdGraphPin* RotPin = FindOuterPin(Node, TEXT("Rot"), EGPD_Input);
    TestNotNull(TEXT("Input pin Rot exists"), RotPin);
    if (RotPin)
    {
        TestEqual(TEXT("Rot is PC_Struct"), RotPin->PinType.PinCategory, UEdGraphSchema_K2::PC_Struct);
    }

    UEdGraphPin* ColPin = FindOuterPin(Node, TEXT("Col"), EGPD_Input);
    TestNotNull(TEXT("Input pin Col exists"), ColPin);
    if (ColPin)
    {
        TestEqual(TEXT("Col is PC_Struct"), ColPin->PinType.PinCategory, UEdGraphSchema_K2::PC_Struct);
    }

    UEdGraphPin* NamePin = FindOuterPin(Node, TEXT("Name"), EGPD_Input);
    TestNotNull(TEXT("Input pin Name exists"), NamePin);
    if (NamePin)
    {
        TestEqual(TEXT("Name is PC_Name"), NamePin->PinType.PinCategory, UEdGraphSchema_K2::PC_Name);
    }

    UEdGraphPin* LabelPin = FindOuterPin(Node, TEXT("Label"), EGPD_Input);
    TestNotNull(TEXT("Input pin Label exists"), LabelPin);
    if (LabelPin)
    {
        TestEqual(TEXT("Label is PC_Text"), LabelPin->PinType.PinCategory, UEdGraphSchema_K2::PC_Text);
    }

    UEdGraphPin* RefPin = FindOuterPin(Node, TEXT("Ref"), EGPD_Input);
    TestNotNull(TEXT("Input pin Ref exists"), RefPin);
    if (RefPin)
    {
        TestEqual(TEXT("Ref is PC_Object"), RefPin->PinType.PinCategory, UEdGraphSchema_K2::PC_Object);
    }
    return true;
}

// ============================================================================
// 21. BpirExpression.ReturnWithOutput
// `return $Input` in an expression with a declared output should compile
// without errors — the return wires to the exit tunnel, not a FunctionResult.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirExpressionReturnWithOutputTest,
    "PinWright.bpir.expression.ReturnWithOutput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirExpressionReturnWithOutputTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirExprTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    UK2Node_BpirExpression* Node = CreateBpirExpressionNode(BP);
    TestNotNull(TEXT("BpirExpression node was created"), Node);
    if (!Node) return false;

    // return is impure, so the expression will have exec pins.
    // The declared output "Result" creates an exit tunnel input pin.
    // The return statement should wire $Input -> exit tunnel's Result pin.
    Node->BpirText = TEXT(
        "input Input: float\n"
        "output Result\n"
        "---\n"
        "return $Input");

    Node->ReconstructNode();

    TestFalse(TEXT("No compiler error"), Node->bHasCompilerMessage);
    TestTrue(TEXT("ErrorMsg is empty"), Node->ErrorMsg.IsEmpty());

    // Should have exec + data pins
    TestNotNull(TEXT("Input pin Input exists"), FindOuterPin(Node, TEXT("Input"), EGPD_Input));
    TestNotNull(TEXT("Output pin Result exists"), FindOuterPin(Node, TEXT("Result"), EGPD_Output));

    // The exit tunnel should have the Result pin wired (not a stray FunctionResult)
    UK2Node_Tunnel* Exit = Node->GetExitNode();
    TestNotNull(TEXT("Exit tunnel exists"), Exit);
    if (Exit)
    {
        UEdGraphPin* ExitResultPin = Exit->FindPin(FName(TEXT("Result")), EGPD_Input);
        TestNotNull(TEXT("Exit tunnel has Result pin"), ExitResultPin);
    }

    // No UK2Node_FunctionResult should exist in the BoundGraph
    TestNull(TEXT("BoundGraph must not contain a FunctionResult node"),
        FindNodeOfType<UK2Node_FunctionResult>(Node->BoundGraph));
    return true;
}

// ============================================================================
// 22. BpirExpression.ReturnWithoutOutput
// `return $Input` with no declared output — return has nowhere to wire,
// should not crash (pre-fix: would create a FunctionResult and fail).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirExpressionReturnWithoutOutputTest,
    "PinWright.bpir.expression.ReturnWithoutOutput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirExpressionReturnWithoutOutputTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirExprTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    UK2Node_BpirExpression* Node = CreateBpirExpressionNode(BP);
    TestNotNull(TEXT("BpirExpression node was created"), Node);
    if (!Node) return false;

    // No output declaration — exit tunnel has no data input pin.
    // `return $Input` will try to wire to the exit tunnel but find no data pin.
    // This should produce an error, not crash by creating a FunctionResult.
    Node->BpirText = TEXT("return $Input");

    Node->ReconstructNode();

    // The node should exist and not have crashed.
    // It may or may not report an error (the return has no output pin to wire
    // to on the exit tunnel), but it must not create a FunctionResult node.
    TestNull(TEXT("BoundGraph must not contain a FunctionResult node"),
        FindNodeOfType<UK2Node_FunctionResult>(Node->BoundGraph));
    return true;
}

// ============================================================================
// 23. BpirExpression.MultipleBpirNodesNoCrash
// Creating multiple BPIR expression nodes in the same Blueprint should not
// crash (pre-fix: BoundGraph->Rename with hardcoded name caused a fatal
// assert on the second node).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirExpressionMultipleNodesNoCrashTest,
    "PinWright.bpir.expression.MultipleBpirNodesNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirExpressionMultipleNodesNoCrashTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirExprTestBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // Create first node — should succeed
    UK2Node_BpirExpression* Node1 = CreateBpirExpressionNode(BP);
    TestNotNull(TEXT("First BpirExpression node was created"), Node1);

    // Create second node — pre-fix this would crash with fatal assert in
    // UObject::Rename because "BpirExpressionGraph" already existed
    UK2Node_BpirExpression* Node2 = CreateBpirExpressionNode(BP);
    TestNotNull(TEXT("Second BpirExpression node was created"), Node2);

    // Both should have valid BoundGraphs with distinct names
    if (Node1 && Node2 && Node1->BoundGraph && Node2->BoundGraph)
    {
        TestNotEqual(TEXT("BoundGraph names are distinct"),
            Node1->BoundGraph->GetName(), Node2->BoundGraph->GetName());
    }

    // Create a third to be thorough
    UK2Node_BpirExpression* Node3 = CreateBpirExpressionNode(BP);
    TestNotNull(TEXT("Third BpirExpression node was created"), Node3);
    return true;
}
