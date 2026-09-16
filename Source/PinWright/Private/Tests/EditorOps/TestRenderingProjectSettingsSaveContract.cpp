// Copyright (c) 2026 Alexander Penkin. MIT License.

// Behavioural regression for B-rendering-save-false-success. The handler used to
// discard TryUpdateDefaultConfigFile's false result and return the expected path as
// savedTo. A read-only DefaultEngine.ini is the engine's documented refusal path.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"

#include "Engine/RendererSettings.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderingProjectSettingsReportsConfigSaveFailureTest,
    "PinWright.rendering.set_project_settings.ReportsConfigSaveFailure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRenderingProjectSettingsReportsConfigSaveFailureTest::RunTest(const FString& Parameters)
{
    URendererSettings* Settings = GetMutableDefault<URendererSettings>();
    if (!TestNotNull(TEXT("URendererSettings CDO present"), Settings))
    {
        return false;
    }

    const FString ConfigFile = Settings->GetDefaultConfigFilename();
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    if (!TestTrue(TEXT("DefaultEngine.ini exists for the refusal test"),
        PlatformFile.FileExists(*ConfigFile)))
    {
        return false;
    }

    const bool bOriginalValue = Settings->bDefaultFeatureAutoExposure != 0;
    const bool bWasReadOnly = PlatformFile.IsReadOnly(*ConfigFile);
    if (!bWasReadOnly && !TestTrue(TEXT("DefaultEngine.ini made read-only"),
        PlatformFile.SetReadOnly(*ConfigFile, true)))
    {
        return false;
    }
    ON_SCOPE_EXIT
    {
        Settings->bDefaultFeatureAutoExposure = bOriginalValue ? 1 : 0;
        PlatformFile.SetReadOnly(*ConfigFile, bWasReadOnly);
    };

    TSharedPtr<FJsonObject> Updates = MakeShared<FJsonObject>();
    Updates->SetBoolField(TEXT("bDefaultFeatureAutoExposure"), !bOriginalValue);
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("updates"), Updates);
    Payload->SetBoolField(TEXT("save"), true);

    AddExpectedError(TEXT("is read-only and cannot be written to"),
        EAutomationExpectedErrorFlags::Contains, 1);
    FTestResponseCapture Capture;
    TestTrue(TEXT("rendering.set_project_settings dispatched"),
        InvokeHandlerWithCapture(TEXT("rendering.set_project_settings"), Payload, Capture));
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    TestFalse(TEXT("read-only config is not a successful persistence receipt"), Capture.bSuccess);
    TestEqual(TEXT("save refusal has typed error"), Capture.ErrorCode, FString(TEXT("SAVE_FAILED")));
    if (Capture.bSuccess || !TestTrue(TEXT("failure includes measured result"), Capture.Result.IsValid()))
    {
        return false;
    }

    TestTrue(TEXT("save request is reported"), Capture.Result->GetBoolField(TEXT("saveRequested")));
    TestFalse(TEXT("failed config write is not saved"), Capture.Result->GetBoolField(TEXT("saved")));
    TestEqual(TEXT("failed config write has failed state"),
        Capture.Result->GetStringField(TEXT("saveState")), FString(TEXT("failed")));
    TestFalse(TEXT("savedTo is absent when persistence failed"),
        Capture.Result->HasField(TEXT("savedTo")));
    TestEqual(TEXT("the setting was still applied in memory"),
        Settings->bDefaultFeatureAutoExposure != 0, !bOriginalValue);
    return true;
}
