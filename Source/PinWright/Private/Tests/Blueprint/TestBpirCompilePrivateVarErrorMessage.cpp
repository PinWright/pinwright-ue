// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for UE engine bug: K2Node_VariableSet.cpp:450 calls
// LOCTEXT("UnableToSet_ReadOnly", "{VariableName} is private...").ToString()
// without FText::Format, so the {VariableName} named arg is never substituted.
// The sibling "NotWritable" case (line 446) does it correctly.
//
// Since the plugin cannot patch engine source, BlueprintHandlerUtils.cpp's
// CompileBlueprintWithDiagnostics applies a narrow repair pass after
// FTokenizedMessage::ToText() produces the broken string.  This test calls the
// production repair helper directly with the known engine-produced broken
// string so it runs without depending on live engine compile state.
#include "Misc/AutomationTest.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirPrivateVariableErrorMessageInterpolationTest,
    "PinWright.bpir.compile.PrivateVariableErrorMessageInterpolated",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirPrivateVariableErrorMessageInterpolationTest::RunTest(const FString& Parameters)
{
    // The exact string that the engine emits after FTokenizedMessage token
    // expansion: {VariableName} is the un-substituted placeholder from
    // K2Node_VariableSet.cpp:450, and "Set HiddenVar" is the @@-expanded node
    // title appended by FTokenizedMessage.  Two spaces between the period and
    // "Set" match the engine's formatting.
    const FString BrokenEngineMessage =
        TEXT("{VariableName} is private and not accessible in this context.  Set HiddenVar");

    const FString ExpectedMessage =
        TEXT("HiddenVar is private and not accessible in this context.  Set HiddenVar");

    const FString Repaired = BlueprintHandlerUtils::RepairUnableToSetReadOnlyMessage(BrokenEngineMessage);

    TestFalse(TEXT("no literal placeholder"), Repaired.Contains(TEXT("{VariableName}")));
    TestTrue(TEXT("contains real var name"), Repaired.Contains(TEXT("HiddenVar")));
    TestEqual(TEXT("full repaired message matches expected"), Repaired, ExpectedMessage);

    // Defensive cases: strings that don't match the broken pattern must pass
    // through unchanged, so the repair can't corrupt unrelated messages.
    const FString Unrelated = TEXT("Some other compile error message.");
    TestEqual(TEXT("unrelated message passes through unchanged"),
        BlueprintHandlerUtils::RepairUnableToSetReadOnlyMessage(Unrelated), Unrelated);

    const FString Truncated = TEXT("{VariableName} is private and not accessible in this context.");
    TestEqual(TEXT("truncated message without Set suffix passes through unchanged"),
        BlueprintHandlerUtils::RepairUnableToSetReadOnlyMessage(Truncated), Truncated);

    return true;
}
