// Copyright (c) 2026 Alexander Penkin. MIT License.

// widget.export_xml output must be accepted back by the write verbs, unedited:
//  - B-widget-xml-navigation-not-reimportable: the Instanced UWidget::Navigation subobject exports as
//    `{Down={Rule=...,Widget=,CustomDelegate={_kind=FDelegateProperty,...}},_kind=/Script/UMG.WidgetNavigation}`,
//    which widget.import_xml refused (CONSTRUCTION_FAILED, ImportText left trailing input).
//  - B-widget-set-rejects-export-braces: widget.set refused the `{...}` struct text export_xml emits
//    (Brush, ColorAndOpacity, Font) that import_xml accepted.
// Both tests export a source tree, feed the exported text to the verb against a fresh WBP, and compare
// the result with the source widgets.
#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "WidgetXmlTestHelpers.h"
#include "Compat/EngineVersionCompat.h"
#include "Blueprint/WidgetNavigation.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Components/EditableTextBox.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Engine/SCS_Node.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopeExit.h"
#include "UObject/UObjectHash.h"
#include "WidgetBlueprint.h"
#include "XmlFile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace WidgetXmlExportReimportTestHelpers
{
    FString ExportXml(const FString& WidgetPath, FAutomationTestBase& Test)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("widgetPath"), WidgetPath);
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("widget.export_xml"), Payload, Capture);
        Test.TestTrue(*FString::Printf(TEXT("export %s succeeded (%s: %s)"), *WidgetPath,
            *Capture.ErrorCode, *Capture.Message), Capture.bSuccess);
        return Capture.Result.IsValid() ? Capture.Result->GetStringField(TEXT("xml")) : FString();
    }

    const FXmlNode* FindNodeByName(const FXmlNode* Node, const FString& Name)
    {
        if (!Node)
        {
            return nullptr;
        }
        if (Node->GetAttribute(TEXT("name")) == Name)
        {
            return Node;
        }
        for (const FXmlNode* Child : Node->GetChildrenNodes())
        {
            if (const FXmlNode* Found = FindNodeByName(Child, Name))
            {
                return Found;
            }
        }
        return nullptr;
    }

    // The decoded attribute value, exactly as a caller copying it out of the export would see it.
    FString ExportedAttribute(const FString& Xml, const FString& WidgetName, const FString& Attribute)
    {
        FXmlFile File(TEXT("<Root>\n") + Xml + TEXT("\n</Root>"), EConstructMethod::ConstructFromBuffer);
        const FXmlNode* Node = File.IsValid() ? FindNodeByName(File.GetRootNode(), WidgetName) : nullptr;
        return Node ? Node->GetAttribute(Attribute) : FString();
    }

    void RegisterVariables(UWidgetBlueprint* WBP, std::initializer_list<UWidget*> Widgets)
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        for (UWidget* Widget : Widgets)
        {
            WBP->OnVariableAdded(Widget->GetFName());
        }
#else
        (void)WBP;
        (void)Widgets;
