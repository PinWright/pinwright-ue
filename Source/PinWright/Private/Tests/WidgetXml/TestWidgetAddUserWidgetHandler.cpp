// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "WidgetXmlTestHelpers.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"
#include "Widgets/SWidget.h"


using WidgetXmlTestHelpers::CleanupXmlTestAsset;
using WidgetXmlTestHelpers::MakeOnDiskShapedWidgetBlueprint;

namespace
{
    FString MakeWidgetAddUserWidgetAssetPath(const FString& Prefix)
    {
        return FString::Printf(TEXT("/Game/_Test/%s_%s"),
            *Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    UWidgetBlueprint* MakeCompiledRowWidgetBlueprint(const FString& PackagePath)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            return nullptr;
        }
        Package->SetFlags(RF_Transient);

        UWidgetBlueprint* RowBP = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
            UUserWidget::StaticClass(),
            Package,
            FName(*AssetName),
            BPTYPE_Normal,
            UWidgetBlueprint::StaticClass(),
            UWidgetBlueprintGeneratedClass::StaticClass()));
        if (!RowBP || !RowBP->WidgetTree)
        {
            return nullptr;
        }

        UCanvasPanel* Root = RowBP->WidgetTree->ConstructWidget<UCanvasPanel>(
            UCanvasPanel::StaticClass(), TEXT("RowRoot"));
        RowBP->WidgetTree->RootWidget = Root;
        FKismetEditorUtilities::CompileBlueprint(RowBP);
        return RowBP;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetAddUserWidgetInitializesNestedWidgetTest,
    "PinWright.widget.add.UserWidgetInitializesNestedWidget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetAddUserWidgetInitializesNestedWidgetTest::RunTest(const FString& Parameters)
{
    const FString TargetPath = MakeWidgetAddUserWidgetAssetPath(TEXT("WBP_UserWidgetAddTarget"));
    const FString RowPath = MakeWidgetAddUserWidgetAssetPath(TEXT("WBP_UserWidgetAddRow"));
    ON_SCOPE_EXIT
    {
        // Target first: it owns the nested instance of the row's generated class.
        CleanupXmlTestAsset(TargetPath);
        CleanupXmlTestAsset(RowPath);
    };

    UWidgetBlueprint* TargetBP = MakeOnDiskShapedWidgetBlueprint(TargetPath);
    TestNotNull(TEXT("target widget blueprint allocated"), TargetBP);
    if (!TargetBP || !TargetBP->WidgetTree)
    {
        return false;
    }

    UCanvasPanel* RootCanvas = TargetBP->WidgetTree->ConstructWidget<UCanvasPanel>(
        UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
    TargetBP->WidgetTree->RootWidget = RootCanvas;

    UWidgetBlueprint* RowBP = MakeCompiledRowWidgetBlueprint(RowPath);
    TestNotNull(TEXT("row widget blueprint allocated"), RowBP);
    if (!RowBP || !RowBP->GeneratedClass)
    {
        return false;
    }

    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("widgetPath"), TargetPath);
    AddPayload->SetStringField(TEXT("type"), RowBP->GeneratedClass->GetName());
    AddPayload->SetStringField(TEXT("name"), TEXT("NestedRow"));
    AddPayload->SetStringField(TEXT("parentName"), TEXT("RootCanvas"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.add handler found"),
        InvokeHandlerWithCapture(TEXT("widget.add"), AddPayload, Capture));
    TestTrue(TEXT("widget.add succeeded"), Capture.bSuccess);

    UWidget* AddedWidget = TargetBP->WidgetTree->FindWidget(FName(TEXT("NestedRow")));
    TestNotNull(TEXT("nested row widget exists"), AddedWidget);

    UUserWidget* AddedUserWidget = Cast<UUserWidget>(AddedWidget);
    TestNotNull(TEXT("nested row is a UUserWidget"), AddedUserWidget);
    if (!AddedUserWidget)
    {
        return false;
    }

    if (!TestNotNull(TEXT("nested row WidgetTree is initialized"), AddedUserWidget->WidgetTree.Get()))
    {
        return false;
    }

    TSharedRef<SWidget> SlateWidget = AddedUserWidget->TakeWidget();
    TestTrue(TEXT("nested row returns a valid Slate widget"), SlateWidget.ToSharedPtr().IsValid());

    return true;
}
