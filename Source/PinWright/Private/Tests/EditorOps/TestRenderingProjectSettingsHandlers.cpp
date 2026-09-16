// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for rendering.* namespace handlers (URendererSettings r/w).
// See board ticket F-rendering-project-settings for design rationale.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"

#include "Engine/RendererSettings.h"
#include "Misc/ScopeExit.h"

// ============================================================================
// Registration counterfactual: dispatcher must know about rendering.* handlers.
// If RenderingProjectSettingsHandler.cpp is reverted, dispatcher returns
// METHOD_NOT_FOUND for rendering.get_project_settings and this test fails.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderingHandlerRegistrationTest,
    "PinWright.rendering.handlers.Registered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRenderingHandlerRegistrationTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("rendering.get_project_settings registered"),
        IsHandlerRegistered(TEXT("rendering.get_project_settings")));
    TestTrue(TEXT("rendering.set_project_settings registered"),
        IsHandlerRegistered(TEXT("rendering.set_project_settings")));
    TestTrue(TEXT("rendering.set_lumen_method registered"),
        IsHandlerRegistered(TEXT("rendering.set_lumen_method")));
    TestTrue(TEXT("rendering.set_dynamic_gi_method registered"),
        IsHandlerRegistered(TEXT("rendering.set_dynamic_gi_method")));
    return true;
}

// ============================================================================
// get_project_settings: response must contain at least one well-known
// URendererSettings UPROPERTY. bDefaultFeatureAutoExposure is verified present
// at RendererSettings.h:793 and is CPF_Config-tagged.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderingGetProjectSettingsReturnsKnownFieldTest,
    "PinWright.rendering.get_project_settings.ReturnsKnownField",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRenderingGetProjectSettingsReturnsKnownFieldTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    FTestResponseCapture Capture;
    TestTrue(TEXT("rendering.get_project_settings invoked"),
        InvokeHandlerWithCapture(TEXT("rendering.get_project_settings"), Payload, Capture));
    if (!TestTrue(TEXT("handler returned success"), Capture.bSuccess)) return false;
    if (!TestTrue(TEXT("result valid"), Capture.Result.IsValid())) return false;

    const TSharedPtr<FJsonObject>* Settings = nullptr;
    if (!TestTrue(TEXT("result has settings object"),
        Capture.Result->TryGetObjectField(TEXT("settings"), Settings))) return false;

    TestTrue(TEXT("settings contains bDefaultFeatureAutoExposure (CPF_Config UPROPERTY at RendererSettings.h:793)"),
        (*Settings)->HasField(TEXT("bDefaultFeatureAutoExposure")));
    TestTrue(TEXT("settings contains DynamicGlobalIllumination UPROPERTY"),
        (*Settings)->HasField(TEXT("DynamicGlobalIllumination")));
    TestTrue(TEXT("settings contains Reflections UPROPERTY"),
        (*Settings)->HasField(TEXT("Reflections")));
    TestTrue(TEXT("response includes configFile path"),
        Capture.Result->HasField(TEXT("configFile")));
    return true;
}

