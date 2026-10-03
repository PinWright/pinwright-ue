// Copyright (c) 2026 Alexander Penkin. MIT License.

// Property-chain references in two positions the value path did not cover:
//
// - `%r = get $obj.Prop` must bind the register (B-bpir-get-property-into-register-unresolved).
//   The parser used to build a self-member VariableGet literally named "$obj.Prop", which has
//   no output pin, so the NEXT line failed with "Could not resolve value '%r'". It now parses
//   as an alias of the value reference, the same path the inline value-position form takes.
// - `Target: %ref.Prop` must scope member-function lookup to the chain's leaf class
//   (B-bpir-member-call-through-chained-property). ResolveViaTargetArg used to fall back to
//   the producer's primary pin, picking the wrong class: "Unresolved function", or a
//   same-named function on the calling Blueprint that then failed wiring Target -> self.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Camera/CameraComponent.h"
#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

using namespace CompilerTestUtils;

namespace TestBpirChainedPropertyRefsHelpers
{
    static void AddObjectVariable(UBlueprint* BP, const TCHAR* Name, UClass* Class)
    {
        FEdGraphPinType Type;
        Type.PinCategory = UEdGraphSchema_K2::PC_Object;
        Type.PinSubCategoryObject = Class;
        FBlueprintEditorUtils::AddMemberVariable(BP, Name, Type);
    }

