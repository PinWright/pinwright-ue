// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Dispatch/RpcDispatcher.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/UI/LiveUiSnapshot.h"
#include "Handlers/UI/LiveUiSnapshotJsonWriter.h"

namespace
{
    static TSharedPtr<FJsonValueObject> ObjectValue(const TSharedPtr<FJsonObject>& Obj)
    {
        return MakeShared<FJsonValueObject>(Obj);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDescribeTextFormatterShowsStructuredSlotTest,
    "PinWright.Widget.Describe.TextFormatterShowsStructuredSlot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWidgetDescribeTextFormatterShowsStructuredSlotTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;
    Dispatcher.DrainFormatterRegistrations();

    const FRpcFormatterFunc* Formatter = Dispatcher.GetFormatters().Find(TEXT("widget.describe"));
    TestNotNull(TEXT("Registered widget.describe text formatter"), Formatter);
    if (!Formatter)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Offsets = MakeShared<FJsonObject>();
    Offsets->SetNumberField(TEXT("Left"), 10.0);
    Offsets->SetNumberField(TEXT("Top"), 20.0);
    Offsets->SetNumberField(TEXT("Right"), 400.0);
    Offsets->SetNumberField(TEXT("Bottom"), 240.0);

    TSharedPtr<FJsonObject> AnchorMin = MakeShared<FJsonObject>();
    AnchorMin->SetNumberField(TEXT("X"), 0.0);
    AnchorMin->SetNumberField(TEXT("Y"), 0.0);

    TSharedPtr<FJsonObject> AnchorMax = MakeShared<FJsonObject>();
    AnchorMax->SetNumberField(TEXT("X"), 0.0);
    AnchorMax->SetNumberField(TEXT("Y"), 1.0);

    TSharedPtr<FJsonObject> Anchors = MakeShared<FJsonObject>();
    Anchors->SetField(TEXT("Minimum"), ObjectValue(AnchorMin));
    Anchors->SetField(TEXT("Maximum"), ObjectValue(AnchorMax));

    TSharedPtr<FJsonObject> LayoutData = MakeShared<FJsonObject>();
    LayoutData->SetField(TEXT("Offsets"), ObjectValue(Offsets));
    LayoutData->SetField(TEXT("Anchors"), ObjectValue(Anchors));

    TSharedPtr<FJsonObject> SlotProps = MakeShared<FJsonObject>();
    SlotProps->SetField(TEXT("LayoutData"), ObjectValue(LayoutData));
    SlotProps->SetNumberField(TEXT("ZOrder"), 7.0);

    TSharedPtr<FJsonObject> Slot = MakeShared<FJsonObject>();
    Slot->SetStringField(TEXT("type"), TEXT("CanvasPanelSlot"));
    Slot->SetObjectField(TEXT("props"), SlotProps);

    TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
    Props->SetStringField(TEXT("Slot"), TEXT("/Game/UI/ReplayEditor/WBP_ReplayEditor.WBP_ReplayEditor_C:WidgetTree.Timeline.Slot"));

    TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
    Node->SetStringField(TEXT("type"), TEXT("W_ReplayEditor_Timeline_C"));
    Node->SetStringField(TEXT("name"), TEXT("Timeline"));
    Node->SetObjectField(TEXT("props"), Props);
    Node->SetObjectField(TEXT("slot"), Slot);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), TEXT("/Game/UI/W_Test"));
    Result->SetStringField(TEXT("root_class"), TEXT("UserWidget"));
    Result->SetNumberField(TEXT("widget_count"), 1.0);
    Result->SetObjectField(TEXT("tree"), Node);

    FString Text;
    TestTrue(TEXT("Formatter accepts widget.describe result"), (*Formatter)(Result, Text));
    TestTrue(TEXT("Text includes slot type"), Text.Contains(TEXT("CanvasPanelSlot")));
    TestTrue(TEXT("Text includes layout data"), Text.Contains(TEXT("LayoutData")));
    TestTrue(TEXT("Text includes offsets"), Text.Contains(TEXT("Offsets=")));
    TestTrue(TEXT("Text includes anchors"), Text.Contains(TEXT("Anchors=")));
    TestFalse(TEXT("Structured slot suppresses raw props.Slot path"), Text.Contains(TEXT("Slot=/Game/UI/ReplayEditor")));
    TestFalse(TEXT("Structured slot avoids truncated raw Slot ellipsis"), Text.Contains(TEXT("WBP_ReplayEdito...")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDescribeTextFormatterRendersLiveSnapshotTest,
    "PinWright.Widget.Describe.TextFormatterRendersLiveSnapshot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWidgetDescribeTextFormatterRendersLiveSnapshotTest::RunTest(const FString& Parameters)
{
    FLiveUiSnapshot Snapshot;
    Snapshot.CaptureSource = TEXT("live");
    Snapshot.ViewportSize = FVector2D(1920.0, 1080.0);
    Snapshot.RootNode.SlateType = TEXT("SConstraintCanvas");
    Snapshot.RootNode.DebugName = TEXT("SConstraintCanvas");

    FLiveUiSnapshotNode Child;
    Child.SlateType = TEXT("SObjectWidget");
    Child.DebugName = TEXT("SObjectWidget");
    Child.SourceInfo.bHasBackingWidget = true;
    Child.SourceInfo.WidgetName = TEXT("WBP_RootLayout_C_0");
    Child.SourceInfo.OwningUserWidgetClassPath = TEXT("/Game/UI/WBP_RootLayout.WBP_RootLayout_C");
    Child.SourceInfo.WidgetBlueprintPath = TEXT("/Game/UI/WBP_RootLayout.WBP_RootLayout");
    Snapshot.RootNode.Children.Add(Child);

    TSharedPtr<FJsonObject> Json = FLiveUiSnapshotJsonWriter::Write(Snapshot);

    FRpcDispatcher Dispatcher;
    Dispatcher.DrainFormatterRegistrations();

    const FRpcFormatterFunc* Formatter = Dispatcher.GetFormatters().Find(TEXT("widget.describe"));
    TestNotNull(TEXT("Registered widget.describe text formatter"), Formatter);
    if (!Formatter)
    {
        return false;
    }

    FString Text;
    const bool bOk = (*Formatter)(Json, Text);
    TestTrue(TEXT("Formatter accepts live snapshot"), bOk);
    TestFalse(TEXT("Output does not collapse to 'Widget: Unknown'"), Text.Contains(TEXT("Widget: Unknown")));
    TestTrue(TEXT("Output mentions Live capture"), Text.Contains(TEXT("Live")));
    TestTrue(TEXT("Output includes the hosted user widget name"), Text.Contains(TEXT("WBP_RootLayout_C_0")));
    TestTrue(TEXT("Output includes the SObjectWidget slate type"), Text.Contains(TEXT("SObjectWidget")));

    return true;
}
