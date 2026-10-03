// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-bpir-cannot-assign-interface-variable: a Blueprint member
// variable typed to a Blueprint Interface (PC_Interface) must be writable from BPIR.
//
// CastRhsCompiles: `set Receiver = cast<Iface_C>($Source)` wires the pure cast's
// interface-typed result pin into the Set node and the Blueprint compiles.
//
// ObjectRhsNamesCastRoute: `set Receiver = $Source` with a plain Actor source cannot
// connect -- the K2 schema has no object -> interface conversion node (the editor
// refuses the same drag) -- so the compile must fail and the diagnostic must name the
// schema's reason and the cast<> route. Counterfactual: drop the hint from
// FBpirCompiler::WireDataPins and the "cast<" assertion fails.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Interface.h"
#include "UObject/Package.h"

using namespace CompilerTestUtils;

namespace TestBpirSetInterfaceVariableHelpers
{
    // A transient Blueprint Interface plus an Actor Blueprint holding one member
    // variable `Receiver` typed to that interface (the editor's own interface-variable shape).
    struct FFixture
    {
        UBlueprint* InterfaceBP = nullptr;
        UClass* InterfaceClass = nullptr;
        UBlueprint* BP = nullptr;
    };

    static bool MakeFixture(FAutomationTestBase& Test, const TCHAR* Prefix, FFixture& Out)
    {
        Out.InterfaceBP = FKismetEditorUtilities::CreateBlueprint(
            UInterface::StaticClass(),
            GetTransientPackage(),
            MakeUniqueTestBPName(FString::Printf(TEXT("%sIface"), Prefix)),
            BPTYPE_Interface,
            UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass());
        if (!Test.TestNotNull(TEXT("Blueprint Interface was created"), Out.InterfaceBP)) return false;
        FKismetEditorUtilities::CompileBlueprint(Out.InterfaceBP);
        Out.InterfaceClass = Out.InterfaceBP->GeneratedClass;
        if (!Test.TestTrue(TEXT("Interface generated class is a UInterface"),
            Out.InterfaceClass && Out.InterfaceClass->IsChildOf(UInterface::StaticClass())))
        {
            return false;
        }

        Out.BP = CreateTransientTestBP(FString::Printf(TEXT("%sHolder"), Prefix));
        if (!Test.TestNotNull(TEXT("Holder Blueprint was created"), Out.BP)) return false;

        FEdGraphPinType InterfaceType;
        InterfaceType.PinCategory = UEdGraphSchema_K2::PC_Interface;
        InterfaceType.PinSubCategoryObject = Out.InterfaceClass;
        if (!Test.TestTrue(TEXT("Receiver interface variable added"),
            FBlueprintEditorUtils::AddMemberVariable(Out.BP, TEXT("Receiver"), InterfaceType)))
        {
            return false;
        }
        FKismetEditorUtilities::CompileBlueprint(Out.BP);
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirSetInterfaceVariableCastRhsTest,
    "PinWright.bpir.compile.SetInterfaceVariable.CastRhsCompiles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirSetInterfaceVariableCastRhsTest::RunTest(const FString& Parameters)
{
    TestBpirSetInterfaceVariableHelpers::FFixture Fx;
    if (!TestBpirSetInterfaceVariableHelpers::MakeFixture(*this, TEXT("BpirSetIfaceCast"), Fx)) return false;

    const FString Source = FString::Printf(
        TEXT("entry function Apply(object<Object> Source) {\n")
        TEXT("    set Receiver = cast<%s>($Source)\n")
        TEXT("}"),
        *Fx.InterfaceClass->GetName());

    FBpirCompiler Compiler(Fx.BP);
    const FCompileResult Result = Compiler.Compile(Source);
    for (const FCompileError& Err : Result.Errors)
    {
        AddError(FString::Printf(TEXT("BPIR compile error L%d: %s"), Err.Line, *Err.Message));
    }
    if (!TestTrue(TEXT("BPIR compiled `set Receiver = cast<Iface_C>($Source)`"), Result.bSuccess)) return false;

    UEdGraph* FunctionGraph = nullptr;
    for (UEdGraph* Graph : Fx.BP->FunctionGraphs)
    {
        if (Graph && Graph->GetFName() == FName(TEXT("Apply")))
        {
            FunctionGraph = Graph;
        }
    }
    if (!TestNotNull(TEXT("Apply function graph exists"), FunctionGraph)) return false;

    UK2Node_VariableSet* SetNode = nullptr;
    for (UEdGraphNode* Node : FunctionGraph->Nodes)
    {
        if (UK2Node_VariableSet* Set = Cast<UK2Node_VariableSet>(Node))
        {
            SetNode = Set;
        }
    }
    if (!TestNotNull(TEXT("Receiver Set node exists"), SetNode)) return false;

    UEdGraphPin* ValuePin = SetNode->FindPin(TEXT("Receiver"), EGPD_Input);
    if (!TestNotNull(TEXT("Set node has the Receiver input pin"), ValuePin)) return false;
    TestEqual(TEXT("Receiver pin is interface-typed"),
        ValuePin->PinType.PinCategory, UEdGraphSchema_K2::PC_Interface);
    if (!TestEqual(TEXT("Receiver pin has exactly one link"), ValuePin->LinkedTo.Num(), 1)) return false;

    UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(ValuePin->LinkedTo[0]->GetOwningNode());
    if (!TestNotNull(TEXT("Receiver is fed by a dynamic cast"), CastNode)) return false;
    TestTrue(TEXT("Cast is pure"), CastNode->IsNodePure());
    TestEqual(TEXT("Cast targets the interface class"), CastNode->TargetType.Get(), Fx.InterfaceClass);

    BlueprintHandlerUtils::FBlueprintCompileDiagnostics Diagnostics =
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(Fx.BP);
    for (const FString& Err : Diagnostics.Errors)
    {
        AddError(FString::Printf(TEXT("Post-BPIR full compile error: %s"), *Err));
    }
    TestTrue(TEXT("Post-BPIR full compile succeeded"), Diagnostics.bCompiled);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirSetInterfaceVariableObjectRhsTest,
    "PinWright.bpir.compile.SetInterfaceVariable.ObjectRhsNamesCastRoute",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirSetInterfaceVariableObjectRhsTest::RunTest(const FString& Parameters)
{
    TestBpirSetInterfaceVariableHelpers::FFixture Fx;
    if (!TestBpirSetInterfaceVariableHelpers::MakeFixture(*this, TEXT("BpirSetIfaceObj"), Fx)) return false;

    FBpirCompiler Compiler(Fx.BP);
    const FCompileResult Result = Compiler.Compile(
        TEXT("entry function ApplyRaw(object<Actor> Source) {\n")
        TEXT("    set Receiver = $Source\n")
        TEXT("}"));

    FString Joined;
    for (const FCompileError& Err : Result.Errors)
    {
        Joined += FString::Printf(TEXT("[L%d] %s\n"), Err.Line, *Err.Message);
    }
    AddInfo(FString::Printf(TEXT("Diagnostics:\n%s"), *Joined));

    TestFalse(TEXT("An Actor (no interface implemented) cannot be assigned to an interface variable"),
        Result.bSuccess);
    TestTrue(TEXT("Diagnostic reports the failed data wire"),
        ErrorsContain(Result.Errors, TEXT("TryCreateConnection failed wiring data")));
    TestTrue(TEXT("Diagnostic names the cast<> route"),
        ErrorsContain(Result.Errors, TEXT("cast<InterfaceClass>")));
    return true;
}