    static UK2Node_VariableGet* FindVariableGet(UEdGraph* Graph, FName MemberName)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(Node);
            if (Get && Get->VariableReference.GetMemberName() == MemberName)
            {
                return Get;
            }
        }
        return nullptr;
    }

    static UK2Node_CallFunction* FindCall(UEdGraph* Graph, FName FunctionName)
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

    static UEdGraph* FindFunctionGraph(UBlueprint* BP, const TCHAR* Name)
    {
        for (UEdGraph* Graph : BP->FunctionGraphs)
        {
            if (Graph && Graph->GetFName() == FName(Name))
            {
                return Graph;
            }
        }
        return nullptr;
    }

    static void ReportErrors(FAutomationTestBase& Test, const FCompileResult& Result)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            Test.AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
        }
    }

    // A callee Blueprint with a member function the library/broad search cannot find
    // (non-static), held through a property of a second "holder" Blueprint.
    struct FChainFixture
    {
        UBlueprint* CalleeBP = nullptr;
        UBlueprint* HolderBP = nullptr;
    };

    static bool BuildChainFixture(FAutomationTestBase& Test, FChainFixture& Out)
    {
        Out.CalleeBP = CreateTransientTestBP(TEXT("ChainCalleeBP"));
        if (!Out.CalleeBP)
        {
            Test.AddError(TEXT("Fixture: callee Blueprint was not created"));
            return false;
        }
        FBpirCompiler CalleeCompiler(Out.CalleeBP);
        const FCompileResult CalleeResult = CalleeCompiler.Compile(
            TEXT("entry function ChainCalleeFn(int Count) {\n")
            TEXT("    call PrintString(InString: \"callee\")\n")
            TEXT("    return\n")
            TEXT("}\n"));
        ReportErrors(Test, CalleeResult);
        FKismetEditorUtilities::CompileBlueprint(Out.CalleeBP);
        if (!CalleeResult.bSuccess || !Out.CalleeBP->GeneratedClass
            || !Out.CalleeBP->GeneratedClass->FindFunctionByName(TEXT("ChainCalleeFn")))
        {
            Test.AddError(TEXT("Fixture: callee class does not expose ChainCalleeFn"));
            return false;
        }

        Out.HolderBP = CreateTransientTestBP(TEXT("ChainHolderBP"));
        if (!Out.HolderBP)
        {
            Test.AddError(TEXT("Fixture: holder Blueprint was not created"));
            return false;
        }
        AddObjectVariable(Out.HolderBP, TEXT("CalleeRef"), Out.CalleeBP->GeneratedClass);
        FKismetEditorUtilities::CompileBlueprint(Out.HolderBP);
        if (!Out.HolderBP->GeneratedClass
            || !FindFProperty<FObjectProperty>(Out.HolderBP->GeneratedClass, TEXT("CalleeRef")))
        {
            Test.AddError(TEXT("Fixture: holder class has no CalleeRef property"));
            return false;
        }
        return true;
    }

    // Asserts the ChainCalleeFn call in Graph is bound to the callee class, carries its
    // Count parameter, and takes its Target from the CalleeRef VariableGet.
    static void ExpectCallBoundThroughChain(FAutomationTestBase& Test, UEdGraph* Graph, const FChainFixture& Fixture)
    {
        UK2Node_CallFunction* Call = Graph ? FindCall(Graph, TEXT("ChainCalleeFn")) : nullptr;
        Test.TestNotNull(TEXT("ChainCalleeFn call node emitted"), Call);
        if (!Call) return;

        const UFunction* Bound = Call->GetTargetFunction();
        const UClass* Owner = Bound ? Bound->GetOuterUClass() : nullptr;
        Test.TestTrue(TEXT("Call is bound to the callee Blueprint's function, not a same-named one elsewhere"),
            Owner && Owner->ClassGeneratedBy == Fixture.CalleeBP);

        UEdGraphPin* CountPin = Call->FindPin(TEXT("Count"), EGPD_Input);
        Test.TestNotNull(TEXT("Call carries the callee's Count parameter"), CountPin);
        if (CountPin)
        {
            Test.TestEqual(TEXT("Count literal applied"), CountPin->DefaultValue, FString(TEXT("2")));
        }

        UEdGraphPin* SelfPin = Call->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
        UK2Node_VariableGet* SourceGet = (SelfPin && SelfPin->LinkedTo.Num() == 1)
            ? Cast<UK2Node_VariableGet>(SelfPin->LinkedTo[0]->GetOwningNode())
            : nullptr;
        Test.TestTrue(TEXT("Target is wired from the CalleeRef property read"),
            SourceGet && SourceGet->VariableReference.GetMemberName() == FName(TEXT("CalleeRef")));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirGetDollarChainBindsRegisterTest,
    "PinWright.bpir.compiler.chained_property.GetDollarChainBindsRegister",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirGetDollarChainBindsRegisterTest::RunTest(const FString& Parameters)
{
    using namespace TestBpirChainedPropertyRefsHelpers;

    UBlueprint* BP = CreateTransientTestBP(TEXT("GetChainRegisterBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    AddObjectVariable(BP, TEXT("Cam"), UCameraComponent::StaticClass());
    FEdGraphPinType DoubleType;
    DoubleType.PinCategory = UEdGraphSchema_K2::PC_Real;
    DoubleType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("StoredFov"), DoubleType);

    FBpirCompiler Compiler(BP);
    const FCompileResult Result = Compiler.Compile(
        TEXT("entry function UpdateFov() {\n")
        TEXT("    %fov = get $Cam.FieldOfView\n")
        TEXT("    %half = pure Multiply_DoubleDouble(A: %fov, B: 0.5)\n")
        TEXT("    set StoredFov = %half\n")
        TEXT("    return\n")
        TEXT("}\n"));
    ReportErrors(*this, Result);
    TestTrue(TEXT("`%r = get $obj.Prop` compiles and its register is readable"), Result.bSuccess);

    UEdGraph* Graph = FindFunctionGraph(BP, TEXT("UpdateFov"));
    TestNotNull(TEXT("UpdateFov graph exists"), Graph);
    if (!Graph) return false;

    TestNull(TEXT("No self-member VariableGet named after the whole chain"),
        FindVariableGet(Graph, TEXT("$Cam.FieldOfView")));

    UK2Node_VariableGet* FovGet = FindVariableGet(Graph, TEXT("FieldOfView"));
    UK2Node_CallFunction* Multiply = FindCall(Graph, TEXT("Multiply_DoubleDouble"));
    TestNotNull(TEXT("External FieldOfView VariableGet emitted"), FovGet);
    TestNotNull(TEXT("Multiply node emitted"), Multiply);
    if (!FovGet || !Multiply) return false;

    UEdGraphPin* APin = Multiply->FindPin(TEXT("A"), EGPD_Input);
    UEdGraphPin* FovOut = FovGet->FindPin(TEXT("FieldOfView"), EGPD_Output);
    TestTrue(TEXT("Register %fov is wired from the FieldOfView read into Multiply.A"),
        APin && FovOut && APin->LinkedTo.Contains(FovOut));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirMemberCallTargetThroughChainTest,
    "PinWright.bpir.compiler.chained_property.MemberCallTargetThroughChain",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirMemberCallTargetThroughChainTest::RunTest(const FString& Parameters)
{
    using namespace TestBpirChainedPropertyRefsHelpers;

    FChainFixture Fixture;
    if (!BuildChainFixture(*this, Fixture)) return false;

    UBlueprint* CallerBP = CreateTransientTestBP(TEXT("ChainCallerBP"));
    TestNotNull(TEXT("Caller Blueprint was created"), CallerBP);
    if (!CallerBP) return false;
    AddObjectVariable(CallerBP, TEXT("HolderRef"), AActor::StaticClass());

    FBpirCompiler Compiler(CallerBP);
    const FCompileResult Result = Compiler.Compile(FString::Printf(
        TEXT("entry function DriveChain() {\n")
        TEXT("    %%m = cast<%s>($HolderRef)\n")
        TEXT("    call ChainCalleeFn(Target: %%m.CalleeRef, Count: 2)\n")
        TEXT("    return\n")
        TEXT("}\n"),
        *Fixture.HolderBP->GeneratedClass->GetName()));
    ReportErrors(*this, Result);
    TestTrue(TEXT("Member call through `Target: %ref.Prop` resolves on the chain's class"), Result.bSuccess);

    ExpectCallBoundThroughChain(*this, FindFunctionGraph(CallerBP, TEXT("DriveChain")), Fixture);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirMemberCallChainBeatsLocalSameNameTest,
    "PinWright.bpir.compiler.chained_property.ChainTargetBeatsSameNamedLocalFunction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirMemberCallChainBeatsLocalSameNameTest::RunTest(const FString& Parameters)
{
    using namespace TestBpirChainedPropertyRefsHelpers;

    FChainFixture Fixture;
    if (!BuildChainFixture(*this, Fixture)) return false;

    UBlueprint* CallerBP = CreateTransientTestBP(TEXT("ChainSameNameCallerBP"));
    TestNotNull(TEXT("Caller Blueprint was created"), CallerBP);
    if (!CallerBP) return false;
    AddObjectVariable(CallerBP, TEXT("HolderRef"), AActor::StaticClass());

    // The caller owns a parameterless ChainCalleeFn of its own. The Target's class must
    // win; binding the local one would wire the CalleeRef object into its `self` pin.
    FBpirCompiler Compiler(CallerBP);
    const FCompileResult Result = Compiler.Compile(FString::Printf(
        TEXT("entry function ChainCalleeFn() {\n")
        TEXT("    return\n")
        TEXT("}\n")
        TEXT("\n")
        TEXT("entry function DriveChain() {\n")
        TEXT("    %%m = cast<%s>($HolderRef)\n")
        TEXT("    call ChainCalleeFn(Target: %%m.CalleeRef, Count: 2)\n")
        TEXT("    return\n")
        TEXT("}\n"),
        *Fixture.HolderBP->GeneratedClass->GetName()));
    ReportErrors(*this, Result);
    TestTrue(TEXT("Same-named local function does not capture a chained Target"), Result.bSuccess);

    ExpectCallBoundThroughChain(*this, FindFunctionGraph(CallerBP, TEXT("DriveChain")), Fixture);
    return true;
}

// An alias register (`%m = get $Holder.Prop`) emits no node; as a member-call Target it
// must still scope the lookup to the aliased value's class.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirMemberCallTargetThroughAliasTest,
    "PinWright.bpir.compiler.chained_property.MemberCallTargetThroughGetAlias",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirMemberCallTargetThroughAliasTest::RunTest(const FString& Parameters)
{
    using namespace TestBpirChainedPropertyRefsHelpers;

    FChainFixture Fixture;
    if (!BuildChainFixture(*this, Fixture)) return false;

    UBlueprint* CallerBP = CreateTransientTestBP(TEXT("ChainAliasCallerBP"));
    TestNotNull(TEXT("Caller Blueprint was created"), CallerBP);
    if (!CallerBP) return false;
    AddObjectVariable(CallerBP, TEXT("HolderRef"), Fixture.HolderBP->GeneratedClass);

    FBpirCompiler Compiler(CallerBP);
    const FCompileResult Result = Compiler.Compile(
        TEXT("entry function DriveChain() {\n")
        TEXT("    %m = get $HolderRef.CalleeRef\n")
        TEXT("    call ChainCalleeFn(Target: %m, Count: 2)\n")
        TEXT("    return\n")
        TEXT("}\n"));
    ReportErrors(*this, Result);
    TestTrue(TEXT("Member call through a `get` alias Target resolves on the aliased class"), Result.bSuccess);

    ExpectCallBoundThroughChain(*this, FindFunctionGraph(CallerBP, TEXT("DriveChain")), Fixture);
    return true;
}

// A value parameter literally named `Target` (FInterpTo's float Target) fed by a non-object
// chain is not a caller object. Resolving the Target class must stay silent: a logged
// `LogBpirCompiler: Error` here fails this test through the automation log hook even though
// the compile itself succeeds.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirValueParamNamedTargetSilentTest,
    "PinWright.bpir.compiler.chained_property.ValueParamNamedTargetStaysSilent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirValueParamNamedTargetSilentTest::RunTest(const FString& Parameters)
{
    using namespace TestBpirChainedPropertyRefsHelpers;

    UBlueprint* BP = CreateTransientTestBP(TEXT("ValueTargetChainBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;
    FEdGraphPinType DoubleType;
    DoubleType.PinCategory = UEdGraphSchema_K2::PC_Real;
    DoubleType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("StoredX"), DoubleType);

    FBpirCompiler Compiler(BP);
    const FCompileResult Result = Compiler.Compile(
        TEXT("entry function InterpX() {\n")
        TEXT("    %v = pure GetActorLocation(Target: self)\n")
        TEXT("    %r = pure FInterpTo(Current: 0.0, Target: %v.X, DeltaTime: 0.1, InterpSpeed: 1.0)\n")
        TEXT("    set StoredX = %r\n")
        TEXT("    return\n")
        TEXT("}\n"));
    ReportErrors(*this, Result);
    TestTrue(TEXT("FInterpTo with `Target: %v.X` compiles"), Result.bSuccess);

    UEdGraph* Graph = FindFunctionGraph(BP, TEXT("InterpX"));
    UK2Node_CallFunction* Interp = Graph ? FindCall(Graph, TEXT("FInterpTo")) : nullptr;
    TestNotNull(TEXT("FInterpTo node emitted"), Interp);
    UEdGraphPin* TargetPin = Interp ? Interp->FindPin(TEXT("Target"), EGPD_Input) : nullptr;
    TestTrue(TEXT("FInterpTo's value pin Target is wired from the X member"),
        TargetPin && TargetPin->LinkedTo.Num() == 1);
    return true;
}

// Same as above for `$` refs: a float member and a float property chain fed into the
// value parameter `Target` must not log `ResolveTargetClass: ... not an object class`.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirValueParamNamedTargetDollarSilentTest,
    "PinWright.bpir.compiler.chained_property.DollarValueParamNamedTargetStaysSilent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirValueParamNamedTargetDollarSilentTest::RunTest(const FString& Parameters)
{
    using namespace TestBpirChainedPropertyRefsHelpers;

    UBlueprint* BP = CreateTransientTestBP(TEXT("DollarValueTargetBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;
    FEdGraphPinType DoubleType;
    DoubleType.PinCategory = UEdGraphSchema_K2::PC_Real;
    DoubleType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("Goal"), DoubleType);
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("StoredA"), DoubleType);
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("StoredB"), DoubleType);
    AddObjectVariable(BP, TEXT("Cam"), UCameraComponent::StaticClass());

    FBpirCompiler Compiler(BP);
    const FCompileResult Result = Compiler.Compile(
        TEXT("entry function InterpGoal() {\n")
        TEXT("    %a = pure FInterpTo(Current: 0.0, Target: $Goal, DeltaTime: 0.1, InterpSpeed: 1.0)\n")
        TEXT("    set StoredA = %a\n")
        TEXT("    %b = pure FInterpTo(Current: 0.0, Target: $Cam.FieldOfView, DeltaTime: 0.1, InterpSpeed: 1.0)\n")
        TEXT("    set StoredB = %b\n")
        TEXT("    return\n")
        TEXT("}\n"));
    ReportErrors(*this, Result);
    TestTrue(TEXT("FInterpTo with `$` value Targets compiles"), Result.bSuccess);

    UEdGraph* Graph = FindFunctionGraph(BP, TEXT("InterpGoal"));
    int32 WiredTargets = 0;
    if (Graph)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
            if (Call && Call->FunctionReference.GetMemberName() == FName(TEXT("FInterpTo")))
            {
                UEdGraphPin* TargetPin = Call->FindPin(TEXT("Target"), EGPD_Input);
                WiredTargets += (TargetPin && TargetPin->LinkedTo.Num() == 1) ? 1 : 0;
            }
        }
    }
    TestEqual(TEXT("Both FInterpTo value Targets are wired"), WiredTargets, 2);
    return true;
}
