// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-bpir-const-ref-fname-literal-dropped: a literal passed to a
// by-reference input (`const T&` without AutoCreateRefTerm) cannot live in the pin's
// DefaultValue -- the editor hides the field and the Blueprint compiler rejects it with
// '"by ref" params expect a valid input'. BPIR must feed it through a MakeLiteral* node,
// or refuse with a reason when no MakeLiteral exists for the type.
//
// NameLiteralWiredThroughMakeLiteral: `SetValueAsName(KeyName: "TargetKey", ...)` on a
// Blackboard component. Counterfactual: revert the WireDataPins by-ref branch and the
// KeyName pin is left unlinked with a default, the link assertion fails and the full
// Blueprint compile reports the by-ref error.
//
// StructLiteralRefusedWithReason: `FTruncVector(InVector: FVector(...))` has no MakeLiteral
// route; BPIR must fail naming the pin instead of handing the UE compiler a dead default.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "BehaviorTree/BlackboardComponent.h"
#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

using namespace CompilerTestUtils;

namespace TestBpirByRefLiteralHelpers
{
    static UK2Node_CallFunction* FindCallTo(UEdGraph* Graph, FName FunctionName)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
            if (Call && Call->FunctionReference.GetMemberName() == FunctionName)
            {
                return Call;
            }
        }
        return nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirByRefNameLiteralTest,
    "PinWright.bpir.compile.ByRefLiteral.NameLiteralWiredThroughMakeLiteral",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirByRefNameLiteralTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirByRefNameLiteralBP"));
    if (!TestNotNull(TEXT("Blueprint was created"), BP)) return false;

    FEdGraphPinType BlackboardType;
    BlackboardType.PinCategory = UEdGraphSchema_K2::PC_Object;
    BlackboardType.PinSubCategoryObject = UBlackboardComponent::StaticClass();
    if (!TestTrue(TEXT("BB member variable added"),
        FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("BB"), BlackboardType)))
    {
        return false;
    }
    FKismetEditorUtilities::CompileBlueprint(BP);

    FBpirCompiler Compiler(BP);
    const FCompileResult Result = Compiler.Compile(
        TEXT("entry function WriteKey() {\n")
        TEXT("    call SetValueAsName(Target: $BB, KeyName: \"TargetKey\", NameValue: \"Hello\")\n")
        TEXT("}"));
    for (const FCompileError& Err : Result.Errors)
    {
        AddError(FString::Printf(TEXT("BPIR compile error L%d: %s"), Err.Line, *Err.Message));
    }
    if (!TestTrue(TEXT("BPIR compile succeeded"), Result.bSuccess)) return false;

    UEdGraph* Graph = nullptr;
    for (UEdGraph* Candidate : BP->FunctionGraphs)
    {
        if (Candidate && Candidate->GetFName() == FName(TEXT("WriteKey")))
        {
            Graph = Candidate;
        }
    }
    if (!TestNotNull(TEXT("WriteKey function graph exists"), Graph)) return false;

    UK2Node_CallFunction* SetCall = TestBpirByRefLiteralHelpers::FindCallTo(Graph, TEXT("SetValueAsName"));
    if (!TestNotNull(TEXT("SetValueAsName call node exists"), SetCall)) return false;

    UEdGraphPin* KeyPin = SetCall->FindPin(TEXT("KeyName"), EGPD_Input);
    UEdGraphPin* ValuePin = SetCall->FindPin(TEXT("NameValue"), EGPD_Input);
    if (!TestNotNull(TEXT("KeyName pin exists"), KeyPin) || !TestNotNull(TEXT("NameValue pin exists"), ValuePin))
    {
        return false;
    }
    // Fixture precondition: the pin under test really is a by-ref input the schema refuses a default on.
    if (!TestTrue(TEXT("Precondition: KeyName is a by-reference pin"), KeyPin->PinType.bIsReference)) return false;

    if (!TestEqual(TEXT("KeyName is wired (one link)"), KeyPin->LinkedTo.Num(), 1)) return false;
    UK2Node_CallFunction* LiteralCall = Cast<UK2Node_CallFunction>(KeyPin->LinkedTo[0]->GetOwningNode());
    if (!TestNotNull(TEXT("KeyName is fed by a call node"), LiteralCall)) return false;
    TestEqual(TEXT("KeyName is fed by MakeLiteralName"),
        LiteralCall->FunctionReference.GetMemberName(), FName(TEXT("MakeLiteralName")));
    UEdGraphPin* LiteralValuePin = LiteralCall->FindPin(TEXT("Value"), EGPD_Input);
    if (!TestNotNull(TEXT("MakeLiteralName has a Value pin"), LiteralValuePin)) return false;
    TestEqual(TEXT("MakeLiteralName carries the literal"), LiteralValuePin->DefaultValue, FString(TEXT("TargetKey")));

    // Negative control: a by-value pin keeps its literal default and gets no extra node.
    TestEqual(TEXT("NameValue (by value) stays unlinked"), ValuePin->LinkedTo.Num(), 0);
    TestEqual(TEXT("NameValue (by value) keeps its default"), ValuePin->DefaultValue, FString(TEXT("Hello")));

    BlueprintHandlerUtils::FBlueprintCompileDiagnostics Diagnostics =
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(BP);
    for (const FString& Err : Diagnostics.Errors)
    {
        AddError(FString::Printf(TEXT("Post-BPIR full compile error: %s"), *Err));
    }
    TestTrue(TEXT("Post-BPIR full compile succeeded"), Diagnostics.bCompiled);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirByRefStructLiteralTest,
    "PinWright.bpir.compile.ByRefLiteral.StructLiteralRefusedWithReason",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirByRefStructLiteralTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirByRefStructLiteralBP"));
    if (!TestNotNull(TEXT("Blueprint was created"), BP)) return false;

    FBpirCompiler Compiler(BP);
    const FCompileResult Result = Compiler.Compile(
        TEXT("entry function Trunc() {\n")
        TEXT("    %t = call FTruncVector(InVector: FVector(1.5, 2.5, 3.5))\n")
        TEXT("}"));

    FString Joined;
    for (const FCompileError& Err : Result.Errors)
    {
        Joined += FString::Printf(TEXT("[L%d] %s\n"), Err.Line, *Err.Message);
    }
    AddInfo(FString::Printf(TEXT("Diagnostics:\n%s"), *Joined));

    TestFalse(TEXT("A struct literal on a by-ref pin is refused at the BPIR layer"), Result.bSuccess);
    TestTrue(TEXT("Diagnostic names the by-reference pin"),
        ErrorsContain(Result.Errors, TEXT("Pin 'InVector' is a by-reference parameter")));
    return true;
}

