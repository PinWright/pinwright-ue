// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Tests/Widget/WidgetTestFixtures.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/PanelSlot.h"
#include "Components/TextBlock.h"
#include "Misc/ScopeExit.h"
#include "WidgetBlueprint.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetSetRollsBackOnSlotErrorTest,
    "PinWright.widget.set.RollbackOnSlotError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetSetRollsBackOnSlotErrorTest::RunTest(const FString& Parameters)
{
    const FString WidgetPath = WidgetTestFixtures::MakeWidgetAssetPath(TEXT("WBP_WidgetSetRollback"));
    UWidgetBlueprint* WBP = WidgetTestFixtures::MakeTransientWidgetBlueprint(WidgetPath);
    TestNotNull(TEXT("widget blueprint allocated"), WBP);
    if (!WBP || !WBP->WidgetTree)
    {
        return false;
    }

    ON_SCOPE_EXIT
    {
        if (UPackage* Package = WBP->GetOutermost())
        {
            Package->SetDirtyFlag(false);
        }
        CleanupTestAsset(WidgetPath);
    };

    UCanvasPanel* Root = WidgetTestFixtures::AddCanvasRoot(WBP);
    TestNotNull(TEXT("root canvas allocated"), Root);

    UTextBlock* Label = WidgetTestFixtures::AddTextBlockToPanel(WBP, Root, TEXT("RollbackLabel"));
    TestNotNull(TEXT("label allocated"), Label);
    if (!Root || !Label)
    {
        return false;
    }

    UPanelSlot* LabelSlot = Label->Slot;
    TestNotNull(TEXT("label added to canvas"), LabelSlot);
    if (!LabelSlot || !Label->Slot)
    {
        return false;
    }

    Label->SetRenderOpacity(0.75f);
    const float OriginalRenderOpacity = Label->GetRenderOpacity();

    TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetNumberField(TEXT("RenderOpacity"), 0.25);

    TSharedPtr<FJsonObject> Slot = MakeShared<FJsonObject>();
    Slot->SetNumberField(TEXT("NoSuchSlotProperty"), 10.0);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), WidgetPath);
    Payload->SetStringField(TEXT("widgetName"), TEXT("RollbackLabel"));
    Payload->SetObjectField(TEXT("properties"), Properties);
    Payload->SetObjectField(TEXT("slot"), Slot);

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.set handler found"),
        InvokeHandlerWithCapture(TEXT("widget.set"), Payload, Capture));
    TestTrue(TEXT("response was sent"), Capture.bWasCalled);
    TestFalse(TEXT("response is an error"), Capture.bSuccess);
    TestEqual(TEXT("slot failure returns INVALID_PROPERTY"),
        Capture.ErrorCode,
        FString(TEXT("INVALID_PROPERTY")));
    TestEqual(TEXT("RenderOpacity rolled back after slot error"),
        Label->GetRenderOpacity(),
        OriginalRenderOpacity);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetSetFailurePreservesPriorAddTest,
    "PinWright.widget.set.FailurePreservesPriorAdd",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetSetFailurePreservesPriorAddTest::RunTest(const FString& Parameters)
{
    const FString WidgetPath = WidgetTestFixtures::MakeWidgetAssetPath(TEXT("WBP_WidgetSetPriorAdd"));
    UWidgetBlueprint* WBP = WidgetTestFixtures::MakeTransientWidgetBlueprint(WidgetPath);
    TestNotNull(TEXT("widget blueprint allocated"), WBP);
    if (!WBP || !WBP->WidgetTree)
    {
        return false;
    }

    ON_SCOPE_EXIT
    {
        if (UPackage* Package = WBP->GetOutermost())
        {
            Package->SetDirtyFlag(false);
        }
        CleanupTestAsset(WidgetPath);
    };

    UCanvasPanel* Root = WidgetTestFixtures::AddCanvasRoot(WBP);
    TestNotNull(TEXT("root canvas allocated"), Root);
    if (!Root)
    {
        return false;
    }

    FTestResponseCapture Capture;

    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("widgetPath"), WidgetPath);
    AddPayload->SetStringField(TEXT("type"), TEXT("TextBlock"));
    AddPayload->SetStringField(TEXT("name"), TEXT("ReasonText"));
    AddPayload->SetStringField(TEXT("parentName"), TEXT("RootCanvas"));

    TestTrue(TEXT("widget.add handler found"),
        InvokeHandlerWithCapture(TEXT("widget.add"), AddPayload, Capture));
    TestTrue(TEXT("widget.add succeeded"), Capture.bSuccess);
    TestNotNull(TEXT("ReasonText exists after widget.add"),
        WBP->WidgetTree->FindWidget(FName(TEXT("ReasonText"))));

    TSharedPtr<FJsonObject> InvalidProperties = MakeShared<FJsonObject>();
    InvalidProperties->SetStringField(TEXT("Text"), TEXT("INVTEXT(\"\")"));

    TSharedPtr<FJsonObject> InvalidSetPayload = MakeShared<FJsonObject>();
    InvalidSetPayload->SetStringField(TEXT("widgetPath"), WidgetPath);
    InvalidSetPayload->SetStringField(TEXT("widgetName"), TEXT("ReasonText"));
    InvalidSetPayload->SetObjectField(TEXT("properties"), InvalidProperties);

    TestTrue(TEXT("widget.set handler found for invalid text"),
        InvokeHandlerWithCapture(TEXT("widget.set"), InvalidSetPayload, Capture));
    TestTrue(TEXT("invalid widget.set response was sent"), Capture.bWasCalled);
    TestFalse(TEXT("invalid widget.set failed"), Capture.bSuccess);
    TestEqual(TEXT("invalid widget.set returns INVALID_PROPERTY"),
        Capture.ErrorCode,
        FString(TEXT("INVALID_PROPERTY")));
    TestNotNull(TEXT("ReasonText remains after failed widget.set"),
        WBP->WidgetTree->FindWidget(FName(TEXT("ReasonText"))));

    TSharedPtr<FJsonObject> ValidProperties = MakeShared<FJsonObject>();
    ValidProperties->SetStringField(TEXT("Text"),
        TEXT("NSLOCTEXT(\"WidgetSetAtomicity\", \"ReasonText\", \"Valid reason\")"));

    TSharedPtr<FJsonObject> ValidSetPayload = MakeShared<FJsonObject>();
    ValidSetPayload->SetStringField(TEXT("widgetPath"), WidgetPath);
    ValidSetPayload->SetStringField(TEXT("widgetName"), TEXT("ReasonText"));
    ValidSetPayload->SetObjectField(TEXT("properties"), ValidProperties);

    TestTrue(TEXT("widget.set handler found for valid text"),
        InvokeHandlerWithCapture(TEXT("widget.set"), ValidSetPayload, Capture));
    TestTrue(TEXT("valid widget.set succeeds after failed set"), Capture.bSuccess);

    return true;
}