#endif
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetXmlNavigationRoundTripTest,
    "PinWright.widget.import_xml.NavigationRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetXmlNavigationRoundTripTest::RunTest(const FString& Parameters)
{
    using namespace WidgetXmlExportReimportTestHelpers;
    const FString SourcePath = WidgetXmlTestHelpers::MakeXmlTestAssetPath(TEXT("WBP_XmlNavSrc"));
    const FString TargetPath = WidgetXmlTestHelpers::MakeXmlTestAssetPath(TEXT("WBP_XmlNavTgt"));

    UWidgetBlueprint* SourceBP = WidgetXmlTestHelpers::MakeOnDiskShapedWidgetBlueprint(SourcePath);
    if (!TestTrue(TEXT("source blueprint allocated"), SourceBP && SourceBP->WidgetTree))
    {
        return false;
    }
    UVerticalBox* Root = SourceBP->WidgetTree->ConstructWidget<UVerticalBox>(
        UVerticalBox::StaticClass(), TEXT("NavRoot"));
    SourceBP->WidgetTree->RootWidget = Root;
    UEditableTextBox* NameBox = SourceBP->WidgetTree->ConstructWidget<UEditableTextBox>(
        UEditableTextBox::StaticClass(), TEXT("NameBox"));
    UEditableTextBox* PassBox = SourceBP->WidgetTree->ConstructWidget<UEditableTextBox>(
        UEditableTextBox::StaticClass(), TEXT("PassBox"));
    Root->AddChild(NameBox);
    Root->AddChild(PassBox);
    RegisterVariables(SourceBP, {Root, NameBox, PassBox});

    // What the UMG designer authors: a UWidgetNavigation subobject with per-direction rules. Widget and
    // CustomDelegate stay unset (runtime-resolved), which is what puts the empty `Widget=` leaf and the
    // unbound delegate marker into the export.
    UWidgetNavigation* SourceNav = NewObject<UWidgetNavigation>(NameBox, NAME_None, RF_Transactional);
    SourceNav->Down.Rule = EUINavigationRule::Explicit;
    SourceNav->Down.WidgetToFocus = TEXT("PassBox");
    SourceNav->Next.Rule = EUINavigationRule::Explicit;
    SourceNav->Next.WidgetToFocus = TEXT("PassBox");
    SourceNav->Up.Rule = EUINavigationRule::Stop;
    NameBox->Navigation = SourceNav;

    const FString SourceXml = ExportXml(SourcePath, *this);
    const FString SourceNavAttr = ExportedAttribute(SourceXml, TEXT("NameBox"), TEXT("Navigation"));
    // Fixture precondition: the export still carries the annotated form the ticket reported, so the
    // import below exercises it rather than some friendlier spelling.
    if (!TestTrue(FString::Printf(TEXT("source export carries the annotated Navigation form: %s"), *SourceNavAttr),
            SourceNavAttr.StartsWith(TEXT("{")) && SourceNavAttr.Contains(TEXT("_kind="))
            && SourceNavAttr.Contains(TEXT("bindingStatus=empty")) && SourceNavAttr.Contains(TEXT("Widget=,"))))
    {
        return false;
    }

    UWidgetBlueprint* TargetBP = WidgetXmlTestHelpers::MakeOnDiskShapedWidgetBlueprint(TargetPath);
    if (!TestTrue(TEXT("target blueprint allocated"), TargetBP && TargetBP->WidgetTree))
    {
        return false;
    }
    TSharedPtr<FJsonObject> ImportPayload = MakeShared<FJsonObject>();
    ImportPayload->SetStringField(TEXT("widgetPath"), TargetPath);
    ImportPayload->SetStringField(TEXT("xml"), SourceXml);
    ImportPayload->SetStringField(TEXT("mode"), TEXT("replace"));
    FTestResponseCapture ImportCapture;
    InvokeHandlerWithCapture(TEXT("widget.import_xml"), ImportPayload, ImportCapture);
    if (!TestTrue(FString::Printf(TEXT("import of the unedited export succeeds (%s: %s)"),
            *ImportCapture.ErrorCode, *ImportCapture.Message), ImportCapture.bSuccess))
    {
        return false;
    }

    UWidget* ImportedNameBox = TargetBP->WidgetTree->FindWidget(TEXT("NameBox"));
    UWidgetNavigation* ImportedNav = ImportedNameBox ? ImportedNameBox->Navigation.Get() : nullptr;
    if (!TestNotNull(TEXT("imported NameBox has a Navigation subobject"), ImportedNav))
    {
        return false;
    }
    TestTrue(TEXT("imported Navigation is a fresh subobject of the imported widget"),
        ImportedNav != SourceNav && ImportedNav->GetOuter() == ImportedNameBox);
    TestTrue(TEXT("Down rule"), ImportedNav->Down.Rule == EUINavigationRule::Explicit);
    TestEqual(TEXT("Down target"), ImportedNav->Down.WidgetToFocus, FName(TEXT("PassBox")));
    TestTrue(TEXT("Next rule"), ImportedNav->Next.Rule == EUINavigationRule::Explicit);
    TestEqual(TEXT("Next target"), ImportedNav->Next.WidgetToFocus, FName(TEXT("PassBox")));
    TestTrue(TEXT("Up rule"), ImportedNav->Up.Rule == EUINavigationRule::Stop);
    TestTrue(TEXT("untouched Left rule keeps the class default"),
        ImportedNav->Left.Rule == SourceNav->Left.Rule);
    TestFalse(TEXT("CustomDelegate stays unbound"), ImportedNav->Down.CustomDelegate.IsBound());

    const FString TargetNavAttr = ExportedAttribute(ExportXml(TargetPath, *this), TEXT("NameBox"), TEXT("Navigation"));
    TestEqual(TEXT("re-export of the imported widget matches the source Navigation"), TargetNavAttr, SourceNavAttr);

    // Refusals: a bad field value and a _kind outside the property class leave Navigation untouched.
    for (const TCHAR* Bad : {TEXT("{Down={Rule=Bogus},_kind=/Script/UMG.WidgetNavigation}"),
                             TEXT("{_kind=/Script/Engine.Actor}")})
    {
        TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
        Properties->SetStringField(TEXT("Navigation"), Bad);
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("widgetPath"), TargetPath);
        Payload->SetStringField(TEXT("widgetName"), TEXT("NameBox"));
        Payload->SetObjectField(TEXT("properties"), Properties);
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("widget.set"), Payload, Capture);
        TestFalse(FString::Printf(TEXT("widget.set Navigation=%s is refused"), Bad), Capture.bSuccess);
        TestTrue(FString::Printf(TEXT("Navigation pointer unchanged after %s"), Bad),
            ImportedNameBox->Navigation == ImportedNav);
    }
    TestTrue(TEXT("Down rule unchanged after the refusals"), ImportedNav->Down.Rule == EUINavigationRule::Explicit);
    return true;
}

