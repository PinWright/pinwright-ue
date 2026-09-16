// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "WidgetXmlTestHelpers.h"

#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/TextBlock.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"


namespace
{
    FString MakeWidgetDuplicateAssetPath(const FString& Prefix)
    {
        return FString::Printf(TEXT("/Game/_Test/%s_%s"),
            *Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    UWidgetBlueprint* MakeWidgetDuplicateBlueprint(const FString& PackagePath)
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDuplicateCopiesSubtreeSlotAndPlacementTest,
    "PinWright.widget.duplicate.CopiesSubtreeSlotAndPlacement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetDuplicateCopiesSubtreeSlotAndPlacementTest::RunTest(const FString& Parameters)
{
    const FString WidgetPath = MakeWidgetDuplicateAssetPath(TEXT("WBP_DuplicateWidget"));
    ON_SCOPE_EXIT
    {
        WidgetXmlTestHelpers::CleanupXmlTestAsset(WidgetPath);
    };

    UWidgetBlueprint* WBP = MakeWidgetDuplicateBlueprint(WidgetPath);
    TestNotNull(TEXT("widget blueprint allocated"), WBP);
    if (!WBP || !WBP->WidgetTree)
    {
        return false;
    }

    UCanvasPanel* Root = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(
        UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
    WBP->WidgetTree->RootWidget = Root;

    UTextBlock* Before = WBP->WidgetTree->ConstructWidget<UTextBlock>(
        UTextBlock::StaticClass(), TEXT("Before"));
    Root->AddChild(Before);

    UHorizontalBox* SourceRow = WBP->WidgetTree->ConstructWidget<UHorizontalBox>(
        UHorizontalBox::StaticClass(), TEXT("SourceRow"));
    UCanvasPanelSlot* SourceSlot = Cast<UCanvasPanelSlot>(Root->AddChild(SourceRow));
    TestNotNull(TEXT("source row has canvas slot"), SourceSlot);
    if (!SourceSlot)
    {
        return false;
    }
    SourceSlot->SetPosition(FVector2D(42.0f, 84.0f));
    SourceSlot->SetSize(FVector2D(320.0f, 48.0f));

    UTextBlock* ChildLabel = WBP->WidgetTree->ConstructWidget<UTextBlock>(
        UTextBlock::StaticClass(), TEXT("ChildLabel"));
    SourceRow->AddChild(ChildLabel);

    TSharedPtr<FJsonObject> Placement = MakeShared<FJsonObject>();
    Placement->SetStringField(TEXT("after"), TEXT("SourceRow"));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), WidgetPath);
    Payload->SetStringField(TEXT("sourceName"), TEXT("SourceRow"));
    Payload->SetStringField(TEXT("name"), TEXT("SourceRow_Copy"));
    Payload->SetObjectField(TEXT("placement"), Placement);
    Payload->SetBoolField(TEXT("duplicateChildren"), true);
    Payload->SetBoolField(TEXT("copySlot"), true);

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.duplicate handler found"),
        InvokeHandlerWithCapture(TEXT("widget.duplicate"), Payload, Capture));
    TestTrue(TEXT("widget.duplicate succeeded"), Capture.bSuccess);

    UWidget* DuplicatedWidget = WBP->WidgetTree->FindWidget(FName(TEXT("SourceRow_Copy")));
    TestNotNull(TEXT("duplicate exists"), DuplicatedWidget);
    UHorizontalBox* DuplicatedRow = Cast<UHorizontalBox>(DuplicatedWidget);
    TestNotNull(TEXT("duplicate preserves class"), DuplicatedRow);
    if (!DuplicatedRow)
    {
        return false;
    }

    TestEqual(TEXT("duplicate placed after source"), Root->GetChildIndex(DuplicatedRow), Root->GetChildIndex(SourceRow) + 1);
    UCanvasPanelSlot* DuplicateSlot = Cast<UCanvasPanelSlot>(DuplicatedRow->Slot);
    TestNotNull(TEXT("duplicate has canvas slot"), DuplicateSlot);
    if (DuplicateSlot)
    {
        TestEqual(TEXT("slot position copied"), DuplicateSlot->GetPosition(), FVector2D(42.0f, 84.0f));
        TestEqual(TEXT("slot size copied"), DuplicateSlot->GetSize(), FVector2D(320.0f, 48.0f));
    }

    TestEqual(TEXT("child duplicated"), DuplicatedRow->GetChildrenCount(), 1);
    if (DuplicatedRow->GetChildrenCount() == 1)
    {
        TestTrue(TEXT("duplicated child preserves class"),
            DuplicatedRow->GetChildAt(0)->IsA<UTextBlock>());
    }

    TestTrue(TEXT("mapping returned"), Capture.Result.IsValid() && Capture.Result->HasTypedField<EJson::Object>(TEXT("mapping")));
    if (Capture.Result.IsValid())
    {
        const TSharedPtr<FJsonObject> Mapping = Capture.Result->GetObjectField(TEXT("mapping"));
        FString MappedName;
        Mapping->TryGetStringField(TEXT("SourceRow"), MappedName);
        TestEqual(TEXT("root mapping returned"), MappedName, FString(TEXT("SourceRow_Copy")));
    }

    return true;
}
