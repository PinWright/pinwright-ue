// Copyright (c) 2026 Alexander Penkin. MIT License.

// Where widget.import_xml puts what it builds.
//
// B-import-xml-add-drops-sibling-roots: FXmlFile parses only the first top-level element,
// so `add` with two sibling elements built the first and dropped the second while still
// reporting success. B-import-xml-replace-moves-subtree-to-end: a targeted replace removed
// the target and re-attached the rebuilt subtree with AddChild, so it became the LAST child
// of its parent, which reorders any box layout.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "WidgetXmlTestHelpers.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/HorizontalBox.h"
#include "Components/Image.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace WidgetXmlImportPlacementTests
{
    FString ChildNames(const UPanelWidget* Panel)
    {
        TArray<FString> Names;
        for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
        {
            Names.Add(Panel->GetChildAt(i)->GetName());
        }
        return FString::Join(Names, TEXT(","));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FXmlImportAddImportsEveryTopLevelSiblingTest,
    "PinWright.widget.import_xml.AddImportsEveryTopLevelSibling",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FXmlImportAddImportsEveryTopLevelSiblingTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = WidgetXmlTestHelpers::MakeXmlTestAssetPath(TEXT("WBP_XmlAddSiblings"));
    UWidgetBlueprint* WBP = WidgetXmlTestHelpers::MakeOnDiskShapedWidgetBlueprint(AssetPath);
    if (!TestNotNull(TEXT("blueprint allocated"), WBP) || !WBP->WidgetTree) return false;

    UHorizontalBox* Row = WBP->WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("Row"));
    WBP->WidgetTree->RootWidget = Row;
    Row->AddChild(WBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Existing")));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), AssetPath);
    Payload->SetStringField(TEXT("mode"), TEXT("add"));
    Payload->SetStringField(TEXT("targetName"), TEXT("Row"));
    Payload->SetStringField(TEXT("xml"), TEXT("<Image name=\"SiblingA\" />\n<TextBlock name=\"SiblingB\" />"));

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("widget.import_xml"), Payload, Capture);
    if (!TestTrue(TEXT("add with two sibling elements succeeded"), Capture.bSuccess))
    {
        AddError(Capture.ErrorCode + TEXT(": ") + Capture.Message);
        return false;
    }

    TestEqual(TEXT("both siblings appended after the existing child, in document order"),
        WidgetXmlImportPlacementTests::ChildNames(Row), FString(TEXT("Existing,SiblingA,SiblingB")));
    TestEqual(TEXT("widget_count counts both siblings"),
        static_cast<int32>(Capture.Result->GetNumberField(TEXT("widget_count"))), 2);

    TArray<FString> RootWidgets;
    Capture.Result->TryGetStringArrayField(TEXT("rootWidgets"), RootWidgets);
    TestEqual(TEXT("rootWidgets names every top-level element"),
        FString::Join(RootWidgets, TEXT(",")), FString(TEXT("SiblingA,SiblingB")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FXmlImportReplaceRejectsMultipleTopLevelElementsTest,
    "PinWright.widget.import_xml.ReplaceRejectsMultipleTopLevelElements",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FXmlImportReplaceRejectsMultipleTopLevelElementsTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = WidgetXmlTestHelpers::MakeXmlTestAssetPath(TEXT("WBP_XmlReplaceSiblings"));
    UWidgetBlueprint* WBP = WidgetXmlTestHelpers::MakeOnDiskShapedWidgetBlueprint(AssetPath);
    if (!TestNotNull(TEXT("blueprint allocated"), WBP) || !WBP->WidgetTree) return false;

    UVerticalBox* Column = WBP->WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Column"));
    WBP->WidgetTree->RootWidget = Column;
    UTextBlock* Target = WBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Target"));
    Column->AddChild(Target);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), AssetPath);
    Payload->SetStringField(TEXT("targetName"), TEXT("Target"));
    Payload->SetStringField(TEXT("xml"), TEXT("<TextBlock name=\"Target\" /><TextBlock name=\"Extra\" />"));

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("widget.import_xml"), Payload, Capture);
    TestFalse(TEXT("replace with two top-level elements is refused"), Capture.bSuccess);
    TestEqual(TEXT("error code is INVALID_ARGUMENT"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    TestTrue(TEXT("the tree is untouched"), Column->GetChildrenCount() == 1 && Column->GetChildAt(0) == Target);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FXmlImportReplaceTargetKeepsChildIndexTest,
    "PinWright.widget.import_xml.ReplaceTargetKeepsChildIndex",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FXmlImportReplaceTargetKeepsChildIndexTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = WidgetXmlTestHelpers::MakeXmlTestAssetPath(TEXT("WBP_XmlReplaceIndex"));
    UWidgetBlueprint* WBP = WidgetXmlTestHelpers::MakeOnDiskShapedWidgetBlueprint(AssetPath);
    if (!TestNotNull(TEXT("blueprint allocated"), WBP) || !WBP->WidgetTree) return false;

    UVerticalBox* Column = WBP->WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Column"));
    WBP->WidgetTree->RootWidget = Column;
    Column->AddChild(WBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("First")));
    Column->AddChild(WBP->WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("Middle")));
    Column->AddChild(WBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Last")));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), AssetPath);
    Payload->SetStringField(TEXT("mode"), TEXT("replace"));
    Payload->SetStringField(TEXT("targetName"), TEXT("Middle"));
    Payload->SetStringField(TEXT("xml"), TEXT("<SizeBox name=\"Middle\"><TextBlock name=\"MiddleLabel\" /></SizeBox>"));

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("widget.import_xml"), Payload, Capture);
    if (!TestTrue(TEXT("targeted replace succeeded"), Capture.bSuccess))
    {
        AddError(Capture.ErrorCode + TEXT(": ") + Capture.Message);
        return false;
    }

    TestEqual(TEXT("rebuilt subtree keeps the replaced widget's child index"),
        WidgetXmlImportPlacementTests::ChildNames(Column), FString(TEXT("First,Middle,Last")));
    // No pointer comparison: re-constructing "Middle" under the same outer can reuse the old
    // object's address. The imported child is what proves index 1 holds the rebuilt subtree.
    const USizeBox* NewMiddle = Cast<USizeBox>(Column->GetChildAt(1));
    TestTrue(TEXT("index 1 holds the rebuilt SizeBox with the imported child"),
        NewMiddle && NewMiddle->GetChildrenCount() == 1 && NewMiddle->GetChildAt(0)->GetName() == TEXT("MiddleLabel"));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