// ============================================================================
// set_project_settings round-trip: flip a known bool, read it back, restore.
// Uses save:false so the test never touches DefaultEngine.ini on disk.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderingSetProjectSettingsRoundTripTest,
    "PinWright.rendering.set_project_settings.RoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRenderingSetProjectSettingsRoundTripTest::RunTest(const FString& Parameters)
{
    URendererSettings* S = GetMutableDefault<URendererSettings>();
    if (!TestNotNull(TEXT("URendererSettings CDO present"), S)) return false;

    const bool OriginalValue = (S->bDefaultFeatureAutoExposure != 0);
    ON_SCOPE_EXIT
    {
        // Restore in-memory state regardless of test outcome (save:false → ini untouched).
        URendererSettings* SR = GetMutableDefault<URendererSettings>();
        if (SR) SR->bDefaultFeatureAutoExposure = OriginalValue ? 1 : 0;
    };

    const bool FlippedValue = !OriginalValue;

    TSharedPtr<FJsonObject> Updates = MakeShared<FJsonObject>();
    Updates->SetBoolField(TEXT("bDefaultFeatureAutoExposure"), FlippedValue);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("updates"), Updates);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("rendering.set_project_settings invoked"),
        InvokeHandlerWithCapture(TEXT("rendering.set_project_settings"), Payload, Capture));
    if (!TestTrue(TEXT("set returned success"), Capture.bSuccess)) return false;

    const TArray<TSharedPtr<FJsonValue>>* Applied = nullptr;
    if (!TestTrue(TEXT("response has applied[]"),
        Capture.Result->TryGetArrayField(TEXT("applied"), Applied))) return false;
    TestEqual(TEXT("exactly one property applied"), Applied->Num(), 1);

    TestEqual(TEXT("flipped value visible in CDO"),
        (S->bDefaultFeatureAutoExposure != 0), FlippedValue);

    // Re-read through the handler and assert round-trip.
    FTestResponseCapture ReadBack;
    TestTrue(TEXT("rendering.get_project_settings invoked again"),
        InvokeHandlerWithCapture(TEXT("rendering.get_project_settings"),
            MakeShared<FJsonObject>(), ReadBack));
    const TSharedPtr<FJsonObject>* SettingsObj = nullptr;
    if (TestTrue(TEXT("readback returned settings"),
        ReadBack.Result->TryGetObjectField(TEXT("settings"), SettingsObj)))
    {
        bool ReadBackValue = false;
        TestTrue(TEXT("readback exposes the flipped field"),
            (*SettingsObj)->TryGetBoolField(TEXT("bDefaultFeatureAutoExposure"), ReadBackValue));
        TestEqual(TEXT("readback matches set"), ReadBackValue, FlippedValue);
    }
    return true;
}

// ============================================================================
// Non-config / unknown property rejection:
// - A fabricated nonexistent property name must land in `rejected[]` with
//   reason `"unknown_property"` (not crash, not silently no-op).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderingSetProjectSettingsRejectsNonConfigPropertyTest,
    "PinWright.rendering.set_project_settings.RejectsNonConfigProperty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRenderingSetProjectSettingsRejectsNonConfigPropertyTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Updates = MakeShared<FJsonObject>();
    Updates->SetStringField(TEXT("ThisFieldDoesNotExistOnRendererSettings"), TEXT("ignored"));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("updates"), Updates);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("rendering.set_project_settings invoked"),
        InvokeHandlerWithCapture(TEXT("rendering.set_project_settings"), Payload, Capture));
    if (!TestTrue(TEXT("handler succeeded (rejection is data, not error)"), Capture.bSuccess)) return false;

    const TArray<TSharedPtr<FJsonValue>>* Rejected = nullptr;
    if (!TestTrue(TEXT("response includes rejected[]"),
        Capture.Result->TryGetArrayField(TEXT("rejected"), Rejected))) return false;
    if (!TestEqual(TEXT("exactly one rejection"), Rejected->Num(), 1)) return false;

    const TSharedPtr<FJsonObject>* Entry = nullptr;
    if (!TestTrue(TEXT("rejection entry is object"),
        (*Rejected)[0]->TryGetObject(Entry))) return false;

    FString Name, Reason;
    TestTrue(TEXT("entry has name"), (*Entry)->TryGetStringField(TEXT("name"), Name));
    TestTrue(TEXT("entry has reason"), (*Entry)->TryGetStringField(TEXT("reason"), Reason));
    TestEqual(TEXT("name matches input"), Name, FString(TEXT("ThisFieldDoesNotExistOnRendererSettings")));
    TestEqual(TEXT("reason is unknown_property"), Reason, FString(TEXT("unknown_property")));

    const TArray<TSharedPtr<FJsonValue>>* Applied = nullptr;
    if (TestTrue(TEXT("response includes applied[]"),
        Capture.Result->TryGetArrayField(TEXT("applied"), Applied)))
    {
        TestEqual(TEXT("nothing applied"), Applied->Num(), 0);
    }
    return true;
}

