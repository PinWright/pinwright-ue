// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"

using namespace CompilerTestUtils;

// Qualified-but-schema-invalid enum literal (e.g. `ESlateVisibility::NotARealEnumerator`)
// on a PC_Byte pin must be rejected at BPIR-compile time. Previously CodePinResolver::
// SetPinDefaultValue's fallback TrySetDefaultValue discarded the K2 schema's verdict, so
// the bogus literal landed in DefaultValue and produced a "valid unsigned number for a
// byte property" failure on the next full BP recompile — not at BPIR time.
//
// Bare identifiers (e.g. `Collapsed` without `ESlateVisibility::`) fail even earlier in
// BpirValueResolver::ResolveValue with "Unknown value reference format", before they
// can reach the pin-default path — see board entry
// B-bpir-byte-pin-accepts-bare-enum-literal history #2.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirQualifiedInvalidEnumLiteralOnBytePinRejectedTest,
    "PinWright.bpir.compiler.qualified_invalid_enum_literal_on_byte_pin_rejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirQualifiedInvalidEnumLiteralOnBytePinRejectedTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("QualInvalidEnumLitBytePinBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // The A pin uses a valid `ESlateVisibility::Visible` qualifier so the enum subtype
    // resolves and the resolver advances past the `::` branch. The B pin uses a
    // qualified-but-invalid enumerator on the same enum — that branch's enum-literal
    // resolution fails, control falls through to the schema's IsPinDefaultValid check
    // (CodePinResolver.cpp ~line 242), which rejects the value with a diagnostic that
    // lists the qualified-literal workaround.
    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %cmp = pure EqualEqual_ByteByte(A: ESlateVisibility::Visible, B: ESlateVisibility::NotARealEnumerator)\n")
        TEXT("    call PrintString(InString: \"x\")\n")
        TEXT("}"));

    TestFalse(TEXT("Compile failed (qualified-but-invalid enum literal on byte pin must be rejected)"), Result.bSuccess);
    TestTrue(TEXT("At least one compile error was emitted"), Result.Errors.Num() >= 1);
    TestTrue(TEXT("Error message names the byte/enum pin rejection"),
        ErrorsContain(Result.Errors, TEXT("is not valid for byte/enum pin")));
    TestTrue(TEXT("Error message lists the qualified-literal workaround"),
        ErrorsContain(Result.Errors, TEXT("EnumName::Value")));
    return true;
}
