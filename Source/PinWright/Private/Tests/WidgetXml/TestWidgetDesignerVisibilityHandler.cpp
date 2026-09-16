// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "WidgetXmlTestHelpers.h"

#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/TextBlock.h"
#include "Layout/Visibility.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"


namespace
{
    FString MakeWidgetDesignerVisibilityAssetPath(const FString& Prefix)
    {
        return FString::Printf(TEXT("/Game/_Test/%s_%s"),
            *Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    UWidgetBlueprint* MakeWidgetDesignerVisibilityBlueprint(const FString& PackagePath)
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDesignerVisibilitySetDoesNotChangeRuntimeVisibilityTest,
    "PinWright.widget.designer_visibility.SetDoesNotChangeRuntimeVisibility",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetDesignerVisibilitySetDoesNotChangeRuntimeVisibilityTest::RunTest(const FString& Parameters)
{
    const FString WidgetPath = MakeWidgetDesignerVisibilityAssetPath(TEXT("WBP_DesignerVisibility"));
    ON_SCOPE_EXIT
    {
        WidgetXmlTestHelpers::CleanupXmlTestAsset(WidgetPath);
    };

    UWidgetBlueprint* WBP = MakeWidgetDesignerVisibilityBlueprint(WidgetPath);
    TestNotNull(TEXT("widget blueprint allocated"), WBP);
    if (!WBP || !WBP->WidgetTree)
    {
        return false;
    }

    UCanvasPanel* Root = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(
        UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
    WBP->WidgetTree->RootWidget = Root;

    UTextBlock* Target = WBP->WidgetTree->ConstructWidget<UTextBlock>(
        UTextBlock::StaticClass(), TEXT("DesignerOnlyTarget"));
    Root->AddChild(Target);
    Target->SetVisibility(ESlateVisibility::Collapsed);
    Target->bHiddenInDesigner = false;

    TSharedPtr<FJsonObject> HidePayload = MakeShared<FJsonObject>();
    HidePayload->SetStringField(TEXT("widgetPath"), WidgetPath);
    HidePayload->SetStringField(TEXT("widgetName"), TEXT("DesignerOnlyTarget"));
    HidePayload->SetBoolField(TEXT("visible"), false);

    FTestResponseCapture HideCapture;
    TestTrue(TEXT("widget.set_designer_visibility handler found"),
        InvokeHandlerWithCapture(TEXT("widget.set_designer_visibility"), HidePayload, HideCapture));
    TestTrue(TEXT("widget.set_designer_visibility hide succeeded"), HideCapture.bSuccess);
    TestTrue(TEXT("target hidden in designer"), Target->bHiddenInDesigner);
    TestTrue(TEXT("runtime visibility unchanged after hide"),
        Target->GetVisibility() == ESlateVisibility::Collapsed);
    if (HideCapture.Result.IsValid())
    {
        TestTrue(TEXT("hide response reports runtime visibility unchanged"),
            HideCapture.Result->GetBoolField(TEXT("runtimeVisibilityUnchanged")));
    }

    TSharedPtr<FJsonObject> GetPayload = MakeShared<FJsonObject>();
    GetPayload->SetStringField(TEXT("widgetPath"), WidgetPath);
    GetPayload->SetStringField(TEXT("widgetName"), TEXT("DesignerOnlyTarget"));

    FTestResponseCapture GetCapture;
    TestTrue(TEXT("widget.get_designer_visibility handler found"),
        InvokeHandlerWithCapture(TEXT("widget.get_designer_visibility"), GetPayload, GetCapture));
    TestTrue(TEXT("widget.get_designer_visibility succeeded"), GetCapture.bSuccess);
    if (GetCapture.Result.IsValid())
    {
        TestFalse(TEXT("get reports designer visible false"),
            GetCapture.Result->GetBoolField(TEXT("visible")));
    }

    TSharedPtr<FJsonObject> ShowPayload = MakeShared<FJsonObject>();
    ShowPayload->SetStringField(TEXT("widgetPath"), WidgetPath);
    ShowPayload->SetStringField(TEXT("widgetName"), TEXT("DesignerOnlyTarget"));
    ShowPayload->SetBoolField(TEXT("visible"), true);

    FTestResponseCapture ShowCapture;
    TestTrue(TEXT("widget.set_designer_visibility show handler found"),
        InvokeHandlerWithCapture(TEXT("widget.set_designer_visibility"), ShowPayload, ShowCapture));
    TestTrue(TEXT("widget.set_designer_visibility show succeeded"), ShowCapture.bSuccess);
    TestFalse(TEXT("target shown in designer"), Target->bHiddenInDesigner);
    TestTrue(TEXT("runtime visibility unchanged after show"),
        Target->GetVisibility() == ESlateVisibility::Collapsed);
    if (ShowCapture.Result.IsValid())
    {
        TestTrue(TEXT("show response reports runtime visibility unchanged"),
            ShowCapture.Result->GetBoolField(TEXT("runtimeVisibilityUnchanged")));
    }

    return true;
}
