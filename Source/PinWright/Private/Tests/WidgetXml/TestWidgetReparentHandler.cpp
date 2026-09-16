// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "WidgetXmlTestHelpers.h"

#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"


namespace
{
    FString MakeWidgetReparentAssetPath(const FString& Prefix)
    {
        return FString::Printf(TEXT("/Game/_Test/%s_%s"),
            *Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    UWidgetBlueprint* MakeWidgetReparentBlueprint(const FString& PackagePath)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            return nullptr;
        }
        Package->SetFlags(RF_Transient);

        UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
            Package, *AssetName, RF_Transient | RF_Public | RF_Standalone);
        if (!WBP)
        {
            return nullptr;
        }

        WBP->WidgetTree = NewObject<UWidgetTree>(WBP, NAME_None, RF_Transient);
        return WBP;
    }

    UTextBlock* AddTextChild(UWidgetTree* Tree, UPanelWidget* Parent, const TCHAR* Name)
    {
        UTextBlock* Child = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), Name);
        Parent->AddChild(Child);
        return Child;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetReparentPlacesBeforeSiblingTest,
    "PinWright.widget.reparent.PlacesBeforeSibling",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetReparentPlacesBeforeSiblingTest::RunTest(const FString& Parameters)
{
    const FString WidgetPath = MakeWidgetReparentAssetPath(TEXT("WBP_ReparentBefore"));
    ON_SCOPE_EXIT
    {
        WidgetXmlTestHelpers::CleanupXmlTestAsset(WidgetPath);
    };

    UWidgetBlueprint* WBP = MakeWidgetReparentBlueprint(WidgetPath);
    TestNotNull(TEXT("widget blueprint allocated"), WBP);
    if (!WBP || !WBP->WidgetTree)
    {
        return false;
    }

    UCanvasPanel* Root = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(
        UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
    WBP->WidgetTree->RootWidget = Root;

    UTextBlock* Moving = AddTextChild(WBP->WidgetTree, Root, TEXT("Moving"));
    UHorizontalBox* TargetPanel = WBP->WidgetTree->ConstructWidget<UHorizontalBox>(
        UHorizontalBox::StaticClass(), TEXT("TargetPanel"));
    Root->AddChild(TargetPanel);
    UTextBlock* First = AddTextChild(WBP->WidgetTree, TargetPanel, TEXT("First"));
    UTextBlock* Second = AddTextChild(WBP->WidgetTree, TargetPanel, TEXT("Second"));

    TSharedPtr<FJsonObject> Placement = MakeShared<FJsonObject>();
    Placement->SetStringField(TEXT("before"), TEXT("Second"));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), WidgetPath);
    Payload->SetStringField(TEXT("slotName"), TEXT("Moving"));
    Payload->SetStringField(TEXT("newParent"), TEXT("TargetPanel"));
    Payload->SetObjectField(TEXT("placement"), Placement);

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.reparent_widget handler found"),
        InvokeHandlerWithCapture(TEXT("widget.reparent_widget"), Payload, Capture));
    TestTrue(TEXT("widget.reparent_widget succeeded"), Capture.bSuccess);

    TestEqual(TEXT("moving parent changed"), Moving->GetParent(), Cast<UPanelWidget>(TargetPanel));
    TestEqual(TEXT("first remains first"), TargetPanel->GetChildIndex(First), 0);
    TestEqual(TEXT("moving inserted before second"), TargetPanel->GetChildIndex(Moving), 1);
    TestEqual(TEXT("second shifted after moving"), TargetPanel->GetChildIndex(Second), 2);
    if (Capture.Result.IsValid())
    {
        TestEqual(TEXT("insert index returned"), static_cast<int32>(Capture.Result->GetNumberField(TEXT("insertIndex"))), 1);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetReparentPlacesAtIndexTest,
    "PinWright.widget.reparent.PlacesAtIndex",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetReparentPlacesAtIndexTest::RunTest(const FString& Parameters)
{
    const FString WidgetPath = MakeWidgetReparentAssetPath(TEXT("WBP_ReparentIndex"));
    ON_SCOPE_EXIT
    {
        WidgetXmlTestHelpers::CleanupXmlTestAsset(WidgetPath);
    };

    UWidgetBlueprint* WBP = MakeWidgetReparentBlueprint(WidgetPath);
    TestNotNull(TEXT("widget blueprint allocated"), WBP);
    if (!WBP || !WBP->WidgetTree)
    {
        return false;
    }

    UCanvasPanel* Root = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(
        UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
    WBP->WidgetTree->RootWidget = Root;

    UTextBlock* Before = AddTextChild(WBP->WidgetTree, Root, TEXT("Before"));
    UTextBlock* Moving = AddTextChild(WBP->WidgetTree, Root, TEXT("Moving"));
    UTextBlock* After = AddTextChild(WBP->WidgetTree, Root, TEXT("After"));
    UCanvasPanelSlot* MovingSlot = Cast<UCanvasPanelSlot>(Moving->Slot);
    TestNotNull(TEXT("moving has canvas slot"), MovingSlot);
    if (!MovingSlot)
    {
        return false;
    }
    MovingSlot->SetPosition(FVector2D(12.0f, 34.0f));
    MovingSlot->SetSize(FVector2D(56.0f, 78.0f));

    TSharedPtr<FJsonObject> Placement = MakeShared<FJsonObject>();
    Placement->SetNumberField(TEXT("index"), 0);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), WidgetPath);
    Payload->SetStringField(TEXT("slotName"), TEXT("Moving"));
    Payload->SetStringField(TEXT("newParent"), TEXT("RootCanvas"));
    Payload->SetObjectField(TEXT("placement"), Placement);

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.reparent_widget handler found"),
        InvokeHandlerWithCapture(TEXT("widget.reparent_widget"), Payload, Capture));
    TestTrue(TEXT("widget.reparent_widget succeeded"), Capture.bSuccess);

    TestEqual(TEXT("moving inserted at index"), Root->GetChildIndex(Moving), 0);
    TestEqual(TEXT("before shifted after moving"), Root->GetChildIndex(Before), 1);
    TestEqual(TEXT("after remains last"), Root->GetChildIndex(After), 2);
    UCanvasPanelSlot* NewMovingSlot = Cast<UCanvasPanelSlot>(Moving->Slot);
    TestNotNull(TEXT("moving still has canvas slot"), NewMovingSlot);
    if (NewMovingSlot)
    {
        TestEqual(TEXT("slot position preserved"), NewMovingSlot->GetPosition(), FVector2D(12.0f, 34.0f));
        TestEqual(TEXT("slot size preserved"), NewMovingSlot->GetSize(), FVector2D(56.0f, 78.0f));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetReparentPlacesAfterSiblingInSameParentTest,
    "PinWright.widget.reparent.PlacesAfterSiblingInSameParent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetReparentPlacesAfterSiblingInSameParentTest::RunTest(const FString& Parameters)
{
    const FString WidgetPath = MakeWidgetReparentAssetPath(TEXT("WBP_ReparentAfterSameParent"));
    ON_SCOPE_EXIT
    {
        WidgetXmlTestHelpers::CleanupXmlTestAsset(WidgetPath);
    };

    UWidgetBlueprint* WBP = MakeWidgetReparentBlueprint(WidgetPath);
    TestNotNull(TEXT("widget blueprint allocated"), WBP);
    if (!WBP || !WBP->WidgetTree)
    {
        return false;
    }

    UCanvasPanel* Root = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(
        UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
    WBP->WidgetTree->RootWidget = Root;

    UTextBlock* Moving = AddTextChild(WBP->WidgetTree, Root, TEXT("Moving"));
    UTextBlock* Target = AddTextChild(WBP->WidgetTree, Root, TEXT("Target"));
    UTextBlock* Last = AddTextChild(WBP->WidgetTree, Root, TEXT("Last"));

    TSharedPtr<FJsonObject> Placement = MakeShared<FJsonObject>();
    Placement->SetStringField(TEXT("after"), TEXT("Target"));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), WidgetPath);
    Payload->SetStringField(TEXT("slotName"), TEXT("Moving"));
    Payload->SetStringField(TEXT("newParent"), TEXT("RootCanvas"));
    Payload->SetObjectField(TEXT("placement"), Placement);

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.reparent_widget handler found"),
        InvokeHandlerWithCapture(TEXT("widget.reparent_widget"), Payload, Capture));
    TestTrue(TEXT("widget.reparent_widget succeeded"), Capture.bSuccess);

    TestEqual(TEXT("target remains first"), Root->GetChildIndex(Target), 0);
    TestEqual(TEXT("moving inserted after target"), Root->GetChildIndex(Moving), 1);
    TestEqual(TEXT("last remains last"), Root->GetChildIndex(Last), 2);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetReparentRejectsInvalidPlacementTest,
    "PinWright.widget.reparent.RejectsInvalidPlacement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetReparentRejectsInvalidPlacementTest::RunTest(const FString& Parameters)
{
    const FString WidgetPath = MakeWidgetReparentAssetPath(TEXT("WBP_ReparentInvalid"));
    ON_SCOPE_EXIT
    {
        WidgetXmlTestHelpers::CleanupXmlTestAsset(WidgetPath);
    };

    UWidgetBlueprint* WBP = MakeWidgetReparentBlueprint(WidgetPath);
    TestNotNull(TEXT("widget blueprint allocated"), WBP);
    if (!WBP || !WBP->WidgetTree)
    {
        return false;
    }

    UCanvasPanel* Root = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(
        UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
    WBP->WidgetTree->RootWidget = Root;

    AddTextChild(WBP->WidgetTree, Root, TEXT("Moving"));
    UHorizontalBox* TargetPanel = WBP->WidgetTree->ConstructWidget<UHorizontalBox>(
        UHorizontalBox::StaticClass(), TEXT("TargetPanel"));
    Root->AddChild(TargetPanel);

    TSharedPtr<FJsonObject> Placement = MakeShared<FJsonObject>();
    Placement->SetStringField(TEXT("after"), TEXT("MissingSibling"));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), WidgetPath);
    Payload->SetStringField(TEXT("slotName"), TEXT("Moving"));
    Payload->SetStringField(TEXT("newParent"), TEXT("TargetPanel"));
    Payload->SetObjectField(TEXT("placement"), Placement);

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.reparent_widget handler found"),
        InvokeHandlerWithCapture(TEXT("widget.reparent_widget"), Payload, Capture));
    TestFalse(TEXT("widget.reparent_widget rejected invalid placement"), Capture.bSuccess);
    TestEqual(TEXT("invalid placement error code"), Capture.ErrorCode, FString(TEXT("INVALID_PLACEMENT")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetReparentRejectsDescendantParentTest,
    "PinWright.widget.reparent.RejectsDescendantParent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetReparentRejectsDescendantParentTest::RunTest(const FString& Parameters)
{
    const FString WidgetPath = MakeWidgetReparentAssetPath(TEXT("WBP_ReparentDescendant"));
    ON_SCOPE_EXIT
    {
        WidgetXmlTestHelpers::CleanupXmlTestAsset(WidgetPath);
    };

    UWidgetBlueprint* WBP = MakeWidgetReparentBlueprint(WidgetPath);
    TestNotNull(TEXT("widget blueprint allocated"), WBP);
    if (!WBP || !WBP->WidgetTree)
    {
        return false;
    }

    UCanvasPanel* Root = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(
        UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
    WBP->WidgetTree->RootWidget = Root;

    UHorizontalBox* Parent = WBP->WidgetTree->ConstructWidget<UHorizontalBox>(
        UHorizontalBox::StaticClass(), TEXT("ParentPanel"));
    Root->AddChild(Parent);
    UHorizontalBox* ChildPanel = WBP->WidgetTree->ConstructWidget<UHorizontalBox>(
        UHorizontalBox::StaticClass(), TEXT("ChildPanel"));
    Parent->AddChild(ChildPanel);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), WidgetPath);
    Payload->SetStringField(TEXT("slotName"), TEXT("ParentPanel"));
    Payload->SetStringField(TEXT("newParent"), TEXT("ChildPanel"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.reparent_widget handler found"),
        InvokeHandlerWithCapture(TEXT("widget.reparent_widget"), Payload, Capture));
    TestFalse(TEXT("widget.reparent_widget rejected descendant parent"), Capture.bSuccess);
    TestEqual(TEXT("invalid parent error code"), Capture.ErrorCode, FString(TEXT("INVALID_PARENT")));

    return true;
}
