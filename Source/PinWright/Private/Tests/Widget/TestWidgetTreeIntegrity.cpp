// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"

#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Handlers/UI/WidgetAuthoringUtils.h"
#include "Tests/Widget/WidgetTestFixtures.h"

#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/PanelSlot.h"
#include "Components/TextBlock.h"
#include "Dom/JsonObject.h"
#include "Misc/ScopeExit.h"
#include "WidgetBlueprint.h"

namespace
{
    bool ArePanelSlotsValid(UPanelWidget* Panel)
    {
        if (!Panel)
        {
            return false;
        }

        const TArray<UPanelSlot*>& Slots = Panel->GetSlots();
        for (int32 SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
        {
            FString InvalidReason;
            if (WidgetAuthoringHelpers::TryGetInvalidPanelSlotReason(Panel, SlotIndex, nullptr, InvalidReason))
            {
                return false;
            }
        }
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetRemoveCompactsInvalidPanelSlotsTest,
    "PinWright.Widget.TreeIntegrity.RemoveCompactsInvalidPanelSlots",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetRemoveCompactsInvalidPanelSlotsTest::RunTest(const FString& Parameters)
{
    const FString WidgetPath = WidgetTestFixtures::MakeWidgetAssetPath(TEXT("WBP_RemoveCompactsSlots"));
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

    UTextBlock* StaleLabel = WidgetTestFixtures::AddTextBlockToPanel(WBP, Root, TEXT("StaleLabel"));
    UTextBlock* RemoveMe = WidgetTestFixtures::AddTextBlockToPanel(WBP, Root, TEXT("RemoveMe"));
    TestNotNull(TEXT("stale label allocated"), StaleLabel);
    TestNotNull(TEXT("remove target allocated"), RemoveMe);
    if (!Root || !StaleLabel || !RemoveMe)
    {
        return false;
    }

    UPanelSlot* StaleSlot = StaleLabel->Slot;
    TestNotNull(TEXT("stale slot allocated"), StaleSlot);
    if (!StaleSlot)
    {
        return false;
    }
    UPanelSlot* NullContentSlot = NewObject<UCanvasPanelSlot>(
        Root, UCanvasPanelSlot::StaticClass(), NAME_None, RF_Transient | RF_Transactional);
    TestNotNull(TEXT("null-content slot allocated"), NullContentSlot);
    UPanelSlot* DuplicateStaleSlot = NewObject<UCanvasPanelSlot>(
        Root, UCanvasPanelSlot::StaticClass(), NAME_None, RF_Transient | RF_Transactional);
    TestNotNull(TEXT("duplicate stale slot allocated"), DuplicateStaleSlot);
    if (!NullContentSlot || !DuplicateStaleSlot)
    {
        return false;
    }
    NullContentSlot->Parent = Root;
    DuplicateStaleSlot->Parent = Root;
    DuplicateStaleSlot->Content = StaleLabel;

    TArray<UPanelSlot*>& RootSlots = const_cast<TArray<UPanelSlot*>&>(Root->GetSlots());
    RootSlots.Add(NullContentSlot);
    RootSlots.Add(DuplicateStaleSlot);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), WidgetPath);
    Payload->SetStringField(TEXT("slotName"), TEXT("RemoveMe"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.remove_widget handler found"),
        InvokeHandlerWithCapture(TEXT("widget.remove_widget"), Payload, Capture));
    TestTrue(TEXT("response was sent"), Capture.bWasCalled);
    TestTrue(TEXT("response is success"), Capture.bSuccess);
    TestNotNull(TEXT("response result"), Capture.Result.Get());
    if (!Capture.Result.IsValid())
    {
        return false;
    }

    double CompactedPanelSlots = 0.0;
    TestTrue(TEXT("compactedPanelSlots returned"),
        Capture.Result->TryGetNumberField(TEXT("compactedPanelSlots"), CompactedPanelSlots));
    TestTrue(TEXT("planted invalid slots were compacted"),
        CompactedPanelSlots >= 2.0);
    // FAutomationTestBase::TestSamePtr was added after UE 5.4 (it exists on 5.6/5.7
    // but not 5.4). An explicit pointer-identity comparison via TestTrue is equivalent
    // and compiles on every supported engine version.
    TestTrue(TEXT("stale duplicate compaction preserves valid content slot"),
        StaleLabel->Slot.Get() == StaleSlot);
    TestTrue(TEXT("stale duplicate content remains in parent"),
        Root->GetChildIndex(StaleLabel) != INDEX_NONE);
    TestEqual(TEXT("only the valid remaining child slot remains"),
        Root->GetChildrenCount(), 1);
    TestTrue(TEXT("remaining panel slots have valid content/parent/backref"),
        ArePanelSlotsValid(Root));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetTreeIntegrityDetectsInvalidPanelSlotTest,
    "PinWright.Widget.TreeIntegrity.DetectsInvalidPanelSlot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetTreeIntegrityDetectsInvalidPanelSlotTest::RunTest(const FString& Parameters)
{
    const FString WidgetPath = WidgetTestFixtures::MakeWidgetAssetPath(TEXT("WBP_DetectsInvalidSlot"));
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

    UTextBlock* CorruptLabel = WidgetTestFixtures::AddTextBlockToPanel(WBP, Root, TEXT("CorruptLabel"));
    TestNotNull(TEXT("corrupt label allocated"), CorruptLabel);
    if (!Root || !CorruptLabel || !CorruptLabel->Slot)
    {
        return false;
    }
    CorruptLabel->Slot->Content = nullptr;

    TArray<BlueprintHandlerUtils::FBlueprintIntegrityFailure> Failures;
    const bool bResult = BlueprintHandlerUtils::ValidateBlueprintGraphIntegrity(WBP, Failures);
    TestFalse(TEXT("invalid panel slot fails integrity validation"), bResult);

    bool bFoundPanelSlotFailure = false;
    for (const BlueprintHandlerUtils::FBlueprintIntegrityFailure& Failure : Failures)
    {
        if (Failure.NodeKind == TEXT("UPanelSlot")
            && Failure.GraphName == TEXT("WidgetTree")
            && Failure.Reason.Contains(TEXT("Content is null")))
        {
            bFoundPanelSlotFailure = true;
            break;
        }
    }
    TestTrue(TEXT("validator reports widget-tree panel-slot failure"), bFoundPanelSlotFailure);

    return true;
}
