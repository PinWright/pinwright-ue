// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit test for system.inspect.list_subsystems
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"

// ============================================================================
// Editor scope must always include UAssetEditorSubsystem (loaded in any
// editor-context automation run).  Counterfactual: omit the Editor branch
// from the handler and the assertion below fails.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSubsystemInspectListEditorScopeTest,
    "PinWright.system.inspect.list_subsystems.EditorScopeReturnsKnownSubsystem",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSubsystemInspectListEditorScopeTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("scope"), TEXT("Editor"));

    FTestResponseCapture Capture;
    if (!InvokeHandlerWithCapture(TEXT("system.inspect.list_subsystems"), Payload, Capture))
    {
        AddError(TEXT("Handler 'system.inspect.list_subsystems' not registered"));
        return false;
    }

    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    TestTrue(TEXT("response is success"), Capture.bSuccess);
    if (!Capture.Result.IsValid())
    {
        AddError(TEXT("result object is invalid"));
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Subsystems = nullptr;
    if (!Capture.Result->TryGetArrayField(TEXT("subsystems"), Subsystems) || !Subsystems)
    {
        AddError(TEXT("'subsystems' field missing or not an array"));
        return false;
    }

    bool bFoundAssetEditorSubsystem = false;
    for (const TSharedPtr<FJsonValue>& EntryVal : *Subsystems)
    {
        const TSharedPtr<FJsonObject>* EntryObj = nullptr;
        if (!EntryVal.IsValid() || !EntryVal->TryGetObject(EntryObj) || !EntryObj) continue;
        FString ClassName, EntryScope;
        (*EntryObj)->TryGetStringField(TEXT("className"), ClassName);
        (*EntryObj)->TryGetStringField(TEXT("scope"), EntryScope);
        if (ClassName == TEXT("AssetEditorSubsystem") && EntryScope == TEXT("Editor"))
        {
            bFoundAssetEditorSubsystem = true;
            break;
        }
    }
    TestTrue(TEXT("Editor scope contains AssetEditorSubsystem"), bFoundAssetEditorSubsystem);
    return true;
}
