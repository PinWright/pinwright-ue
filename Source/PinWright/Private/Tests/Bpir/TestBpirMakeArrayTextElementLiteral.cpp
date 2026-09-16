// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-bpir-make-array-loses-ftext-literals.
//
// UK2Node_MakeContainer allocates the output pin and every `[N]` element pin as
// PC_Wildcard, and AddInputPin copies the still-wildcard output type onto each
// pin the emitter adds. A literal FText element used to be applied while those
// pins were untyped: the wildcard escape hatch in
// FCodePinResolver::SetPinDefaultValue wrote the raw NSLOCTEXT(...) string into
// DefaultValue, which is the slot a PC_Text pin never reads. The type then
// arrived anyway, one instruction later, when the array output was wired to a
// typed consumer and the engine's NotifyPinConnectionListChanged propagated it
// down -- so the finished node LOOKS right (PC_Text element pins, decompile
// prints the literal back verbatim) while carrying an empty DefaultTextValue.
// The BPIR compiled, the node existed, and the text was gone at runtime.
//
// The fix pre-types the MakeArray's output and element pins at emit time, from
// the `%name: array<T> =` annotation when present and otherwise from an element
// literal that parses as an FText with a real localization identity.
//
// Counterfactual -- every DefaultTextValue / DefaultValue assertion below fails
// before the fix: the element pins ended up PC_Text (propagated from the
// consumer) with an EMPTY DefaultTextValue and the raw literal string still
// sitting in DefaultValue. Asserting only bSuccess, the node count, or even the
// element pin's PinCategory proves nothing -- the broken compile produced all
// three.

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
#include "K2Node_MakeArray.h"
#include "Kismet2/BlueprintEditorUtils.h"

using namespace CompilerTestUtils;

// File-unique named namespace (not anonymous): Unity merges translation units,
// and two anonymous namespaces in one merged TU are the same namespace.
namespace MakeArrayTextElementTestLocal
{
    const TCHAR* const TextNamespace = TEXT("PwMakeArrayElement");
    const TCHAR* const FirstKey = TEXT("FirstKey");
    const TCHAR* const SecondKey = TEXT("SecondKey");
    const TCHAR* const FirstDisplay = TEXT("FIRST_TEXT");
    const TCHAR* const SecondDisplay = TEXT("SECOND_TEXT");

    // The BPIR member variable the array is stored into. A typed consumer is what
    // makes the pre-fix behaviour subtle rather than loud: without one, nothing
    // ever types the node and the loss is visible as a wildcard graph instead.
    const TCHAR* const CaptionsVarName = TEXT("Captions");

    inline FString MakeNsLocTextBpirLiteral(const TCHAR* Key, const TCHAR* Display)
    {
        return BpirStructLiteralUtils::EscapeBpirString(FString::Printf(
            TEXT("NSLOCTEXT(\"%s\", \"%s\", \"%s\")"), TextNamespace, Key, Display));
    }

