// Copyright (c) 2026 Alexander Penkin. MIT License.

// Counterfactual: reverting bOmitSlotChain=true at the asset-dump call sites that invoke
// WidgetXmlExporter::BuildWidgetTreeXmlWithDiagnostic causes UPanelSlot::Parent/Content
// recursion to re-emit _kind=cycle and _kind=max_depth markers, failing this test.
#include "Misc/AutomationTest.h"
#include "WidgetXmlTestHelpers.h"
#include "Handlers/UI/WidgetXmlExporter.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Overlay.h"
#include "Components/TextBlock.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpWidgetTreeXmlOmitsSlotChainTest,
    "PinWright.widget.export_xml.AssetDumpOmitsSlotChain",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAssetDumpWidgetTreeXmlOmitsSlotChainTest::RunTest(const FString& Parameters)
{
    const FString PackagePath = WidgetXmlTestHelpers::MakeXmlTestAssetPath(TEXT("WBP_SlotChain"));

    UWidgetBlueprint* WBP = WidgetXmlTestHelpers::MakeOnDiskShapedWidgetBlueprint(PackagePath);
    if (!TestNotNull(TEXT("WBP constructed"), WBP))
    {
        return false;
    }

    UOverlay* RootOverlay = WBP->WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass());
    if (!TestNotNull(TEXT("Overlay constructed"), RootOverlay))
    {
        WidgetXmlTestHelpers::CleanupXmlTestAsset(PackagePath);
        return false;
    }
    WBP->WidgetTree->RootWidget = RootOverlay;

    for (int32 i = 0; i < 3; ++i)
    {
        UTextBlock* Child = WBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
        if (!TestNotNull(TEXT("TextBlock child constructed"), Child))
        {
            WidgetXmlTestHelpers::CleanupXmlTestAsset(PackagePath);
            return false;
        }
        RootOverlay->AddChildToOverlay(Child);
    }

    const FString Xml = WidgetXmlExporter::BuildWidgetTreeXmlWithDiagnostic(
        WBP, /*bIncludeDefaults=*/false, /*bOmitSlotChain=*/true).Xml;

    TestFalse(TEXT("xml not empty"), Xml.IsEmpty());
    TestEqual(TEXT("zero cycle markers"), Xml.Contains(TEXT("_kind=cycle")), false);
    TestEqual(TEXT("zero max_depth markers"), Xml.Contains(TEXT("_kind=max_depth")), false);
    TestFalse(TEXT("no Slot.Parent"), Xml.Contains(TEXT("Slot.Parent")));
    TestFalse(TEXT("no Slot.Content"), Xml.Contains(TEXT("Slot.Content")));
    TestTrue(TEXT("Overlay tag"), Xml.Contains(TEXT("<Overlay")));

    int32 TextBlockCount = 0;
    int32 SearchFrom = 0;
    while (true)
    {
        const int32 Found = Xml.Find(TEXT("<TextBlock"), ESearchCase::CaseSensitive,
            ESearchDir::FromStart, SearchFrom);
        if (Found == INDEX_NONE)
        {
            break;
        }
        ++TextBlockCount;
        SearchFrom = Found + 1;
    }
    TestTrue(TEXT(">=3 TextBlock elements"), TextBlockCount >= 3);

    WidgetXmlTestHelpers::CleanupXmlTestAsset(PackagePath);
    return true;
}
