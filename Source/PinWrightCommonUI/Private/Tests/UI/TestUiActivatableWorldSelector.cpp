// Copyright (c) 2026 Alexander Penkin. MIT License.

// F-ui-runtime-verbs-pie-instance-selector: the ui.activatable_* verbs accept editor.console_command's
// `world` selector (PieWorldSelector::ResolveGameWorld; its contract is unit-tested in
// PinWright.ui.pie_world.ResolveGameWorldContract). Multi-client PIE cannot be started in a headless
// suite, so this asserts the selector is routed for both addressing modes with no PIE running.
// Before the fix `world` was ignored and these answered LAYER_HOST_UNAVAILABLE / HOST_NOT_FOUND.

#include "Misc/AutomationTest.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "Editor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiActivatableWorldSelectorRoutedTest,
    "PinWright.ui.activatable.WorldSelectorRouted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUiActivatableWorldSelectorRoutedTest::RunTest(const FString& Parameters)
{
    if (!GEditor || GEditor->PlayWorld || GEditor->IsPlaySessionInProgress())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("preexisting_pie_session"),
            TEXT("The no-PIE routing assertions need an editor with no PIE session running."));
        return true;
    }

    const TCHAR* Methods[] = {
        TEXT("ui.activatable_push"), TEXT("ui.activatable_pop"),
        TEXT("ui.list_stack_widgets"), TEXT("ui.get_active_widget")};
    for (const TCHAR* Method : Methods)
    {
        for (const bool bLayerTag : {true, false})
        {
            TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
            // A resolvable activatable class, so push gets past its class check to target resolution.
            Payload->SetStringField(TEXT("widgetClass"), TEXT("/Script/CommonUI.CommonActivatableWidget"));
            if (bLayerTag)
            {
                Payload->SetStringField(TEXT("layerTag"), TEXT("UI.Layer.Menu"));
            }
            else
            {
                Payload->SetStringField(TEXT("host"), TEXT("PW_ActivatableWorldProbeHost"));
                Payload->SetStringField(TEXT("stack"), TEXT("PW_ActivatableWorldProbeStack"));
            }
            const FString Mode = bLayerTag ? TEXT("layerTag") : TEXT("host+stack");

            Payload->SetStringField(TEXT("world"), TEXT("client:1"));
            FTestResponseCapture Capture;
            TestTrue(FString::Printf(TEXT("%s handler registered"), Method),
                InvokeHandlerWithCapture(Method, Payload, Capture));
            TestFalse(FString::Printf(TEXT("%s (%s) world:'client:1' without PIE does not succeed"), Method, *Mode),
                Capture.bSuccess);
            TestEqual(FString::Printf(TEXT("%s (%s) world:'client:1' without PIE: WORLD_NOT_FOUND"), Method, *Mode),
                Capture.ErrorCode, FString(TEXT("WORLD_NOT_FOUND")));

            Payload->SetStringField(TEXT("world"), TEXT("editor"));
            FTestResponseCapture EditorCapture;
            InvokeHandlerWithCapture(Method, Payload, EditorCapture);
            TestEqual(FString::Printf(TEXT("%s (%s) world:'editor': INVALID_ARGUMENT"), Method, *Mode),
                EditorCapture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        }
    }
    return true;
}
