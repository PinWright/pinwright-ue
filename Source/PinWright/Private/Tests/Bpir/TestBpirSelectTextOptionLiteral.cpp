// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-bpir-select-literal-text-lost.
//
// UK2Node_Select allocates every option pin as PC_Wildcard, so a literal FText
// option used to be applied while the pin was still untyped: the wildcard
// escape hatch in FCodePinResolver::SetPinDefaultValue wrote the raw
// NSLOCTEXT(...) string into DefaultValue, which is the slot a PC_Text pin
// never reads (UK2Node_Select::ExpandNode copies DefaultTextValue into the
// literal term). The BPIR compiled, the node existed, decompile printed the
// literal back verbatim — and the option was empty text at runtime.
//
// The fix pre-types the Select's Return Value and option pins at emit time,
// from the `%name: Type =` annotation when present and otherwise from an
// option literal that parses as an FText with a real localization identity.
//
// Counterfactual — every assertion below on DefaultTextValue fails before the
// fix, because the option pins carried an empty DefaultTextValue and the raw
// literal string in DefaultValue. Asserting only bSuccess proves nothing: the
// broken compile already succeeded.

#include "Misc/AutomationTest.h"

#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/BpirStructLiteralUtils.h"
#include "Compiler/CompilerTypes.h"
#include "Decompiler/BpirDecompiler.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Internationalization/Text.h"
#include "K2Node_Select.h"
#include "Kismet2/BlueprintEditorUtils.h"

using namespace CompilerTestUtils;

namespace
{
    const TCHAR* const SelectTextNamespace = TEXT("PwSelectOption");
    const TCHAR* const SelectTextTrueKey = TEXT("TrueKey");
    const TCHAR* const SelectTextFalseKey = TEXT("FalseKey");
    const TCHAR* const SelectTextTrueDisplay = TEXT("TRUE_TEXT");
    const TCHAR* const SelectTextFalseDisplay = TEXT("FALSE_TEXT");

    FString MakeNsLocTextBpirLiteral(const TCHAR* Key, const TCHAR* Display)
    {
        return BpirStructLiteralUtils::EscapeBpirString(FString::Printf(
            TEXT("NSLOCTEXT(\"%s\", \"%s\", \"%s\")"), SelectTextNamespace, Key, Display));
    }