// A non-const ref (UPARAM(ref)) is written through by the callee: feeding it a MakeLiteral
// temporary would compile and silently drop the write, so BPIR refuses the literal.
// Counterfactual: drop the bIsConst gate in WireDataPins and ReplaceInline compiles with a
// MakeLiteralString feeding SourceString, failing both assertions.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirByRefInOutLiteralTest,
    "PinWright.bpir.compile.ByRefLiteral.InOutRefLiteralRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirByRefInOutLiteralTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirByRefInOutLiteralBP"));
    if (!TestNotNull(TEXT("Blueprint was created"), BP)) return false;

    FBpirCompiler Compiler(BP);
    const FCompileResult Result = Compiler.Compile(
        TEXT("entry function Rep() {\n")
        TEXT("    %n = call ReplaceInline(SourceString: \"abc\", SearchText: \"a\", ReplacementText: \"b\")\n")
        TEXT("}"));

    FString Joined;
    for (const FCompileError& Err : Result.Errors)
    {
        Joined += FString::Printf(TEXT("[L%d] %s\n"), Err.Line, *Err.Message);
    }
    AddInfo(FString::Printf(TEXT("Diagnostics:\n%s"), *Joined));

    TestFalse(TEXT("A literal on a UPARAM(ref) in/out pin is refused"), Result.bSuccess);
    TestTrue(TEXT("Diagnostic names the in/out pin"),
        ErrorsContain(Result.Errors, TEXT("Pin 'SourceString' is a by-reference in/out parameter")));
    return true;
}
