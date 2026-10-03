// Copyright (c) 2026 Alexander Penkin. MIT License.

// E-bpir-cast-accessor-spelling-undocumented: a cast result pin is "As" + the class display
// name, so a Blueprint class B_Foo_Bar_C gives the pin "AsB Foo Bar". Callers guessed
// `%c.AsB_Foo_Bar_C`, got a bare "Could not resolve value", and the refused-wire diagnostic
// printed the raw spaced pin name, which does not compile unquoted.
//
// AcceptedSpellingsCompile pins the four spellings the wiki documents (space-stripped,
// backtick-quoted spaced, the .Result alias, the bare register). Counterfactual for .Result:
// count the impure cast's hidden bSuccess pin in FBpirValueResolver::FindOutputPinByName's
// alias fallback again and the `%typed.Result` case fails.
// MisspelledAccessorListsOutputs: counterfactual — drop the output list from
// FBpirCompiler::WireDataPins and the "outputs of %typed" assertion fails.
// RefusedWireNamesCompilableAccessor: counterfactual — print SourcePin->PinName again and
// the space-stripped assertion fails.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"
#include "Tests/TestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "GameFramework/Actor.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

using namespace CompilerTestUtils;

namespace TestBpirCastAccessorSpellingHelpers
{
    // An Actor Blueprint whose class display name contains spaces, and the cast pin names
    // UK2Node_DynamicCast derives from it.
    struct FCastTarget
    {
        UClass* Class = nullptr;
        FString SpacedPin;     // "AsB Test Cast Target 0"
        FString CanonicalPin;  // "AsBTestCastTarget0"
    };

    bool MakeCastTarget(FAutomationTestBase& Test, FCastTarget& Out)
    {
        UBlueprint* TargetBP = CreateTransientTestBP(TEXT("B_Test_Cast_Target"));
        if (!Test.TestNotNull(TEXT("Cast target Blueprint created"), TargetBP)) return false;
        FKismetEditorUtilities::CompileBlueprint(TargetBP);
        Out.Class = TargetBP->GeneratedClass;
        if (!Test.TestNotNull(TEXT("Cast target generated class exists"), Out.Class)) return false;

        Out.SpacedPin = UEdGraphSchema_K2::PN_CastedValuePrefix + Out.Class->GetDisplayNameText().ToString();
        Out.CanonicalPin = Out.SpacedPin.Replace(TEXT(" "), TEXT(""));
        return Test.TestTrue(FString::Printf(TEXT("Cast pin name '%s' is multi-word (fixture precondition)"), *Out.SpacedPin),
            Out.SpacedPin.Contains(TEXT(" ")));
    }

    // Host Blueprint with `Base` (Actor) and one extra member variable.
    UBlueprint* MakeHost(FAutomationTestBase& Test, const FName VarName, const FEdGraphPinType& VarType)
    {
        UBlueprint* HostBP = CreateTransientTestBP(TEXT("CastAccessorSpellingHost"));
        if (!Test.TestNotNull(TEXT("Host Blueprint created"), HostBP)) return nullptr;

        FEdGraphPinType BaseType;
        BaseType.PinCategory = UEdGraphSchema_K2::PC_Object;
        BaseType.PinSubCategoryObject = AActor::StaticClass();
        FBlueprintEditorUtils::AddMemberVariable(HostBP, TEXT("Base"), BaseType);
        FBlueprintEditorUtils::AddMemberVariable(HostBP, VarName, VarType);
        FKismetEditorUtilities::CompileBlueprint(HostBP);
        return HostBP;
    }

    FString MakeSource(const UClass* CastClass, const FString& VarName, const FString& Value)
    {
        return FString::Printf(
            TEXT("entry event BeginPlay() {\n")
            TEXT("    %%typed = cast<%s>($Base) [success -> @ok, fail -> @done]\n")
            TEXT("@ok:\n")
            TEXT("    set %s = %s\n")
            TEXT("@done:\n")
            TEXT("}"),
            *CastClass->GetName(), *VarName, *Value);
    }