    // A bool member variable makes `$bFlag` type the Select's Index pin, which
    // is what the live repro used. A bool *literal* index leaves the Index pin
    // wildcard — a separate, loud defect tracked on its own.
    void AddBoolFlagVariable(UBlueprint* BP)
    {
        FEdGraphPinType BoolType;
        BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
        FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("bFlag"), BoolType);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSelectTextOptionLiteralRoundTripTest,
    "PinWright.bpir.round_trip.SelectTextOptionLiteral",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSelectTextOptionLiteralRoundTripTest::RunTest(const FString& Parameters)
{
    // Asserts an option pin actually carries the localized text, in the slot
    // the Blueprint compiler reads.
    auto CheckTextOption = [this](
        const TCHAR* Label,
        UEdGraphPin* Pin,
        const TCHAR* ExpectedKey,
        const TCHAR* ExpectedDisplay) -> void
    {
        if (!Pin)
        {
            AddError(FString::Printf(TEXT("%s: option pin not found"), Label));
            return;
        }

        const FString TypedWhat = FString::Printf(TEXT("%s is typed text"), Label);
        TestTrue(*TypedWhat, Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Text);

        const FString HasTextWhat = FString::Printf(TEXT("%s carries a non-empty DefaultTextValue"), Label);
        TestFalse(*HasTextWhat, Pin->DefaultTextValue.IsEmpty());

        const FString DisplayWhat = FString::Printf(TEXT("%s display string survives"), Label);
        TestEqual(*DisplayWhat, Pin->DefaultTextValue.ToString(), FString(ExpectedDisplay));

        const TOptional<FString> Namespace = FTextInspector::GetNamespace(Pin->DefaultTextValue);
        const TOptional<FString> Key = FTextInspector::GetKey(Pin->DefaultTextValue);

        // StartsWith, not equality: UE's stable-localization-key conforming may
        // append a " [PackageId]" suffix to a namespace on a packaged node.
        const FString NamespaceWhat = FString::Printf(TEXT("%s namespace matches"), Label);
        TestTrue(*NamespaceWhat, Namespace.IsSet() && Namespace.GetValue().StartsWith(SelectTextNamespace));

        const FString KeyWhat = FString::Printf(TEXT("%s key matches"), Label);
        TestTrue(*KeyWhat, Key.IsSet() && Key.GetValue() == FString(ExpectedKey));

        // The wildcard escape hatch's raw string must not be left behind: for a
        // PC_Text pin DefaultValue is dead weight that hides the empty text.
        const FString NoRawWhat = FString::Printf(TEXT("%s does not keep the raw literal in DefaultValue"), Label);
        TestTrue(*NoRawWhat, Pin->DefaultValue.IsEmpty());
    };

    const FString TrueLiteral = MakeNsLocTextBpirLiteral(SelectTextTrueKey, SelectTextTrueDisplay);
    const FString FalseLiteral = MakeNsLocTextBpirLiteral(SelectTextFalseKey, SelectTextFalseDisplay);

    // ------------------------------------------------------------------------
    // 1. Un-annotated source — the type is inferred from the option literal.
    // ------------------------------------------------------------------------
    UBlueprint* BP = CreateTransientTestBP(TEXT("SelectTextOptionBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;
    AddBoolFlagVariable(BP);

    const FString InputBpir = FString::Printf(TEXT(
        "entry event BeginPlay() {\n"
        "    %%sel = select(Index: $bFlag, true: %s, false: %s)\n"
        "    call PrintText(InText: %%sel)\n"
        "}"), *TrueLiteral, *FalseLiteral);

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    UK2Node_Select* SelectNode = FindNodeOfType<UK2Node_Select>(BP);
    TestNotNull(TEXT("Select node was created"), SelectNode);
    if (!SelectNode) return false;

    // Option 0 is UE's False branch, Option 1 its True branch
    // (UK2Node_Select::AllocateDefaultPins).
    CheckTextOption(TEXT("Option 0 (false branch)"),
        SelectNode->FindPin(TEXT("Option 0"), EGPD_Input),
        SelectTextFalseKey, SelectTextFalseDisplay);
    CheckTextOption(TEXT("Option 1 (true branch)"),
        SelectNode->FindPin(TEXT("Option 1"), EGPD_Input),
        SelectTextTrueKey, SelectTextTrueDisplay);

    // ------------------------------------------------------------------------
    // 2. Decompile must print the localization identity, not a bare display
    //    string — a decompile that lost namespace/key would silently degrade
    //    the recompile in step 3 to an identity-less FText.
    // ------------------------------------------------------------------------
    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Decompile emits the true branch's NSLOCTEXT identity"),
        Output.Contains(SelectTextTrueKey) && Output.Contains(SelectTextTrueDisplay));
    TestTrue(TEXT("Decompile emits the false branch's NSLOCTEXT identity"),
        Output.Contains(SelectTextFalseKey) && Output.Contains(SelectTextFalseDisplay));
    TestTrue(TEXT("Decompile annotates the select's result type as text"),
        Output.Contains(TEXT(": text = select(")));

    // ------------------------------------------------------------------------
    // 3. Recompiling the decompiled source must reproduce the same pin state.
    //    This leg exercises the `%name: text =` annotation path rather than
    //    literal-shape inference, since the decompiler always annotates the
    //    select's result type.
    // ------------------------------------------------------------------------
    UBlueprint* BP2 = CreateTransientTestBP(TEXT("SelectTextOptionRecompileBP"));
    TestNotNull(TEXT("Recompile blueprint created"), BP2);
    if (!BP2) return false;
    AddBoolFlagVariable(BP2);

    FBpirCompiler Compiler2(BP2);
    FCompileResult RecompileResult = Compiler2.Compile(Output);
    if (!RecompileResult.bSuccess)
    {
        for (const FCompileError& Err : RecompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Re-compile succeeded"), RecompileResult.bSuccess);
    if (!RecompileResult.bSuccess) return false;

    UK2Node_Select* SelectNode2 = FindNodeOfType<UK2Node_Select>(BP2);
    TestNotNull(TEXT("Re-compiled Select node was created"), SelectNode2);
    if (!SelectNode2) return false;

    CheckTextOption(TEXT("Re-compiled Option 0 (false branch)"),
        SelectNode2->FindPin(TEXT("Option 0"), EGPD_Input),
        SelectTextFalseKey, SelectTextFalseDisplay);
    CheckTextOption(TEXT("Re-compiled Option 1 (true branch)"),
        SelectNode2->FindPin(TEXT("Option 1"), EGPD_Input),
        SelectTextTrueKey, SelectTextTrueDisplay);

    return true;
}
