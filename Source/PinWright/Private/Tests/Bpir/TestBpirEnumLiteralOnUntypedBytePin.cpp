// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

using namespace CompilerTestUtils;

// Enum::Value literal on bare PC_Byte pin must write name string, not numeric index.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirEnumLiteralOnUntypedBytePinTest,
    "PinWright.bpir.compiler.enum_literal_on_untyped_byte_pin",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirEnumLiteralOnUntypedBytePinTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("EnumLitUntypedBytePinBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %cmp = pure EqualEqual_ByteByte(A: ESlateVisibility::Visible, B: ESlateVisibility::Collapsed)\n")
        TEXT("    call PrintString(InString: \"x\")\n")
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

    UK2Node_CallFunction* CmpNode = FindCallFunctionBySubstring(BP, TEXT("EqualEqual_ByteByte"));
    TestNotNull(TEXT("EqualEqual_ByteByte node exists"), CmpNode);
    if (!CmpNode) return false;

    UEdGraphPin* APin = CmpNode->FindPin(TEXT("A"));
    UEdGraphPin* BPin = CmpNode->FindPin(TEXT("B"));
    TestNotNull(TEXT("A pin exists"), APin);
    TestNotNull(TEXT("B pin exists"), BPin);
    if (!APin || !BPin) return false;

    UEnum* AEnum = Cast<UEnum>(APin->PinType.PinSubCategoryObject.Get());
    TestNotNull(TEXT("A pin subcategory resolved to a UEnum"), AEnum);
    if (AEnum)
    {
        TestEqual(TEXT("A pin subcategory is ESlateVisibility"),
            AEnum->GetName(), FString(TEXT("ESlateVisibility")));
    }

    // Name strings (not numeric indices) are required so the post-link pin
    // validator accepts the default against the inherited enum subtype.
    TestEqual(TEXT("A pin default is the enum name 'Visible'"),
        APin->DefaultValue, FString(TEXT("Visible")));
    TestEqual(TEXT("B pin default is the enum name 'Collapsed'"),
        BPin->DefaultValue, FString(TEXT("Collapsed")));
    return true;
}