    FString JoinErrors(const FCompileResult& Result)
    {
        FString Joined;
        for (const FCompileError& Err : Result.Errors)
        {
            Joined += FString::Printf(TEXT("[L%d] %s\n"), Err.Line, *Err.Message);
        }
        return Joined;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirCastAccessorSpellingAcceptedTest,
    "PinWright.bpir.compile.CastAccessorSpelling.AcceptedSpellingsCompile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirCastAccessorSpellingAcceptedTest::RunTest(const FString& Parameters)
{
    using namespace TestBpirCastAccessorSpellingHelpers;
    FCastTarget Target;
    if (!MakeCastTarget(*this, Target)) return false;

    FEdGraphPinType TypedVar;
    TypedVar.PinCategory = UEdGraphSchema_K2::PC_Object;
    TypedVar.PinSubCategoryObject = Target.Class;

    const FString Spellings[] = {
        FString::Printf(TEXT("%%typed.%s"), *Target.CanonicalPin),
        FString::Printf(TEXT("%%typed.`%s`"), *Target.SpacedPin),
        FString(TEXT("%typed.Result")),
        FString(TEXT("%typed")),
    };
    for (const FString& Spelling : Spellings)
    {
        UBlueprint* HostBP = MakeHost(*this, TEXT("Typed"), TypedVar);
        if (!HostBP) return false;

        FBpirCompiler Compiler(HostBP);
        const FCompileResult Result = Compiler.Compile(MakeSource(Target.Class, TEXT("Typed"), Spelling));
        if (!TestTrue(FString::Printf(TEXT("`set Typed = %s` compiles; errors:\n%s"), *Spelling, *JoinErrors(Result)),
            Result.bSuccess))
        {
            continue;
        }

        UK2Node_VariableSet* SetNode = nullptr;
        if (UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(HostBP))
        {
            for (UEdGraphNode* Node : EventGraph->Nodes)
            {
                if (UK2Node_VariableSet* Set = Cast<UK2Node_VariableSet>(Node))
                {
                    SetNode = Set;
                }
            }
        }
        UEdGraphPin* ValuePin = SetNode ? SetNode->FindPin(TEXT("Typed"), EGPD_Input) : nullptr;
        if (!TestNotNull(FString::Printf(TEXT("Set Typed node with its value pin exists for %s"), *Spelling), ValuePin))
        {
            continue;
        }
        const bool bFedByCastResult = ValuePin->LinkedTo.Num() == 1
            && Cast<UK2Node_DynamicCast>(ValuePin->LinkedTo[0]->GetOwningNode())
            && ValuePin->LinkedTo[0]->PinName.ToString() == Target.SpacedPin;
        TestTrue(FString::Printf(TEXT("%s wires the cast result pin '%s'"), *Spelling, *Target.SpacedPin), bFedByCastResult);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirCastAccessorSpellingMisspelledTest,
    "PinWright.bpir.compile.CastAccessorSpelling.MisspelledAccessorListsOutputs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirCastAccessorSpellingMisspelledTest::RunTest(const FString& Parameters)
{
    using namespace TestBpirCastAccessorSpellingHelpers;
    FCastTarget Target;
    if (!MakeCastTarget(*this, Target)) return false;

    FEdGraphPinType TypedVar;
    TypedVar.PinCategory = UEdGraphSchema_K2::PC_Object;
    TypedVar.PinSubCategoryObject = Target.Class;
    UBlueprint* HostBP = MakeHost(*this, TEXT("Typed"), TypedVar);
    if (!HostBP) return false;

    // The spelling the single-word wiki example implied: "As" + the class name verbatim.
    const FString MisspelledPin = FString::Printf(TEXT("As%s"), *Target.Class->GetName());
    const FString Misspelled = FString::Printf(TEXT("%%typed.%s"), *MisspelledPin);
    // The value resolver logs its failed pin and property lookups at Error before the
    // compiler reports the diagnostic under test.
    AddOptionalExpectedMessage(*this, MisspelledPin, ELogVerbosity::Error);
    FBpirCompiler Compiler(HostBP);
    const FCompileResult Result = Compiler.Compile(MakeSource(Target.Class, TEXT("Typed"), Misspelled));
    AddInfo(FString::Printf(TEXT("Diagnostics:\n%s"), *JoinErrors(Result)));

    TestFalse(TEXT("The class-name-verbatim accessor does not resolve"), Result.bSuccess);
    TestTrue(TEXT("Diagnostic names the unresolved value"),
        ErrorsContain(Result.Errors, FString::Printf(TEXT("Could not resolve value '%s'"), *Misspelled)));
    TestTrue(TEXT("Diagnostic lists the cast's outputs in their compilable spelling"),
        ErrorsContain(Result.Errors, FString::Printf(TEXT("outputs of %%typed: %s;"), *Target.CanonicalPin)));
    TestTrue(TEXT("Diagnostic says what the bare register resolves to"),
        ErrorsContain(Result.Errors, FString::Printf(TEXT("the bare %%typed resolves to %s"), *Target.CanonicalPin)));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirCastAccessorSpellingRefusedWireTest,
    "PinWright.bpir.compile.CastAccessorSpelling.RefusedWireNamesCompilableAccessor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirCastAccessorSpellingRefusedWireTest::RunTest(const FString& Parameters)
{
    using namespace TestBpirCastAccessorSpellingHelpers;
    FCastTarget Target;
    if (!MakeCastTarget(*this, Target)) return false;

    FEdGraphPinType IntVar;
    IntVar.PinCategory = UEdGraphSchema_K2::PC_Int;
    UBlueprint* HostBP = MakeHost(*this, TEXT("Count"), IntVar);
    if (!HostBP) return false;

    // An object cannot wire into an int: the refused-wire diagnostic names the cast pin.
    FBpirCompiler Compiler(HostBP);
    const FCompileResult Result = Compiler.Compile(MakeSource(Target.Class, TEXT("Count"), TEXT("%typed")));
    AddInfo(FString::Printf(TEXT("Diagnostics:\n%s"), *JoinErrors(Result)));

    TestFalse(TEXT("An object into an int pin is refused"), Result.bSuccess);
    TestTrue(TEXT("Refused wire names the cast pin in its compilable %ref.Pin spelling"),
        ErrorsContain(Result.Errors, FString::Printf(TEXT("wiring data '%s'"), *Target.CanonicalPin)));
    TestFalse(TEXT("Refused wire no longer prints the raw spaced pin name"),
        ErrorsContain(Result.Errors, FString::Printf(TEXT("'%s'"), *Target.SpacedPin)));
    return true;
}