// UActorComponent is DefaultToInstanced, so every component pointer is CPF_InstancedReference. The
// Instanced-subobject text path must refuse it rather than build an unregistered component.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertySetInstancedTextRefusesComponentTest,
    "PinWright.property.set.InstancedTextRefusesComponent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPropertySetInstancedTextRefusesComponentTest::RunTest(const FString& Parameters)
{
    USCS_Node* Node = NewObject<USCS_Node>(GetTransientPackage());
    FObjectProperty* TemplateProp = CastField<FObjectProperty>(
        USCS_Node::StaticClass()->FindPropertyByName(TEXT("ComponentTemplate")));
    if (!TestTrue(TEXT("fixture: USCS_Node.ComponentTemplate is an Instanced object property"),
            TemplateProp && TemplateProp->HasAnyPropertyFlags(CPF_InstancedReference)))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), Node->GetPathName());
    Payload->SetStringField(TEXT("propertyName"), TEXT("ComponentTemplate"));
    Payload->SetStringField(TEXT("value"), TEXT("{_kind=/Script/Engine.StaticMeshComponent}"));
    FTestResponseCapture Capture;
    TestTrue(TEXT("property.set handler registered"), InvokeHandlerWithCapture(TEXT("property.set"), Payload, Capture));
    TestFalse(TEXT("brace text on a component property is refused"), Capture.bSuccess);
    TestTrue(FString::Printf(TEXT("refusal names the component rule: %s"), *Capture.Message),
        Capture.Message.Contains(TEXT("Component property")));
    TestNull(TEXT("ComponentTemplate stays null"), Node->ComponentTemplate.Get());
    TArray<UObject*> Built;
    GetObjectsWithOuter(Node, Built, MCP_FOREACH_EXCLUDE_NESTED_OBJECTS);
    TestEqual(TEXT("no component was constructed under the node"), Built.Num(), 0);
    return true;
}