    // TArray<FText> member. Wiring the make_array into its setter is what used to
    // type the element pins *after* the literals had already been misrouted.
    inline void AddCaptionsVariable(UBlueprint* BP)
    {
        FEdGraphPinType TextArrayType;
        TextArrayType.PinCategory = UEdGraphSchema_K2::PC_Text;
        TextArrayType.ContainerType = EPinContainerType::Array;
        FBlueprintEditorUtils::AddMemberVariable(BP, CaptionsVarName, TextArrayType);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMakeArrayTextElementLiteralRoundTripTest,
    "PinWright.bpir.round_trip.MakeArrayTextElementLiteral",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMakeArrayTextElementLiteralRoundTripTest::RunTest(const FString& Parameters)
{
    using namespace MakeArrayTextElementTestLocal;

    // Asserts an element pin actually carries the localized text, in the slot the
    // Blueprint compiler reads.
    auto CheckTextElement = [this](
        const TCHAR* Label,
        UEdGraphPin* Pin,
        const TCHAR* ExpectedKey,
        const TCHAR* ExpectedDisplay) -> void
    {
        if (!Pin)
        {
            AddError(FString::Printf(TEXT("%s: element pin not found"), Label));
            return;
        }

        const FString TypedWhat = FString::Printf(TEXT("%s is typed text"), Label);
        TestTrue(*TypedWhat, Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Text);

        // An element pin holds one FText, not a container: UK2Node_MakeContainer
        // refuses a container input outright (IsConnectionDisallowed).
        const FString ScalarWhat = FString::Printf(TEXT("%s is a scalar, not a container"), Label);
        TestTrue(*ScalarWhat, Pin->PinType.ContainerType == EPinContainerType::None);

        const FString HasTextWhat = FString::Printf(TEXT("%s carries a non-empty DefaultTextValue"), Label);
        TestFalse(*HasTextWhat, Pin->DefaultTextValue.IsEmpty());

        const FString DisplayWhat = FString::Printf(TEXT("%s display string survives"), Label);
        TestEqual(*DisplayWhat, Pin->DefaultTextValue.ToString(), FString(ExpectedDisplay));

        const TOptional<FString> Namespace = FTextInspector::GetNamespace(Pin->DefaultTextValue);
        const TOptional<FString> Key = FTextInspector::GetKey(Pin->DefaultTextValue);

        // StartsWith, not equality: UE's stable-localization-key conforming may
        // append a " [PackageId]" suffix to a namespace on a packaged node.
        const FString NamespaceWhat = FString::Printf(TEXT("%s namespace matches"), Label);
        TestTrue(*NamespaceWhat, Namespace.IsSet() && Namespace.GetValue().StartsWith(TextNamespace));

        const FString KeyWhat = FString::Printf(TEXT("%s key matches"), Label);
        TestTrue(*KeyWhat, Key.IsSet() && Key.GetValue() == FString(ExpectedKey));

        // The wildcard escape hatch's raw string must not be left behind: for a
        // PC_Text pin DefaultValue is dead weight that hides the empty text.
        const FString NoRawWhat = FString::Printf(TEXT("%s does not keep the raw literal in DefaultValue"), Label);
        TestTrue(*NoRawWhat, Pin->DefaultValue.IsEmpty());
    };

    // The array output pin must end up array<text>, not a bare text pin and not a
    // surviving wildcard.
    auto CheckArrayOutput = [this](const TCHAR* Label, UK2Node_MakeArray* Node) -> void
    {
        UEdGraphPin* OutputPin = Node ? Node->GetOutputPin() : nullptr;
        if (!OutputPin)
        {
            AddError(FString::Printf(TEXT("%s: output pin not found"), Label));
            return;
        }

        const FString CategoryWhat = FString::Printf(TEXT("%s output pin is text"), Label);
        TestTrue(*CategoryWhat, OutputPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Text);

        const FString ContainerWhat = FString::Printf(TEXT("%s output pin is an array"), Label);
        TestTrue(*ContainerWhat, OutputPin->PinType.ContainerType == EPinContainerType::Array);
    };

    const FString FirstLiteral = MakeNsLocTextBpirLiteral(FirstKey, FirstDisplay);
    const FString SecondLiteral = MakeNsLocTextBpirLiteral(SecondKey, SecondDisplay);

    // ------------------------------------------------------------------------
    // 1. Un-annotated source -- the type is inferred from the element literals.
    // ------------------------------------------------------------------------
    UBlueprint* BP = CreateTransientTestBP(TEXT("MakeArrayTextElementBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;
    AddCaptionsVariable(BP);

    const FString InputBpir = FString::Printf(TEXT(
        "entry event BeginPlay() {\n"
        "    %%arr = make_array(%s, %s)\n"
        "    set %s = %%arr\n"
        "}"), *FirstLiteral, *SecondLiteral, CaptionsVarName);

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

    UK2Node_MakeArray* ArrayNode = FindNodeOfType<UK2Node_MakeArray>(BP);
    TestNotNull(TEXT("MakeArray node was created"), ArrayNode);
    if (!ArrayNode) return false;

    CheckArrayOutput(TEXT("MakeArray"), ArrayNode);
    CheckTextElement(TEXT("Element [0]"),
        ArrayNode->FindPin(TEXT("[0]"), EGPD_Input), FirstKey, FirstDisplay);
    CheckTextElement(TEXT("Element [1]"),
        ArrayNode->FindPin(TEXT("[1]"), EGPD_Input), SecondKey, SecondDisplay);

    // ------------------------------------------------------------------------
    // 2. Decompile must print both localization identities AND the array<text>
    //    result annotation -- the annotation is the type source the recompile in
    //    step 3 depends on, and a decompile that lost namespace/key would
    //    silently degrade that recompile to an identity-less FText.
    // ------------------------------------------------------------------------
    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Decompile emits the first element's NSLOCTEXT identity"),
        Output.Contains(FirstKey) && Output.Contains(FirstDisplay));
    TestTrue(TEXT("Decompile emits the second element's NSLOCTEXT identity"),
        Output.Contains(SecondKey) && Output.Contains(SecondDisplay));
    TestTrue(TEXT("Decompile annotates the make_array's result type as array<text>"),
        Output.Contains(TEXT(": array<text> = make_array(")));

    // ------------------------------------------------------------------------
    // 3. Recompiling the decompiled source must reproduce the same pin state.
    //    This leg exercises the `%name: array<text> =` annotation path rather
    //    than literal-shape inference, since the decompiler always annotates the
    //    make_array's result type.
    // ------------------------------------------------------------------------
    UBlueprint* BP2 = CreateTransientTestBP(TEXT("MakeArrayTextElementRecompileBP"));
    TestNotNull(TEXT("Recompile blueprint created"), BP2);
    if (!BP2) return false;
    AddCaptionsVariable(BP2);

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

    UK2Node_MakeArray* ArrayNode2 = FindNodeOfType<UK2Node_MakeArray>(BP2);
    TestNotNull(TEXT("Re-compiled MakeArray node was created"), ArrayNode2);
    if (!ArrayNode2) return false;

    CheckArrayOutput(TEXT("Re-compiled MakeArray"), ArrayNode2);
    CheckTextElement(TEXT("Re-compiled element [0]"),
        ArrayNode2->FindPin(TEXT("[0]"), EGPD_Input), FirstKey, FirstDisplay);
    CheckTextElement(TEXT("Re-compiled element [1]"),
        ArrayNode2->FindPin(TEXT("[1]"), EGPD_Input), SecondKey, SecondDisplay);

    // ------------------------------------------------------------------------
    // 4. A make_array of ordinary quoted strings must be untouched: a bare string
    //    literal has no localization identity, so nothing may mistype it as text.
    //    This is the guard against step 1's inference over-firing.
    // ------------------------------------------------------------------------
    UBlueprint* BP3 = CreateTransientTestBP(TEXT("MakeArrayStringElementBP"));
    TestNotNull(TEXT("String-array blueprint created"), BP3);
    if (!BP3) return false;

    FBpirCompiler Compiler3(BP3);
    FCompileResult StringResult = Compiler3.Compile(TEXT(
        "entry event BeginPlay() {\n"
        "    %arr = make_array(\"Alpha\", \"Beta\")\n"
        "    call PrintString(InString: \"Done\")\n"
        "}"));
    if (!StringResult.bSuccess)
    {
        for (const FCompileError& Err : StringResult.Errors)
        {
            AddError(FString::Printf(TEXT("String-array compile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("String-array compile succeeded"), StringResult.bSuccess);
    if (!StringResult.bSuccess) return false;

    UK2Node_MakeArray* StringArrayNode = FindNodeOfType<UK2Node_MakeArray>(BP3);
    TestNotNull(TEXT("String MakeArray node was created"), StringArrayNode);
    if (!StringArrayNode) return false;

    UEdGraphPin* StringElementPin = StringArrayNode->FindPin(TEXT("[0]"), EGPD_Input);
    if (!StringElementPin)
    {
        AddError(TEXT("String MakeArray element pin [0] not found"));
        return false;
    }
    TestTrue(TEXT("A bare quoted element is not mistyped as text"),
        StringElementPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Text);
    TestEqual(TEXT("A bare quoted element keeps its literal in DefaultValue"),
        StringElementPin->DefaultValue, FString(TEXT("Alpha")));

    return true;
}
