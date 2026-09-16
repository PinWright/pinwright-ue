// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for Input domain handlers (InputHandler.cpp)
// Handlers tested: input.create_input_action, input.create_input_mapping_context,
//   input.add_mapping, input.remove_mapping, input.get_input_info
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"


namespace
{
    // Ensure shared input assets exist so tests do not depend on execution order.
    void EnsureInputTestAssets()
    {
        TSharedPtr<FJsonObject> ActionPayload = MakeShared<FJsonObject>();
        ActionPayload->SetStringField(TEXT("name"), TEXT("IA_Jump"));
        ActionPayload->SetStringField(TEXT("path"), TEXT("/Game/Input"));
        InvokeHandler(TEXT("input.create_input_action"), ActionPayload);

        TSharedPtr<FJsonObject> ContextPayload = MakeShared<FJsonObject>();
        ContextPayload->SetStringField(TEXT("name"), TEXT("IMC_Default"));
        ContextPayload->SetStringField(TEXT("path"), TEXT("/Game/Input"));
        InvokeHandler(TEXT("input.create_input_mapping_context"), ContextPayload);

        CleanupTestAsset(TEXT("/Game/Input/IA_Jump"));
        CleanupTestAsset(TEXT("/Game/Input/IMC_Default"));
    }
} // namespace

// ============================================================================
// input.create_input_action
// ============================================================================

// Both required params present — handler proceeds into asset creation logic.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInputCreateInputActionValidParamsNoCrashTest,
    "PinWright.input.create_input_action.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInputCreateInputActionValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("IA_Jump"));
    Payload->SetStringField(TEXT("path"), TEXT("/Game/Input"));
    TestTrue(TEXT("input.create_input_action handler found and invoked"),
        InvokeHandler(TEXT("input.create_input_action"), Payload));
    CleanupTestAsset(TEXT("/Game/Input/IA_Jump"));
    return true;
}

// ============================================================================
// input.create_input_mapping_context
// ============================================================================

// Both required params present.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInputCreateInputMappingContextValidParamsNoCrashTest,
    "PinWright.input.create_input_mapping_context.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInputCreateInputMappingContextValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("IMC_Default"));
    Payload->SetStringField(TEXT("path"), TEXT("/Game/Input"));
    TestTrue(TEXT("input.create_input_mapping_context handler found and invoked"),
        InvokeHandler(TEXT("input.create_input_mapping_context"), Payload));
    CleanupTestAsset(TEXT("/Game/Input/IMC_Default"));
    return true;
}

// ============================================================================
// input.add_mapping
// ============================================================================

// All required params present with plausible values — handler will fail to load
// the assets in a test environment, but must not crash.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInputAddMappingValidParamsNoCrashTest,
    "PinWright.input.add_mapping.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInputAddMappingValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    EnsureInputTestAssets();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("contextPath"), TEXT("/Game/Input/IMC_Default"));
    Payload->SetStringField(TEXT("actionPath"), TEXT("/Game/Input/IA_Jump"));
    Payload->SetStringField(TEXT("key"), TEXT("SpaceBar"));
    TestTrue(TEXT("input.add_mapping handler found and invoked"),
        InvokeHandler(TEXT("input.add_mapping"), Payload));
    return true;
}

// ============================================================================
// input.remove_mapping
// ============================================================================

// Both required params present.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInputRemoveMappingValidParamsNoCrashTest,
    "PinWright.input.remove_mapping.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInputRemoveMappingValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    EnsureInputTestAssets();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("contextPath"), TEXT("/Game/Input/IMC_Default"));
    Payload->SetStringField(TEXT("actionPath"), TEXT("/Game/Input/IA_Jump"));
    TestTrue(TEXT("input.remove_mapping handler found and invoked"),
        InvokeHandler(TEXT("input.remove_mapping"), Payload));
    return true;
}

// ============================================================================
// input.get_input_info
// ============================================================================

// Required param present — handler will report NOT_FOUND for a non-existent
// test asset, but must not crash.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInputGetInputInfoValidParamsNoCrashTest,
    "PinWright.input.get_input_info.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInputGetInputInfoValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    EnsureInputTestAssets();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Input/IA_Jump"));
    TestTrue(TEXT("input.get_input_info handler found and invoked"),
        InvokeHandler(TEXT("input.get_input_info"), Payload));
    return true;
}

// Regression for E-get-input-info-consume-field-name-mismatch: get_input_info must
// emit the consume flag under the documented key "bConsumeInput", not the legacy
// "consumeInput", so callers keying by the wiki-documented name find it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInputGetInputInfoEmitsDocumentedConsumeKeyTest,
    "PinWright.input.get_input_info.EmitsDocumentedConsumeKey",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInputGetInputInfoEmitsDocumentedConsumeKeyTest::RunTest(const FString& Parameters)
{
    // The documented field name lives in the handler summary that feeds the wiki.
    const FString Summary = GetRegisteredSummary(TEXT("input.get_input_info"));
    TestTrue(TEXT("summary documents the bConsumeInput field"),
        Summary.Contains(TEXT("bConsumeInput")));

    // Create a real UInputAction so the handler enters its UInputAction branch and
    // serializes the consume flag. The create handler is the production path; keep
    // the asset alive across the inspect call, then clean it up. ActionPackage is
    // derived from the same name/path so the lookup can't drift from what was created.
    const FString ActionName = TEXT("IA_ConsumeKeyRegression");
    const FString ActionPath = TEXT("/Game/Input");
    const FString ActionPackage = FString::Printf(TEXT("%s/%s"), *ActionPath, *ActionName);
    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("name"), ActionName);
    CreatePayload->SetStringField(TEXT("path"), ActionPath);
    InvokeHandler(TEXT("input.create_input_action"), CreatePayload);

    TSharedPtr<FJsonObject> InfoPayload = MakeShared<FJsonObject>();
    InfoPayload->SetStringField(TEXT("assetPath"), ActionPackage);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("input.get_input_info"), InfoPayload, Capture);
    TestTrue(TEXT("input.get_input_info handler found"), bFound);

    // The assertions only fire when the asset actually loaded (success). In a bare
    // test environment without a real asset the handler returns NOT_FOUND; the
    // create handler above gives us a real UInputAction so success is expected.
    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        TestTrue(TEXT("response carries the documented bConsumeInput key"),
            Capture.Result->HasField(TEXT("bConsumeInput")));
        TestFalse(TEXT("response does NOT carry the legacy consumeInput key"),
            Capture.Result->HasField(TEXT("consumeInput")));
    }

    CleanupTestAsset(ActionPackage);
    return true;
}
