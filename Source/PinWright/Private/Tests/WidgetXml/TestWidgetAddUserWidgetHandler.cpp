// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "WidgetXmlTestHelpers.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/EngineVersionComparison.h"
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
        return FString::Printf(TEXT("/Game/PinWrightTests/_Test/%s_%s"),
            *Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // A real UUserWidget-parented widget blueprint, as the editor's factory makes one.
    UWidgetBlueprint* MakeUserWidgetBlueprint(const FString& PackagePath)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            return nullptr;
        }
        Package->SetFlags(RF_Transient);

        return Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
            UUserWidget::StaticClass(),
            Package,
            FName(*AssetName),
            BPTYPE_Normal,
            UWidgetBlueprint::StaticClass(),
            UWidgetBlueprintGeneratedClass::StaticClass()));
    }

    UWidgetBlueprint* MakeCompiledRowWidgetBlueprint(const FString& PackagePath)
    {
        UWidgetBlueprint* RowBP = MakeUserWidgetBlueprint(PackagePath);
        if (!RowBP || !RowBP->WidgetTree)
        {
            return nullptr;
        }

        UCanvasPanel* Root = RowBP->WidgetTree->ConstructWidget<UCanvasPanel>(
            UCanvasPanel::StaticClass(), TEXT("RowRoot"));
        // A variable, so the generated class carries a RowRoot property that Initialize() binds.
        Root->bIsVariable = true;
        RowBP->WidgetTree->RootWidget = Root;
        FKismetEditorUtilities::CompileBlueprint(RowBP);
        return RowBP;
    }

    // The value of the row class's RowRoot widget-variable property on a placed row template.
    // Initialize() binds it into the instance's own runtime tree; a template must not carry it,
    // or the parent asset saves a stale copy of the row's layout.
    UObject* GetRowRootBinding(UUserWidget* Row)
    {
        FObjectPropertyBase* Prop = CastField<FObjectPropertyBase>(
            Row->GetClass()->FindPropertyByName(TEXT("RowRoot")));
        return Prop ? Prop->GetObjectPropertyValue_InContainer(Row) : nullptr;
    }

    struct FRowPlacementFixture
    {
        FString TargetPath = MakeWidgetAddUserWidgetAssetPath(TEXT("WBP_RowVarsTarget"));
        FString RowPath = MakeWidgetAddUserWidgetAssetPath(TEXT("WBP_RowVarsRow"));
        UWidgetBlueprint* TargetBP = nullptr;
        UCanvasPanel* RootCanvas = nullptr;
        UWidgetBlueprint* RowBP = nullptr;

        bool Build(FAutomationTestBase& Test)
        {
            // Compiled by the add test, so it needs a real parent class (not the stub helper).
            TargetBP = MakeUserWidgetBlueprint(TargetPath);
            RowBP = MakeCompiledRowWidgetBlueprint(RowPath);
            if (!Test.TestNotNull(TEXT("target widget blueprint allocated"), TargetBP) || !TargetBP->WidgetTree
                || !Test.TestNotNull(TEXT("row widget blueprint allocated"), RowBP) || !RowBP->GeneratedClass)
            {
                return false;
            }

            // A freshly created widget blueprint's tree is RF_ArchetypeObject, so a UserWidget placed
            // in it is a template that Initialize() never binds. UWidgetBlueprint::PostLoad clears
            // the flag, so every asset loaded from disk binds its placed UserWidgets: match that.
            TargetBP->WidgetTree->ClearFlags(RF_ArchetypeObject);

            RootCanvas = TargetBP->WidgetTree->ConstructWidget<UCanvasPanel>(
                UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
            TargetBP->WidgetTree->RootWidget = RootCanvas;
            RegisterGuid(RootCanvas);

            const UWidgetBlueprintGeneratedClass* RowClass = Cast<UWidgetBlueprintGeneratedClass>(RowBP->GeneratedClass);
            const UWidgetTree* RowArchetype = RowClass ? RowClass->GetWidgetTreeArchetype() : nullptr;
            return Test.TestNotNull(TEXT("row class has a RowRoot widget property"),
                    RowBP->GeneratedClass->FindPropertyByName(TEXT("RowRoot")))
                && Test.TestNotNull(TEXT("row class widget tree holds RowRoot"),
                    RowArchetype ? RowArchetype->FindWidget(FName(TEXT("RowRoot"))) : nullptr);
        }

        // ConstructWidget skips the editor's GUID registration; once a verb registers its widget
        // the compiler's GUID validation ensures on any unregistered one.
        void RegisterGuid(UWidget* Widget) const
        {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
            TargetBP->OnVariableAdded(Widget->GetFName());
#endif
        }

        ~FRowPlacementFixture()
        {
            // Target first: it owns the nested instance of the row's generated class.
            CleanupXmlTestAsset(TargetPath);
            CleanupXmlTestAsset(RowPath);
        }
    };

    // Asserts the placed row is a real initialized instance (its own tree holds RowRoot) whose
    // template does not reference that tree.
    void TestRowTemplateOmitsChildVars(FAutomationTestBase& Test, UWidgetBlueprint* TargetBP, const TCHAR* Name)
    {
        UUserWidget* Row = Cast<UUserWidget>(TargetBP->WidgetTree->FindWidget(FName(Name)));
        if (!Test.TestNotNull(TEXT("placed row is a UUserWidget"), Row))
        {
            return;
        }
        Test.TestNotNull(TEXT("placed row's own tree holds RowRoot"),
            Row->WidgetTree ? Row->WidgetTree->FindWidget(FName(TEXT("RowRoot"))) : nullptr);
        Test.TestNull(TEXT("placed row template carries no RowRoot binding"), GetRowRootBinding(Row));
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetAddUserWidgetOmitsChildVarsTest,
    "PinWright.widget.add.UserWidgetOmitsChildWidgetVars",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetAddUserWidgetOmitsChildVarsTest::RunTest(const FString& Parameters)
{
    FRowPlacementFixture Fixture;
    if (!Fixture.Build(*this))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), Fixture.TargetPath);
    Payload->SetStringField(TEXT("type"), Fixture.RowBP->GeneratedClass->GetName());
    Payload->SetStringField(TEXT("name"), TEXT("AddedRow"));
    Payload->SetStringField(TEXT("parentName"), TEXT("RootCanvas"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.add handler found"), InvokeHandlerWithCapture(TEXT("widget.add"), Payload, Capture));
    TestTrue(TEXT("widget.add succeeded"), Capture.bSuccess);
    TestRowTemplateOmitsChildVars(*this, Fixture.TargetBP, TEXT("AddedRow"));

    FKismetEditorUtilities::CompileBlueprint(Fixture.TargetBP);
    TestRowTemplateOmitsChildVars(*this, Fixture.TargetBP, TEXT("AddedRow"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDuplicateUserWidgetOmitsChildVarsTest,
    "PinWright.widget.duplicate.UserWidgetOmitsChildWidgetVars",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetDuplicateUserWidgetOmitsChildVarsTest::RunTest(const FString& Parameters)
{
    FRowPlacementFixture Fixture;
    if (!Fixture.Build(*this))
    {
        return false;
    }

    // Source placed through the engine's own construct path: Initialize() bound its RowRoot,
    // so a property copy would hand the duplicate a reference into the source's tree.
    UUserWidget* Source = Fixture.TargetBP->WidgetTree->ConstructWidget<UUserWidget>(
        Fixture.RowBP->GeneratedClass.Get(), TEXT("SourceRow"));
    if (!TestNotNull(TEXT("source row constructed"), Source))
    {
        return false;
    }
    Fixture.RootCanvas->AddChild(Source);
    Fixture.RegisterGuid(Source);
    TestNotNull(TEXT("engine-constructed source binds RowRoot (fixture precondition)"), GetRowRootBinding(Source));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), Fixture.TargetPath);
    Payload->SetStringField(TEXT("sourceName"), TEXT("SourceRow"));
    Payload->SetStringField(TEXT("name"), TEXT("SourceRow_Copy"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.duplicate handler found"), InvokeHandlerWithCapture(TEXT("widget.duplicate"), Payload, Capture));
    TestTrue(TEXT("widget.duplicate succeeded"), Capture.bSuccess);
    TestRowTemplateOmitsChildVars(*this, Fixture.TargetBP, TEXT("SourceRow_Copy"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetReplaceClassUserWidgetOmitsChildVarsTest,
    "PinWright.widget.replace_class.UserWidgetOmitsChildWidgetVars",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetReplaceClassUserWidgetOmitsChildVarsTest::RunTest(const FString& Parameters)
{
    FRowPlacementFixture Fixture;
    if (!Fixture.Build(*this))
    {
        return false;
    }

    UCanvasPanel* SwapMe = Fixture.TargetBP->WidgetTree->ConstructWidget<UCanvasPanel>(
        UCanvasPanel::StaticClass(), TEXT("SwapMe"));
    Fixture.RootCanvas->AddChild(SwapMe);
    Fixture.RegisterGuid(SwapMe);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), Fixture.TargetPath);
    Payload->SetStringField(TEXT("targetName"), TEXT("SwapMe"));
    Payload->SetStringField(TEXT("newType"), Fixture.RowBP->GeneratedClass->GetName());

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.replace_class handler found"), InvokeHandlerWithCapture(TEXT("widget.replace_class"), Payload, Capture));
    TestTrue(TEXT("widget.replace_class succeeded"), Capture.bSuccess);
    TestRowTemplateOmitsChildVars(*this, Fixture.TargetBP, TEXT("SwapMe"));
    return true;
}