// The importer stamps RF_DefaultSubObject on what it builds under a template. A second Instanced-text
// write to a Blueprint CDO must still move the first imported subobject out from under the CDO; only
// a subobject inherited by name from a parent CDO is kept in place.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertySetInstancedTextCdoReplaceTest,
    "PinWright.property.set.InstancedTextReplacesImportedOnBlueprintCdo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPropertySetInstancedTextCdoReplaceTest::RunTest(const FString& Parameters)
{
    const FString PackagePath = WidgetXmlTestHelpers::MakeXmlTestAssetPath(TEXT("WBP_CdoNav"));
    UPackage* Package = CreatePackage(*PackagePath);
    if (!TestNotNull(TEXT("package created"), Package))
    {
        return false;
    }
    Package->SetFlags(RF_Transient);
    ON_SCOPE_EXIT
    {
        WidgetXmlTestHelpers::CleanupXmlTestAsset(PackagePath);
    };
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(UUserWidget::StaticClass(), Package,
        *FPackageName::GetLongPackageAssetName(PackagePath), BPTYPE_Normal,
        UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass());
    UObject* Cdo = BP && BP->GeneratedClass ? BP->GeneratedClass->GetDefaultObject() : nullptr;
    if (!TestTrue(TEXT("fixture: a Blueprint-generated widget CDO with no Navigation"),
            Cdo && BP->GeneratedClass->HasAnyClassFlags(CLASS_CompiledFromBlueprint)
            && CastChecked<UWidget>(Cdo)->Navigation == nullptr))
    {
        return false;
    }

    UWidget* CdoWidget = CastChecked<UWidget>(Cdo);
    for (const TCHAR* Rule : {TEXT("Stop"), TEXT("Wrap")})
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("objectPath"), Cdo->GetPathName());
        Payload->SetStringField(TEXT("propertyName"), TEXT("Navigation"));
        Payload->SetStringField(TEXT("value"),
            FString::Printf(TEXT("{Down={Rule=%s},_kind=/Script/UMG.WidgetNavigation}"), Rule));
        Payload->SetBoolField(TEXT("markDirty"), false);
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("property.set"), Payload, Capture);
        TestTrue(FString::Printf(TEXT("property.set Navigation Down=%s on the CDO (%s: %s)"), Rule,
            *Capture.ErrorCode, *Capture.Message), Capture.bSuccess);
    }
    UWidgetNavigation* Nav = CdoWidget->Navigation;
    if (!TestNotNull(TEXT("CDO Navigation set"), Nav))
    {
        return false;
    }
    TestTrue(TEXT("second write applied"), Nav->Down.Rule == EUINavigationRule::Wrap);
    TArray<UObject*> Children;
    GetObjectsWithOuter(Cdo, Children, MCP_FOREACH_EXCLUDE_NESTED_OBJECTS);
    int32 NavChildren = 0;
    for (UObject* Child : Children)
    {
        NavChildren += Child->IsA<UWidgetNavigation>() ? 1 : 0;
    }
    TestEqual(TEXT("only the live Navigation stays under the CDO"), NavChildren, 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetSetExportedStructBracesTest,
    "PinWright.widget.set.AcceptsExportedStructBraces",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetSetExportedStructBracesTest::RunTest(const FString& Parameters)
{
    using namespace WidgetXmlExportReimportTestHelpers;
    const FString SourcePath = WidgetXmlTestHelpers::MakeXmlTestAssetPath(TEXT("WBP_SetBraceSrc"));
    const FString TargetPath = WidgetXmlTestHelpers::MakeXmlTestAssetPath(TEXT("WBP_SetBraceTgt"));

    auto BuildTree = [this](const FString& Path, UImage*& OutImage, UTextBlock*& OutText) -> UWidgetBlueprint*
    {
        UWidgetBlueprint* WBP = WidgetXmlTestHelpers::MakeOnDiskShapedWidgetBlueprint(Path);
        if (!TestTrue(TEXT("blueprint allocated"), WBP && WBP->WidgetTree))
        {
            return nullptr;
        }
        UVerticalBox* Root = WBP->WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("BraceRoot"));
        WBP->WidgetTree->RootWidget = Root;
        OutImage = WBP->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("BraceImage"));
        OutText = WBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("BraceText"));
        Root->AddChild(OutImage);
        Root->AddChild(OutText);
        RegisterVariables(WBP, {Root, OutImage, OutText});
        return WBP;
    };

    UImage* SrcImage = nullptr;
    UTextBlock* SrcText = nullptr;
    if (!BuildTree(SourcePath, SrcImage, SrcText))
    {
        return false;
    }
    SrcImage->SetColorAndOpacity(FLinearColor(0.2f, 0.4f, 0.6f, 0.8f));
    SrcImage->SetBrushTintColor(FSlateColor(FLinearColor(0.1f, 0.2f, 0.3f, 1.0f)));
    SrcText->SetColorAndOpacity(FSlateColor(FLinearColor(0.29f, 0.5f, 0.7f, 1.0f)));
    FSlateFontInfo SrcFont = SrcText->GetFont();
    SrcFont.Size = 31;
    SrcText->SetFont(SrcFont);

    const FString SourceXml = ExportXml(SourcePath, *this);
    struct FCase { const TCHAR* Widget; const TCHAR* Property; FString Value; };
    TArray<FCase> Cases = {
        {TEXT("BraceImage"), TEXT("Brush"), FString()},
        {TEXT("BraceImage"), TEXT("ColorAndOpacity"), FString()},
        {TEXT("BraceText"), TEXT("ColorAndOpacity"), FString()},
        {TEXT("BraceText"), TEXT("Font"), FString()},
    };
    for (FCase& Case : Cases)
    {
        Case.Value = ExportedAttribute(SourceXml, Case.Widget, Case.Property);
        // Fixture precondition: the export emits the brace form the ticket reported.
        if (!TestTrue(FString::Printf(TEXT("%s.%s exported in brace form: %s"), Case.Widget, Case.Property, *Case.Value),
                Case.Value.StartsWith(TEXT("{"))))
        {
            return false;
        }
    }

    UImage* TgtImage = nullptr;
    UTextBlock* TgtText = nullptr;
    if (!BuildTree(TargetPath, TgtImage, TgtText))
    {
        return false;
    }
    for (const FCase& Case : Cases)
    {
        TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
        Properties->SetStringField(Case.Property, Case.Value);
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("widgetPath"), TargetPath);
        Payload->SetStringField(TEXT("widgetName"), Case.Widget);
        Payload->SetObjectField(TEXT("properties"), Properties);
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("widget.set"), Payload, Capture);
        TestTrue(FString::Printf(TEXT("widget.set %s.%s from the export text (%s: %s)"),
            Case.Widget, Case.Property, *Capture.ErrorCode, *Capture.Message), Capture.bSuccess);
    }

    TestTrue(TEXT("Image.ColorAndOpacity matches the source"),
        TgtImage->GetColorAndOpacity().Equals(SrcImage->GetColorAndOpacity(), 0.001f));
    TestTrue(TEXT("Image.Brush tint matches the source"),
        TgtImage->GetBrush().TintColor.GetSpecifiedColor().Equals(
            SrcImage->GetBrush().TintColor.GetSpecifiedColor(), 0.001f));
    TestTrue(TEXT("TextBlock.ColorAndOpacity matches the source"),
        TgtText->GetColorAndOpacity().GetSpecifiedColor().Equals(
            SrcText->GetColorAndOpacity().GetSpecifiedColor(), 0.001f));
    TestEqual(TEXT("TextBlock.Font size matches the source"), TgtText->GetFont().Size, SrcFont.Size);
    TestTrue(TEXT("TextBlock.Font object matches the source"),
        TgtText->GetFont().FontObject == SrcFont.FontObject);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