// ============================================================================
// set_lumen_method softwareRTMode friendly-token alias
// (B-rendering-lumen-software-mode-token-alias):
// The wiki + param spec document softwareRTMode as accepting Detail | Global —
// short API tokens that match NEITHER ELumenSoftwareTracingMode's enumerator
// identifiers (DetailTracing=1 / GlobalTracing=0) NOR their UMETA DisplayNames
// ("Detail Tracing" / "Global Tracing"). The handler must alias Detail ->
// DetailTracing and Global -> GlobalTracing before coercion. If the alias is
// reverted, the documented tokens land in rejected[] with "Invalid enum value"
// and these assertions fail. Uses save:false so DefaultEngine.ini is never touched.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderingSetLumenMethodSoftwareRTModeTokenAliasTest,
    "PinWright.rendering.set_lumen_method.SoftwareRTModeTokenAlias",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRenderingSetLumenMethodSoftwareRTModeTokenAliasTest::RunTest(const FString& Parameters)
{
    URendererSettings* S = GetMutableDefault<URendererSettings>();
    if (!TestNotNull(TEXT("URendererSettings CDO present"), S)) return false;

    const TEnumAsByte<ELumenSoftwareTracingMode::Type> OriginalMode = S->LumenSoftwareTracingMode;
    ON_SCOPE_EXIT
    {
        // Restore in-memory state regardless of outcome (save:false → ini untouched).
        URendererSettings* SR = GetMutableDefault<URendererSettings>();
        if (SR) SR->LumenSoftwareTracingMode = OriginalMode;
    };

    // Helper: invoke set_lumen_method with the given softwareRTMode token, assert it
    // applies (not rejected) and lands the expected enum byte value on the CDO.
    auto CheckToken = [this, S](const TCHAR* Token, ELumenSoftwareTracingMode::Type Expected)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("softwareRTMode"), Token);
        Payload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture Capture;
        const FString Label = FString::Printf(TEXT("set_lumen_method softwareRTMode=%s"), Token);
        TestTrue(*FString::Printf(TEXT("%s invoked"), *Label),
            InvokeHandlerWithCapture(TEXT("rendering.set_lumen_method"), Payload, Capture));
        if (!TestTrue(*FString::Printf(TEXT("%s returned success"), *Label), Capture.bSuccess)) return;
        if (!TestTrue(*FString::Printf(TEXT("%s result valid"), *Label), Capture.Result.IsValid())) return;

        // The documented friendly token must NOT be rejected.
        TestFalse(*FString::Printf(TEXT("%s not in rejected[]"), *Label),
            JsonArrayHasObjectWithStringField(Capture.Result, TEXT("rejected"),
                TEXT("name"), TEXT("LumenSoftwareTracingMode")));

        // The applied[] array holds bare UPROPERTY-name strings; assert the field
        // was applied via the shared string-array membership walk.
        TestTrue(*FString::Printf(TEXT("%s applied LumenSoftwareTracingMode"), *Label),
            JsonStringArrayContains(Capture.Result, TEXT("applied"), TEXT("LumenSoftwareTracingMode")));

        // The CDO must hold the correct underlying enum value.
        TestEqual(*FString::Printf(TEXT("%s set CDO to expected enum value"), *Label),
            (int32)S->LumenSoftwareTracingMode.GetValue(), (int32)Expected);
    };

    CheckToken(TEXT("Detail"), ELumenSoftwareTracingMode::DetailTracing);
    CheckToken(TEXT("Global"), ELumenSoftwareTracingMode::GlobalTracing);

    // Raw enum identifiers must keep working (no regression to the pre-existing path).
    CheckToken(TEXT("DetailTracing"), ELumenSoftwareTracingMode::DetailTracing);
    CheckToken(TEXT("GlobalTracing"), ELumenSoftwareTracingMode::GlobalTracing);

    return true;
}
