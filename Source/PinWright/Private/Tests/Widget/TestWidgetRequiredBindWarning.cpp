// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for the required-BindWidget detection behind the widget.remove_widget /
// widget.rename_widget advisory (ticket E-remove-widget-required-bindwidget-no-warning).
//
// widget.remove_widget and widget.rename_widget both feed the names they are about to
// invalidate into WidgetAuthoringHelpers::CollectBrokenRequiredBinds — the single production
// helper that walks the widget blueprint's parent-class chain for REQUIRED meta=(BindWidget)
// properties and reports any whose variable name matches. Those matches become the handlers'
// removedRequiredBinds / renamedRequiredBinds advisory (plus a warning), so an agent learns at
// mutation time that the next blueprint.compile would fail with
// "A required widget binding ... was not found." instead of discovering it a compile later.
//
// This test drives that production helper directly against a widget blueprint parented to the
// reflected fixture UTestWidgetWithRequiredBind (one required meta=(BindWidget) property, one
// meta=(BindWidgetOptional)). It asserts the required bind is reported by name + widget type,
// while an optional bind and an unbound name are not. Detection reads only ParentClass metadata
// and the affected-name set, so no widget tree, compile, or asset registration is needed —
// keeping the test deterministic. If the fix is reverted the helper is gone and this fails to
// compile; if the required/optional filter regresses the assertions fail.

#include "Misc/AutomationTest.h"
#include "Tests/Widget/TestWidgetRequiredBindFixture.h"

#include "Handlers/UI/WidgetAuthoringUtils.h"
#include "Blueprint/UserWidget.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"

namespace
{
    UWidgetBlueprint* MakeTransientWidgetBlueprintWithParent(UClass* ParentClass)
    {
        UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
            GetTransientPackage(), UWidgetBlueprint::StaticClass(), NAME_None, RF_Transient);
        if (WBP)
        {
            WBP->ParentClass = ParentClass;
        }
        return WBP;
    }

    bool FindBrokenBindType(const TArray<TPair<FName, FString>>& Broken, const TCHAR* Name, FString& OutType)
    {
        for (const TPair<FName, FString>& Entry : Broken)
        {
            if (Entry.Key == FName(Name))
            {
                OutType = Entry.Value;
                return true;
            }
        }
        return false;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetRequiredBindDetectionTest,
    "PinWright.Widget.RequiredBindWarning.DetectsBrokenRequiredBind",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetRequiredBindDetectionTest::RunTest(const FString& Parameters)
{
    UWidgetBlueprint* WBP = MakeTransientWidgetBlueprintWithParent(UTestWidgetWithRequiredBind::StaticClass());
    TestNotNull(TEXT("fixture widget blueprint allocated"), WBP);
    if (!WBP)
    {
        return false;
    }

    // Invalidating the REQUIRED bind's variable name is reported, by name and widget type.
    {
        TArray<TPair<FName, FString>> Broken;
        WidgetAuthoringHelpers::CollectBrokenRequiredBinds(
            WBP, TSet<FName>{ FName(TEXT("RequiredBindText")) }, Broken);
        TestEqual(TEXT("required bind produces exactly one match"), Broken.Num(), 1);
        FString Type;
        TestTrue(TEXT("required bind is reported by name"),
            FindBrokenBindType(Broken, TEXT("RequiredBindText"), Type));
        TestEqual(TEXT("required bind reports the bound widget type"), Type, FString(TEXT("TextBlock")));
    }

    // An OPTIONAL bind (meta=(BindWidgetOptional)) never fails compile, so it is excluded.
    {
        TArray<TPair<FName, FString>> Broken;
        WidgetAuthoringHelpers::CollectBrokenRequiredBinds(
            WBP, TSet<FName>{ FName(TEXT("OptionalBindText")) }, Broken);
        TestEqual(TEXT("optional bind is not flagged"), Broken.Num(), 0);
    }

    // A name that matches no BindWidget property is not flagged.
    {
        TArray<TPair<FName, FString>> Broken;
        WidgetAuthoringHelpers::CollectBrokenRequiredBinds(
            WBP, TSet<FName>{ FName(TEXT("PlainLabel")) }, Broken);
        TestEqual(TEXT("non-bind name is not flagged"), Broken.Num(), 0);
    }

    // A mixed affected set flags only the required bind, not the optional one or the unbound name.
    {
        TArray<TPair<FName, FString>> Broken;
        WidgetAuthoringHelpers::CollectBrokenRequiredBinds(
            WBP, TSet<FName>{ FName(TEXT("RequiredBindText")), FName(TEXT("OptionalBindText")), FName(TEXT("PlainLabel")) },
            Broken);
        TestEqual(TEXT("mixed set flags only the required bind"), Broken.Num(), 1);
        FString Type;
        TestTrue(TEXT("required bind present in mixed result"),
            FindBrokenBindType(Broken, TEXT("RequiredBindText"), Type));
    }

    // Empty affected set yields nothing (the guard the handlers rely on when nothing was removed).
    {
        TArray<TPair<FName, FString>> Broken;
        WidgetAuthoringHelpers::CollectBrokenRequiredBinds(WBP, TSet<FName>{}, Broken);
        TestEqual(TEXT("empty affected set yields nothing"), Broken.Num(), 0);
    }

    // A parent class with no BindWidget properties yields nothing even for a matching name.
    {
        UWidgetBlueprint* PlainWBP = MakeTransientWidgetBlueprintWithParent(UUserWidget::StaticClass());
        TestNotNull(TEXT("plain-parent widget blueprint allocated"), PlainWBP);
        if (PlainWBP)
        {
            TArray<TPair<FName, FString>> Broken;
            WidgetAuthoringHelpers::CollectBrokenRequiredBinds(
                PlainWBP, TSet<FName>{ FName(TEXT("RequiredBindText")) }, Broken);
            TestEqual(TEXT("parent class without binds yields nothing"), Broken.Num(), 0);
        }
    }

    return true;
}
