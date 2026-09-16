// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for widget XML export/import handlers (WidgetXmlExportHandler, WidgetXmlImportHandler)
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "WidgetXmlTestHelpers.h"
#include "Handlers/UI/WidgetXmlUtils.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "Components/CanvasPanel.h"
#include "Components/Image.h"
#include "Components/PanelSlot.h"
#include "Components/SizeBox.h"
#include "Components/VerticalBox.h"
#include "Components/Button.h"
#include "Engine/Texture2D.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/UserWidget.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "Compat/EngineVersionCompat.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Styling/SlateBrush.h"
#include "Handlers/UI/WidgetXmlExporter.h"
#include "XmlFile.h"

// ============================================================================
// 1. Handler Registration Tests
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FXmlExportRegisteredTest,
    "PinWright.widget.export_xml.Registered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FXmlExportRegisteredTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("widget.export_xml is registered"), IsHandlerRegistered(TEXT("widget.export_xml")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FXmlImportRegisteredTest,
    "PinWright.widget.import_xml.Registered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FXmlImportRegisteredTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("widget.import_xml is registered"), IsHandlerRegistered(TEXT("widget.import_xml")));
    return true;
}

// ============================================================================
// 2. Missing Param Tests (no-crash)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FXmlExportEmptyParamsNoCrashTest,
    "PinWright.widget.export_xml.EmptyParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FXmlExportEmptyParamsNoCrashTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    bool bFound = InvokeHandlerWithCapture(TEXT("widget.export_xml"), MakeShared<FJsonObject>(), Capture);
    TestTrue(TEXT("handler found"), bFound);
    TestFalse(TEXT("should fail with empty params"), Capture.bSuccess);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FXmlImportEmptyParamsNoCrashTest,
    "PinWright.widget.import_xml.EmptyParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FXmlImportEmptyParamsNoCrashTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    bool bFound = InvokeHandlerWithCapture(TEXT("widget.import_xml"), MakeShared<FJsonObject>(), Capture);
    TestTrue(TEXT("handler found"), bFound);
    TestFalse(TEXT("should fail with empty params"), Capture.bSuccess);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FXmlImportMissingXmlParamTest,
    "PinWright.widget.import_xml.MissingXmlParam",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FXmlImportMissingXmlParamTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), TEXT("/Game/UI/TestWidget"));

    FTestResponseCapture Capture;
    bool bFound = InvokeHandlerWithCapture(TEXT("widget.import_xml"), Payload, Capture);
    TestTrue(TEXT("handler found"), bFound);
    TestFalse(TEXT("should fail without xml param"), Capture.bSuccess);
    return true;
}

// ============================================================================
// 3. Export Tests — error cases
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FXmlExportNonExistentWidgetPathTest,
    "PinWright.widget.export_xml.NonExistentWidgetPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FXmlExportNonExistentWidgetPathTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), TEXT("/Game/_Test/WBP_DoesNotExist_XmlExport"));

    FTestResponseCapture Capture;
    bool bFound = InvokeHandlerWithCapture(TEXT("widget.export_xml"), Payload, Capture);
    TestTrue(TEXT("handler found"), bFound);
    TestFalse(TEXT("should fail for non-existent asset"), Capture.bSuccess);
    TestEqual(TEXT("error code is NOT_FOUND"), Capture.ErrorCode, TEXT("NOT_FOUND"));
    return true;
}

// ============================================================================
// 4. Import Tests — error cases
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FXmlImportInvalidXmlTest,
    "PinWright.widget.import_xml.InvalidXml",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FXmlImportInvalidXmlTest::RunTest(const FString& Parameters)
{
    // The widget blueprint must EXIST. This test used to point at a nonexistent path and
    // accept `NOT_FOUND || INVALID_XML`; WidgetXmlImportHandler.cpp loads the asset (line
    // 593, NOT_FOUND) strictly before it parses the XML (line 601, INVALID_XML), so the
    // answer was always NOT_FOUND and the INVALID_XML half was dead — deleting the
    // FXmlFile::IsValid() guard entirely left this test green. A real target reaches the
    // parser, so the error code can be pinned exactly.
    const FString AssetPath = WidgetXmlTestHelpers::MakeXmlTestAssetPath(TEXT("WBP_XmlInvalidXml"));
    UWidgetBlueprint* WBP = WidgetXmlTestHelpers::MakeOnDiskShapedWidgetBlueprint(AssetPath);
    TestNotNull(TEXT("blueprint allocated"), WBP);
    if (!WBP || !WBP->WidgetTree) return false;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), AssetPath);
    Payload->SetStringField(TEXT("xml"), TEXT("<<<not valid xml>>>"));

    FTestResponseCapture Capture;
    bool bFound = InvokeHandlerWithCapture(TEXT("widget.import_xml"), Payload, Capture);
    TestTrue(TEXT("handler found"), bFound);
    TestFalse(TEXT("should fail with invalid xml"), Capture.bSuccess);
    TestEqual(TEXT("error code is INVALID_XML"), Capture.ErrorCode, FString(TEXT("INVALID_XML")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FXmlImportAddModeNoTargetNameTest,
    "PinWright.widget.import_xml.AddModeNoTargetName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FXmlImportAddModeNoTargetNameTest::RunTest(const FString& Parameters)
{
    // Same dead-branch problem as FXmlImportInvalidXmlTest above: the asset-load NOT_FOUND
    // at WidgetXmlImportHandler.cpp:593 always fired first, so the add-mode targetName
    // guard (line 639, MISSING_PARAMETER) — the thing this test is named for — was never
    // reached and could be deleted without failing anything. A real target reaches it.
    const FString AssetPath = WidgetXmlTestHelpers::MakeXmlTestAssetPath(TEXT("WBP_XmlAddNoTarget"));
    UWidgetBlueprint* WBP = WidgetXmlTestHelpers::MakeOnDiskShapedWidgetBlueprint(AssetPath);
    TestNotNull(TEXT("blueprint allocated"), WBP);
    if (!WBP || !WBP->WidgetTree) return false;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), AssetPath);
    Payload->SetStringField(TEXT("xml"), TEXT("<VerticalBox name=\"Root\" />"));
    Payload->SetStringField(TEXT("mode"), TEXT("add"));

    FTestResponseCapture Capture;
    bool bFound = InvokeHandlerWithCapture(TEXT("widget.import_xml"), Payload, Capture);
    TestTrue(TEXT("handler found"), bFound);
    TestFalse(TEXT("should fail without targetName in add mode"), Capture.bSuccess);
    TestEqual(TEXT("error code is MISSING_PARAMETER"), Capture.ErrorCode, FString(TEXT("MISSING_PARAMETER")));
    return true;
}

// ============================================================================
// 5a. SanitizeXmlName unit test
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSanitizeXmlAttributeNameUnitTest,
    "PinWright.widget.export_xml.SanitizeXmlNameUnit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSanitizeXmlAttributeNameUnitTest::RunTest(const FString& Parameters)
{
    using namespace WidgetXmlHelpers;

    TestEqual(TEXT("space replaced by underscore"),
        SanitizeXmlName(TEXT("Axis Type")), FString(TEXT("Axis_Type")));

    TestEqual(TEXT("already-safe name unchanged"),
        SanitizeXmlName(TEXT("Already_Safe")), FString(TEXT("Already_Safe")));

    TestEqual(TEXT("dot and underscore preserved, space replaced"),
        SanitizeXmlName(TEXT("Bind.Show Seconds")), FString(TEXT("Bind.Show_Seconds")));

    TestEqual(TEXT("empty string unchanged"),
        SanitizeXmlName(TEXT("")), FString(TEXT("")));

    TestEqual(TEXT("dash and dot are valid XML name chars"),
        SanitizeXmlName(TEXT("my-attr.val")), FString(TEXT("my-attr.val")));

    return true;
}

// ============================================================================
// 5b. SanitizesAttributeNameSpaces integration tests
//
// The exporter has TWO sites that emit attribute keys derived from runtime
// data and therefore must run through SanitizeXmlName:
//   (a) WidgetXmlExporter.cpp lines 56-58 — property-reflection path: keys
//       come from FProperty::GetName(). FNames technically can contain any
//       character (no enforcement in FName ctor), and FBlueprintEditorUtils::
//       AddMemberVariable does not reject space-containing names — it only
//       rejects NAME_None and duplicates. Compiled BP variables with legacy
//       display-name-as-FName can therefore deliver space-containing keys.
//       This is rare in normal authoring but possible from older assets,
//       hand-edited .uassets, or programmatic BP construction.
//   (b) WidgetXmlExporter.cpp line 230 — bindings path: keys come from
//       FString::Printf(TEXT("Bind.%s"), *Binding.Key) where Binding.Key
//       maps to FBPVariableMetaDataEntry / FDelegateEditorBinding property
//       names which routinely contain spaces in shipped Lyra/UMG content.
//       This is the dominant production producer of space-containing keys.
//
// FXmlExportSanitizesAttributeNameSpacesTest below covers (b) via a direct
// BindingsMap injection — fast, deterministic, no BP compile.
//
// FXmlExportPropertyReflectionPathSanitizationTest below covers (a) by
// compiling a transient BP whose member variable FName contains a space.
// If the kismet compiler's property creation pipeline preserves the space
// FName on the resulting FProperty (the path the sanitizer defends), the
// test asserts the exporter sanitizes it. If a downstream stage rejects
// or rewrites the FName before reflection, the test records a "SKIPPED:"
// marker naming the unexercised sanitizer and returns true; the production
// code path is then unreachable in practice and lines 56-58 are pure
// defense-in-depth, in which case the unit test
// FSanitizeXmlAttributeNameUnitTest above is the proper coverage anchor.
// The marker is AddInfo rather than AddWarning because this runner treats
// AddWarning as a test failure (see Tests/Bpir/TestCompilerIntegration.cpp
// ~3322) and this skip is a legitimate compiler-rewrite outcome, not a
// defect. Read a "SKIPPED:" line as "this test asserted nothing".
// ============================================================================


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FXmlExportSanitizesAttributeNameSpacesTest,
    "PinWright.widget.export_xml.SanitizesAttributeNameSpaces",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FXmlExportSanitizesAttributeNameSpacesTest::RunTest(const FString& Parameters)
{
    // Covers exporter line 230 (bindings path). For property-reflection
    // path coverage (lines 56-58) see FXmlExportPropertyReflectionPathSanitizationTest.
    // Build a transient WBP with a root TextBlock widget.
    const FString PackagePath = TEXT("/Game/_Test/WBP_SanitizeAttrTest");
    UPackage* Package = CreatePackage(*PackagePath);
    TestNotNull(TEXT("package created"), Package);
    if (!Package) return false;
    Package->SetFlags(RF_Transient);

    UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
        Package, TEXT("WBP_SanitizeAttrTest"), RF_Transient | RF_Public | RF_Standalone);
    TestNotNull(TEXT("WBP created"), WBP);
    if (!WBP) return false;

    WBP->WidgetTree = NewObject<UWidgetTree>(WBP, NAME_None, RF_Transient);
    WBP->Status = BS_BeingCreated;

    UTextBlock* Root = WBP->WidgetTree->ConstructWidget<UTextBlock>(
        UTextBlock::StaticClass(), TEXT("Root"));
    WBP->WidgetTree->RootWidget = Root;

    // Inject a binding with a space in the property name. BuildWidgetTreeXml reads
    // WidgetBlueprint->Bindings via reflection (BuildBindingMap). We call
    // BuildXmlString directly with a pre-built bindings map so the test doesn't
    // depend on FWidgetBinding struct layout.
    TMap<FString, TArray<TPair<FString, FString>>> BindingsMap;
    BindingsMap.FindOrAdd(TEXT("Root")).Emplace(TEXT("Axis Type"), TEXT("GetAxisType"));

    TMap<FName, FGuid> GuidMap;
    WidgetXmlExporter::FGeomContext GeomCtx; // bEnabled = false
    TArray<TPair<FString, FString>> ExtraAttrs;
    int32 WidgetCount = 0;

    FString Output = WidgetXmlExporter::BuildXmlString(
        Root, 0, /*bIncludeDefaults=*/false,
        BindingsMap, &GuidMap, WidgetCount,
        GeomCtx, /*bIsRoot=*/true, ExtraAttrs);

    // Bindings emit as Bind.<key>; the sanitizer must convert the space in
    // "Axis Type" to an underscore, yielding the attribute "Bind.Axis_Type".
    TestFalse(TEXT("output must not contain space in attribute name 'Bind.Axis Type='"),
        Output.Contains(TEXT("Bind.Axis Type=")));
    TestTrue(TEXT("output must contain sanitized attribute name 'Bind.Axis_Type='"),
        Output.Contains(TEXT("Bind.Axis_Type=")));

    // Verify the output is parseable by FXmlFile.
    FXmlFile ParseCheck(Output, EConstructMethod::ConstructFromBuffer);
    TestTrue(TEXT("sanitized XML parses without error"), ParseCheck.IsValid());

    return true;
}

// Covers exporter lines 56-58 (property-reflection path). Constructs a
// compiled BP with a member variable whose FName contains a space, then
// asserts that the exporter's CollectOverriddenAttributes sanitizes the
// resulting attribute key. If any stage of the BP variable pipeline
// rejects/rewrites the space FName before reflection, the test reports
// the constraint via a "SKIPPED:" marker and exits — the property-reflection
// sanitizer is then defense-in-depth and FSanitizeXmlAttributeNameUnitTest is
// the load-bearing coverage. Every skip names itself and the sanitizer it did
// not exercise, so a run where this test asserted nothing is greppable as
// "SKIPPED:" rather than indistinguishable from a real pass.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FXmlExportPropertyReflectionPathSanitizationTest,
    "PinWright.widget.export_xml.SanitizesAttributeNameSpacesPropertyPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FXmlExportPropertyReflectionPathSanitizationTest::RunTest(const FString& Parameters)
{
    const FString PackagePath = TEXT("/Game/_Test/WBP_SanitizePropertyPathTest");
    UPackage* Package = CreatePackage(*PackagePath);
    TestNotNull(TEXT("package created"), Package);
    if (!Package) return false;
    Package->SetFlags(RF_Transient);

    // CreateBlueprint allocates the UBlueprint and triggers skeleton compilation
    // — required so AddMemberVariable produces a real FProperty after the
    // subsequent CompileBlueprint call.
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        UUserWidget::StaticClass(),
        Package,
        TEXT("WBP_SanitizePropertyPathTest"),
        BPTYPE_Normal,
        UWidgetBlueprint::StaticClass(),
        UWidgetBlueprintGeneratedClass::StaticClass());

    if (!BP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("probe-blueprint-unavailable"),
            TEXT("SKIPPED: FXmlExportPropertyReflectionPathSanitizationTest; FKismetEditorUtilities::CreateBlueprint returned null — the property-reflection sanitizer at WidgetXmlExporter.cpp:56-58 was NOT exercised."));
        return true;
    }

    // Add a member variable whose FName contains a space. AddMemberVariable
    // only rejects NAME_None and duplicates — it does not validate against
    // spaces. The interesting question is whether the kismet compiler
    // preserves the FName on the resulting FProperty.
    const FName SpaceVarName(TEXT("Has Space"));
    FEdGraphPinType BoolPinType;
    BoolPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;

    const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(
        BP, SpaceVarName, BoolPinType, FString());
    if (!bAdded)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("probe-blueprint-unavailable"),
            TEXT("SKIPPED: FXmlExportPropertyReflectionPathSanitizationTest; FBlueprintEditorUtils::AddMemberVariable rejected the space-containing FName — the property-reflection sanitizer at WidgetXmlExporter.cpp:56-58 was NOT exercised; it is defense-in-depth and FSanitizeXmlAttributeNameUnitTest is the only live coverage of the helper."));
        return true;
    }

    FKismetEditorUtilities::CompileBlueprint(BP);

    UClass* GeneratedClass = BP->GeneratedClass;
    if (!GeneratedClass)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("probe-blueprint-unavailable"),
            TEXT("SKIPPED: FXmlExportPropertyReflectionPathSanitizationTest; BP->GeneratedClass is null after compile — the property-reflection sanitizer at WidgetXmlExporter.cpp:56-58 was NOT exercised."));
        return true;
    }

    // Locate the FProperty by its compiled FName. If the compiler munged
    // the space out of the FName, this lookup fails and we exit.
    FProperty* SpaceProperty = GeneratedClass->FindPropertyByName(SpaceVarName);
    if (!SpaceProperty || !SpaceProperty->GetName().Contains(TEXT(" ")))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("kismet-rewrote-property-name"),
            TEXT("SKIPPED: FXmlExportPropertyReflectionPathSanitizationTest; the compiled FProperty FName does not contain a space (the kismet compiler rewrote it), so WidgetXmlExporter.cpp:56-58 is unreachable in practice and was NOT exercised; FSanitizeXmlAttributeNameUnitTest is the only live coverage of the helper."));
        return true;
    }

    // FProperty must be visible to the exporter's ShouldExportProperty
    // gate (CPF_Edit | CPF_BlueprintVisible) and must differ from CDO so
    // the !bIncludeDefaults branch does not skip it. AddMemberVariable
    // sets CPF_Edit | CPF_BlueprintVisible by default. To guarantee
    // non-default, mutate the instance value below.

    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(BP);
    if (!WBP || !WBP->WidgetTree)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("probe-blueprint-unavailable"),
            TEXT("SKIPPED: FXmlExportPropertyReflectionPathSanitizationTest; cast to UWidgetBlueprint failed or WidgetTree missing — the property-reflection sanitizer at WidgetXmlExporter.cpp:56-58 was NOT exercised."));
        return true;
    }

    // Construct a widget instance of the compiled UserWidget class so
    // CollectOverriddenAttributes iterates the generated FProperties.
    UUserWidget* Instance = NewObject<UUserWidget>(WBP, GeneratedClass, NAME_None, RF_Transient);
    if (!Instance)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("probe-blueprint-unavailable"),
            TEXT("SKIPPED: FXmlExportPropertyReflectionPathSanitizationTest; NewObject failed for the compiled UUserWidget class — the property-reflection sanitizer at WidgetXmlExporter.cpp:56-58 was NOT exercised."));
        return true;
    }

    // Mutate the bool property to non-default (CDO defaults to false) so
    // the !bIncludeDefaults branch in CollectOverriddenAttributes emits it.
    if (FBoolProperty* BoolProp = CastField<FBoolProperty>(SpaceProperty))
    {
        void* ValuePtr = BoolProp->ContainerPtrToValuePtr<void>(Instance);
        BoolProp->SetPropertyValue(ValuePtr, true);
    }

    TMap<FString, TArray<TPair<FString, FString>>> BindingsMap;
    TMap<FName, FGuid> GuidMap;
    WidgetXmlExporter::FGeomContext GeomCtx;
    TArray<TPair<FString, FString>> ExtraAttrs;
    int32 WidgetCount = 0;

    FString Output = WidgetXmlExporter::BuildXmlString(
        Instance, 0, /*bIncludeDefaults=*/false,
        BindingsMap, &GuidMap, WidgetCount,
        GeomCtx, /*bIsRoot=*/true, ExtraAttrs);

    // Counterfactual: reverting SanitizeXmlName at WidgetXmlExporter.cpp:56-58
    // would cause "Has Space" to flow verbatim into the XML attribute key,
    // producing malformed output and breaking these assertions.
    TestFalse(TEXT("output must not contain space in property-derived attribute name ' Has Space='"),
        Output.Contains(TEXT(" Has Space=")));
    TestTrue(TEXT("output must contain sanitized property-derived attribute name ' Has_Space='"),
        Output.Contains(TEXT(" Has_Space=")));

    // Verify the output is parseable by FXmlFile.
    FXmlFile ParseCheck(Output, EConstructMethod::ConstructFromBuffer);
    TestTrue(TEXT("sanitized XML parses without error"), ParseCheck.IsValid());

    // RF_Transient package — GC will clean up.
    return true;
}

// ============================================================================
// 5c. SanitizesDigitPrefixedTag integration test
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FXmlExportSanitizesDigitPrefixedTagTest,
    "PinWright.widget.export_xml.SanitizesDigitPrefixedTag",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FXmlExportSanitizesDigitPrefixedTagTest::RunTest(const FString& Parameters)
{
    // Create a transient Blueprint whose generated class name begins with a digit.
    // FKismetEditorUtilities::CreateBlueprint + immediate compile is the only way
    // to get a live UClass whose GetName() returns "1_DigitPrefixed_C".
    const FString PackagePath = TEXT("/Game/_Test/WBP_DigitPrefixTest");
    UPackage* Package = CreatePackage(*PackagePath);
    TestNotNull(TEXT("package created"), Package);
    if (!Package) return false;
    Package->SetFlags(RF_Transient);

    // CreateBlueprint allocates the UBlueprint and triggers skeleton compilation.
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        UUserWidget::StaticClass(),
        Package,
        TEXT("1_DigitPrefixed"),
        BPTYPE_Normal,
        UWidgetBlueprint::StaticClass(),
        UWidgetBlueprintGeneratedClass::StaticClass());

    if (!BP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("probe-blueprint-unavailable"),
            TEXT("SKIPPED: FXmlExportSanitizesDigitPrefixedTagTest; FKismetEditorUtilities::CreateBlueprint returned null for the digit-prefixed name — the tag sanitizer was NOT exercised."));
        return true;
    }

    // Compile to materialise the generated class so GetClass()->GetName() works.
    FKismetEditorUtilities::CompileBlueprint(BP);

    UClass* GeneratedClass = BP->GeneratedClass;
    if (!GeneratedClass || !GeneratedClass->IsChildOf(UWidget::StaticClass()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("probe-blueprint-unavailable"),
            TEXT("SKIPPED: FXmlExportSanitizesDigitPrefixedTagTest; generated class is null or not a UWidget subclass after compile — the tag sanitizer was NOT exercised."));
        return true;
    }

    // Instantiate a widget of the digit-prefixed generated class.
    // Use the WBP's WidgetTree (needs a parent WBP for ConstructWidget).
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(BP);
    if (!WBP || !WBP->WidgetTree)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("probe-blueprint-unavailable"),
            TEXT("SKIPPED: FXmlExportSanitizesDigitPrefixedTagTest; cast to UWidgetBlueprint failed or WidgetTree missing — the tag sanitizer was NOT exercised."));
        return true;
    }

    UWidget* Widget = WBP->WidgetTree->ConstructWidget<UWidget>(GeneratedClass, TEXT("DigitWidget"));
    if (!Widget)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("probe-blueprint-unavailable"),
            TEXT("SKIPPED: FXmlExportSanitizesDigitPrefixedTagTest; ConstructWidget returned null for the digit-prefixed class — the tag sanitizer was NOT exercised."));
        return true;
    }

    // Drive the exporter directly — same call site as T1's test.
    TMap<FString, TArray<TPair<FString, FString>>> BindingsMap;
    TMap<FName, FGuid> GuidMap;
    WidgetXmlExporter::FGeomContext GeomCtx;
    TArray<TPair<FString, FString>> ExtraAttrs;
    int32 WidgetCount = 0;

    FString Output = WidgetXmlExporter::BuildXmlString(
        Widget, 0, /*bIncludeDefaults=*/false,
        BindingsMap, &GuidMap, WidgetCount,
        GeomCtx, /*bIsRoot=*/true, ExtraAttrs);

    TestFalse(TEXT("output must not contain a digit-prefixed element tag '<1_'"),
        Output.Contains(TEXT("<1_")));

    TestTrue(TEXT("output must contain original-name attribute for round-trip"),
        Output.Contains(TEXT("original-name=\"1_DigitPrefixed_C\"")));

    // Verify the output parses as well-formed XML.
    FXmlFile ParseCheck(Output, EConstructMethod::ConstructFromBuffer);
    TestTrue(TEXT("sanitized XML parses without error"), ParseCheck.IsValid());

    // RF_Transient package — GC will clean up.
    return true;
}

// ============================================================================
// 5d. Round-Trip Integration Test
// ============================================================================

using WidgetXmlTestHelpers::MakeXmlTestAssetPath;
using WidgetXmlTestHelpers::MakeOnDiskShapedWidgetBlueprint;
using WidgetXmlTestHelpers::CreateXmlTestWidget;
using WidgetXmlTestHelpers::CleanupXmlTestAsset;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FXmlExportInheritedRootHandlerTest,
    "PinWright.widget.export_xml.InheritedRoot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FXmlExportInheritedRootHandlerTest::RunTest(const FString& Parameters)
{
    const FString ParentPath = MakeXmlTestAssetPath(TEXT("WBP_XmlInheritedRoot_Parent"));
    const FString ChildPath = MakeXmlTestAssetPath(TEXT("WBP_XmlInheritedRoot_Child"));
    ON_SCOPE_EXIT
    {
        CleanupXmlTestAsset(ChildPath);
        CleanupXmlTestAsset(ParentPath);
    };

    UPackage* ParentPackage = CreatePackage(*ParentPath);
    TestNotNull(TEXT("parent package created"), ParentPackage);
    if (!ParentPackage) return false;
    ParentPackage->SetFlags(RF_Transient);

    UBlueprint* ParentBP = FKismetEditorUtilities::CreateBlueprint(
        UUserWidget::StaticClass(),
        ParentPackage,
        *FPackageName::GetLongPackageAssetName(ParentPath),
        BPTYPE_Normal,
        UWidgetBlueprint::StaticClass(),
        UWidgetBlueprintGeneratedClass::StaticClass());

    UWidgetBlueprint* ParentWBP = Cast<UWidgetBlueprint>(ParentBP);
    TestNotNull(TEXT("parent WBP created"), ParentWBP);
    if (!ParentWBP || !ParentWBP->WidgetTree) return false;

    UCanvasPanel* ParentRoot = ParentWBP->WidgetTree->ConstructWidget<UCanvasPanel>(
        UCanvasPanel::StaticClass(), TEXT("ParentRoot"));
    ParentWBP->WidgetTree->RootWidget = ParentRoot;

    FKismetEditorUtilities::CompileBlueprint(ParentBP);

    UClass* ParentGeneratedClass = ParentBP ? ParentBP->GeneratedClass : nullptr;
    TestNotNull(TEXT("parent generated class exists"), ParentGeneratedClass);
    if (!ParentGeneratedClass) return false;

    UPackage* ChildPackage = CreatePackage(*ChildPath);
    TestNotNull(TEXT("child package created"), ChildPackage);
    if (!ChildPackage) return false;
    ChildPackage->SetFlags(RF_Transient);

    UBlueprint* ChildBP = FKismetEditorUtilities::CreateBlueprint(
        ParentGeneratedClass,
        ChildPackage,
        *FPackageName::GetLongPackageAssetName(ChildPath),
        BPTYPE_Normal,
        UWidgetBlueprint::StaticClass(),
        UWidgetBlueprintGeneratedClass::StaticClass());

    UWidgetBlueprint* ChildWBP = Cast<UWidgetBlueprint>(ChildBP);
    TestNotNull(TEXT("child WBP created"), ChildWBP);
    if (!ChildWBP || !ChildWBP->WidgetTree) return false;

    ChildWBP->WidgetTree->RootWidget = nullptr;
    FKismetEditorUtilities::CompileBlueprint(ChildBP);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), ChildPath);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("widget.export_xml"), Payload, Capture);
    TestTrue(TEXT("handler found"), bFound);
    TestTrue(TEXT("response was captured"), Capture.bWasCalled);
    TestTrue(TEXT("export succeeds"), Capture.bSuccess);
    TestNotEqual(TEXT("error code is not TREE_EMPTY"),
        Capture.ErrorCode, FString(TEXT("TREE_EMPTY")));
    TestTrue(TEXT("result is valid"), Capture.Result.IsValid());
    if (!Capture.Result.IsValid()) return false;

    const FString Xml = Capture.Result->GetStringField(TEXT("xml"));
    TestFalse(TEXT("xml is non-empty"), Xml.IsEmpty());
    TestTrue(TEXT("xml contains inherited root"), Xml.Contains(TEXT("ParentRoot")));
    TestTrue(TEXT("xml contains inherited_from attribute"), Xml.Contains(TEXT("inherited_from=")));
    TestTrue(TEXT("xml contains parent WBP path"), Xml.Contains(ParentWBP->GetPathName()));
    TestEqual(TEXT("widget_count is inherited root only"),
        static_cast<int32>(Capture.Result->GetNumberField(TEXT("widget_count"))), 1);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FXmlRoundTripTest,
    "PinWright.widget.export_xml.RoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FXmlRoundTripTest::RunTest(const FString& Parameters)
{
    const FString SourcePath = MakeXmlTestAssetPath(TEXT("WBP_XmlRoundTrip_Src"));
    const FString TargetPath = MakeXmlTestAssetPath(TEXT("WBP_XmlRoundTrip_Tgt"));
    FTestResponseCapture Capture;

    // 1. Build source widget blueprint in memory and populate the tree directly.
    // Going through widget.create_widget_blueprint / widget.add / widget.set
    // would trigger FKismetEditorUtilities::CreateBlueprint plus several
    // MarkBlueprintAsStructurallyModified passes (~6-8s). The export handler
    // only reads the WidgetTree so we can skip all that.
    UWidgetBlueprint* SourceBP = MakeOnDiskShapedWidgetBlueprint(SourcePath);
    TestNotNull(TEXT("source blueprint allocated"), SourceBP);
    if (!SourceBP || !SourceBP->WidgetTree) return false;

    UVerticalBox* RootVBox = SourceBP->WidgetTree->ConstructWidget<UVerticalBox>(
        UVerticalBox::StaticClass(), TEXT("RootVBox"));
    SourceBP->WidgetTree->RootWidget = RootVBox;

    UTextBlock* MyLabel = SourceBP->WidgetTree->ConstructWidget<UTextBlock>(
        UTextBlock::StaticClass(), TEXT("MyLabel"));
    RootVBox->AddChild(MyLabel);
    MyLabel->SetText(FText::FromString(TEXT("Hello XML")));

    // ConstructWidget does not register variable GUIDs; the export reads
    // WidgetVariableNameToGuidMap to mark exported widgets. Register both so
    // the round-trip preserves variable status.
    // UE 5.4/5.5 has no OnVariableAdded / GUID map; the exporter there reads
    // UWidget::bIsVariable (true by default), so registration is a no-op.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    SourceBP->OnVariableAdded(RootVBox->GetFName());
    SourceBP->OnVariableAdded(MyLabel->GetFName());
#endif

    // 2. Export the source widget as XML
    TSharedPtr<FJsonObject> ExportPayload = MakeShared<FJsonObject>();
    ExportPayload->SetStringField(TEXT("widgetPath"), SourcePath);
    InvokeHandlerWithCapture(TEXT("widget.export_xml"), ExportPayload, Capture);
    TestTrue(TEXT("export source succeeded"), Capture.bSuccess);

    FString SourceXml;
    if (Capture.Result.IsValid())
    {
        SourceXml = Capture.Result->GetStringField(TEXT("xml"));
    }
    TestFalse(TEXT("source xml not empty"), SourceXml.IsEmpty());

    // 3. Verify XML contains expected tags
    TestTrue(TEXT("xml contains VerticalBox tag"), SourceXml.Contains(TEXT("VerticalBox")));
    TestTrue(TEXT("xml contains TextBlock tag"), SourceXml.Contains(TEXT("TextBlock")));
    TestTrue(TEXT("xml contains name RootVBox"), SourceXml.Contains(TEXT("RootVBox")));
    TestTrue(TEXT("xml contains name MyLabel"), SourceXml.Contains(TEXT("MyLabel")));

    // 3b. Tags and names alone leave the round trip blind to property VALUES: every
    // assertion above stays true if CollectOverriddenAttributes stops emitting
    // properties entirely, and step 7 below only compares the exporter's output
    // against its own re-export, so a symmetric drop is invisible. "Hello XML" is
    // the one value this fixture pins, and it is an independent reference (the
    // literal set on the source UTextBlock at step 1, not anything the exporter
    // produced), so it is asserted on both halves of the trip.
    TestTrue(TEXT("source xml carries the TextBlock's Text value"),
        SourceXml.Contains(TEXT("Hello XML")));

    // 4. Build a fresh, empty target widget blueprint in memory.
    UWidgetBlueprint* TargetBP = MakeOnDiskShapedWidgetBlueprint(TargetPath);
    TestNotNull(TEXT("target blueprint allocated"), TargetBP);
    if (!TargetBP || !TargetBP->WidgetTree) return false;

    // 5. Import the XML into the target blueprint
    TSharedPtr<FJsonObject> ImportPayload = MakeShared<FJsonObject>();
    ImportPayload->SetStringField(TEXT("widgetPath"), TargetPath);
    ImportPayload->SetStringField(TEXT("xml"), SourceXml);
    ImportPayload->SetStringField(TEXT("mode"), TEXT("replace"));
    InvokeHandlerWithCapture(TEXT("widget.import_xml"), ImportPayload, Capture);
    TestTrue(TEXT("import into target succeeded"), Capture.bSuccess);

    // 6. Export the target blueprint
    TSharedPtr<FJsonObject> ExportTarget = MakeShared<FJsonObject>();
    ExportTarget->SetStringField(TEXT("widgetPath"), TargetPath);
    InvokeHandlerWithCapture(TEXT("widget.export_xml"), ExportTarget, Capture);
    TestTrue(TEXT("export target succeeded"), Capture.bSuccess);

    FString TargetXml;
    if (Capture.Result.IsValid())
    {
        TargetXml = Capture.Result->GetStringField(TEXT("xml"));
    }
    TestFalse(TEXT("target xml not empty"), TargetXml.IsEmpty());

    // 7. Verify both XMLs have the same structure
    TestTrue(TEXT("target xml contains VerticalBox"), TargetXml.Contains(TEXT("VerticalBox")));
    TestTrue(TEXT("target xml contains TextBlock"), TargetXml.Contains(TEXT("TextBlock")));
    TestTrue(TEXT("target xml contains RootVBox"), TargetXml.Contains(TEXT("RootVBox")));
    TestTrue(TEXT("target xml contains MyLabel"), TargetXml.Contains(TEXT("MyLabel")));

    // 8. Assert the imported tree against the real UWidget objects, not against the
    // exporter's own re-emission. Class identity + the pinned Text value are what
    // distinguish a real round trip from one where widget.import_xml constructed the
    // right tag but dropped every attribute (ApplyAttributeToObject at
    // WidgetXmlImportHandler.cpp:451 is the line this covers) — that failure mode
    // satisfies every substring assertion above.
    UWidget* ImportedRoot = TargetBP->WidgetTree->RootWidget;
    TestNotNull(TEXT("imported target has a root widget"), ImportedRoot);
    TestTrue(TEXT("imported root is the UVerticalBox from the source"),
        ImportedRoot && ImportedRoot->IsA<UVerticalBox>());

    UWidget* ImportedLabelWidget = TargetBP->WidgetTree->FindWidget(FName(TEXT("MyLabel")));
    TestNotNull(TEXT("imported MyLabel resolves by name in the target tree"), ImportedLabelWidget);
    UTextBlock* ImportedLabel = Cast<UTextBlock>(ImportedLabelWidget);
    TestNotNull(TEXT("imported MyLabel is a UTextBlock"), ImportedLabel);
    if (ImportedLabel)
    {
        TestEqual(TEXT("imported MyLabel carries the source Text value"),
            ImportedLabel->GetText().ToString(), FString(TEXT("Hello XML")));
    }

    TestTrue(TEXT("re-exported target xml carries the TextBlock's Text value"),
        TargetXml.Contains(TEXT("Hello XML")));

    // RF_Transient packages are reaped by the next GC pass — no manual
    // CleanupTestAsset / CollectGarbage round-trip is required.
    return true;
}

// ============================================================================
// 6. Import Mode Tests (require real assets)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FXmlImportReplaceNonExistentTargetTest,
    "PinWright.widget.import_xml.ReplaceNonExistentTarget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FXmlImportReplaceNonExistentTargetTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeXmlTestAssetPath(TEXT("WBP_XmlReplace"));
    FTestResponseCapture Capture;

    // Create a widget blueprint so the asset exists
    TestTrue(TEXT("create handler found"), CreateXmlTestWidget(AssetPath, Capture));
    TestTrue(TEXT("create succeeded"), Capture.bSuccess);

    // Try to replace a non-existent target widget
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), AssetPath);
    Payload->SetStringField(TEXT("xml"), TEXT("<VerticalBox name=\"NewRoot\" />"));
    Payload->SetStringField(TEXT("mode"), TEXT("replace"));
    Payload->SetStringField(TEXT("targetName"), TEXT("WidgetThatDoesNotExist"));
    InvokeHandlerWithCapture(TEXT("widget.import_xml"), Payload, Capture);
    TestFalse(TEXT("should fail for non-existent target"), Capture.bSuccess);
    TestEqual(TEXT("error code is NOT_FOUND"), Capture.ErrorCode, TEXT("NOT_FOUND"));

    CleanupXmlTestAsset(AssetPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FXmlImportAddToNonPanelTargetTest,
    "PinWright.widget.add.AddToNonPanelTarget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FXmlImportAddToNonPanelTargetTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeXmlTestAssetPath(TEXT("WBP_XmlAddNonPanel"));
    FTestResponseCapture Capture;

    // Create widget blueprint and add a TextBlock (non-panel) widget
    TestTrue(TEXT("create handler found"), CreateXmlTestWidget(AssetPath, Capture));
    TestTrue(TEXT("create succeeded"), Capture.bSuccess);

    TSharedPtr<FJsonObject> AddText = MakeShared<FJsonObject>();
    AddText->SetStringField(TEXT("widgetPath"), AssetPath);
    AddText->SetStringField(TEXT("type"), TEXT("TextBlock"));
    AddText->SetStringField(TEXT("name"), TEXT("LeafText"));
    InvokeHandlerWithCapture(TEXT("widget.add"), AddText, Capture);
    TestTrue(TEXT("add TextBlock succeeded"), Capture.bSuccess);

    // Try to add children under a non-panel widget
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), AssetPath);
    Payload->SetStringField(TEXT("xml"), TEXT("<Button name=\"NewChild\" />"));
    Payload->SetStringField(TEXT("mode"), TEXT("add"));
    Payload->SetStringField(TEXT("targetName"), TEXT("LeafText"));
    InvokeHandlerWithCapture(TEXT("widget.import_xml"), Payload, Capture);
    TestFalse(TEXT("should fail adding to non-panel"), Capture.bSuccess);
    TestEqual(TEXT("error code is INVALID_TARGET"), Capture.ErrorCode, TEXT("INVALID_TARGET"));

    CleanupXmlTestAsset(AssetPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FXmlImportUnknownWidgetClassTest,
    "PinWright.widget.import_xml.UnknownWidgetClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FXmlImportUnknownWidgetClassTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeXmlTestAssetPath(TEXT("WBP_XmlUnknownClass"));
    FTestResponseCapture Capture;

    // Create a real widget blueprint so asset lookup succeeds
    TestTrue(TEXT("create handler found"), CreateXmlTestWidget(AssetPath, Capture));
    TestTrue(TEXT("create succeeded"), Capture.bSuccess);

    // Import XML with a tag that doesn't map to any widget class
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), AssetPath);
    Payload->SetStringField(TEXT("xml"), TEXT("<CompletelyBogusWidgetClass_XYZ123 name=\"Bad\" />"));
    Payload->SetStringField(TEXT("mode"), TEXT("replace"));
    InvokeHandlerWithCapture(TEXT("widget.import_xml"), Payload, Capture);
    TestFalse(TEXT("should fail with unknown widget class"), Capture.bSuccess);
    TestEqual(TEXT("error code is VALIDATION_FAILED"), Capture.ErrorCode, TEXT("VALIDATION_FAILED"));

    CleanupXmlTestAsset(AssetPath);
    return true;
}

// Asset-dump export must emit no `Geom.*` attributes; per-element gate already handles
// descendants, root sentinel was the leak.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FXmlAssetDumpExportNoGeomSourceOffTest,
    "PinWright.widget.export_xml.AssetDumpNoGeomSourceOff",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FXmlAssetDumpExportNoGeomSourceOffTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeXmlTestAssetPath(TEXT("WBP_XmlGeomSourceOff"));

    UWidgetBlueprint* WBP = MakeOnDiskShapedWidgetBlueprint(AssetPath);
    TestNotNull(TEXT("blueprint allocated"), WBP);
    if (!WBP || !WBP->WidgetTree) return false;

    UVerticalBox* Root = WBP->WidgetTree->ConstructWidget<UVerticalBox>(
        UVerticalBox::StaticClass(), TEXT("RootVBox"));
    WBP->WidgetTree->RootWidget = Root;

    UTextBlock* Child = WBP->WidgetTree->ConstructWidget<UTextBlock>(
        UTextBlock::StaticClass(), TEXT("ChildLabel"));
    Root->AddChild(Child);

    const FString Xml = WidgetXmlExporter::BuildWidgetTreeXml(WBP, /*bIncludeDefaults=*/false);

    TestFalse(TEXT("xml not empty"), Xml.IsEmpty());
    TestFalse(TEXT("xml must not contain Geom.source=\"off\""),
        Xml.Contains(TEXT("Geom.source=\"off\"")));
    TestFalse(TEXT("xml must not contain any Geom.source= attribute"),
        Xml.Contains(TEXT("Geom.source=")));
    TestTrue(TEXT("xml contains VerticalBox root tag"),
        Xml.Contains(TEXT("VerticalBox")));

    return true;
}

// Exporter must drop the `_C` BPGC suffix from element tag names for nested
// user-widget instances. Counterfactual: if `.RemoveFromEnd(TEXT("_C"))` is
// removed from `StripClassPrefix` in `WidgetXmlUtils.h`, this test fails
// because the child node's tag becomes `"WBP_InnerUser_C"` (BPGC name leaks
// through).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FXmlExportUserWidgetTagDropsCSuffixTest,
    "PinWright.widget.export_xml.UserWidgetTagDropsCSuffix",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FXmlExportUserWidgetTagDropsCSuffixTest::RunTest(const FString& Parameters)
{
    // 1. Build & compile an inner WBP so its generated class GetName() carries "_C".
    const FString InnerPackagePath = TEXT("/Game/_Test/WBP_InnerUser_Pkg");
    UPackage* InnerPackage = CreatePackage(*InnerPackagePath);
    TestNotNull(TEXT("inner package created"), InnerPackage);
    if (!InnerPackage) return false;
    InnerPackage->SetFlags(RF_Transient);

    UBlueprint* InnerBP = FKismetEditorUtilities::CreateBlueprint(
        UUserWidget::StaticClass(),
        InnerPackage,
        TEXT("WBP_InnerUser"),
        BPTYPE_Normal,
        UWidgetBlueprint::StaticClass(),
        UWidgetBlueprintGeneratedClass::StaticClass());

    TestNotNull(TEXT("FKismetEditorUtilities::CreateBlueprint returned non-null"), InnerBP);
    if (!InnerBP) return false;

    UWidgetBlueprint* InnerWBP = Cast<UWidgetBlueprint>(InnerBP);
    TestNotNull(TEXT("cast to UWidgetBlueprint succeeded"), InnerWBP);
    if (!InnerWBP) return false;
    TestNotNull(TEXT("inner WidgetTree present"), InnerWBP->WidgetTree.Get());
    if (!InnerWBP->WidgetTree) return false;

    // Populate inner widget tree (CanvasPanel root + TextBlock child) before compile.
    UCanvasPanel* InnerRoot = InnerWBP->WidgetTree->ConstructWidget<UCanvasPanel>(
        UCanvasPanel::StaticClass(), TEXT("InnerRoot"));
    InnerWBP->WidgetTree->RootWidget = InnerRoot;
    UTextBlock* InnerLabel = InnerWBP->WidgetTree->ConstructWidget<UTextBlock>(
        UTextBlock::StaticClass(), TEXT("InnerLabel"));
    InnerRoot->AddChild(InnerLabel);

    FKismetEditorUtilities::CompileBlueprint(InnerBP);

    UClass* InnerBPGC = InnerBP->GeneratedClass;
    TestNotNull(TEXT("inner generated class produced by compile"), InnerBPGC);
    if (!InnerBPGC) return false;
    TestTrue(TEXT("inner generated class is a UUserWidget subclass"),
        InnerBPGC->IsChildOf(UUserWidget::StaticClass()));
    if (!InnerBPGC->IsChildOf(UUserWidget::StaticClass())) return false;

    // Precondition: the BPGC's GetName() must carry "_C" — if this stops being true,
    // the fix is moot and so is this test.
    TestTrue(TEXT("compiled BPGC GetName() ends with _C"),
        InnerBPGC->GetName().EndsWith(TEXT("_C")));
    if (!InnerBPGC->GetName().EndsWith(TEXT("_C"))) return false;

    // 2. Build the outer WBP with a CanvasPanel root and an instance of the inner BPGC under it.
    const FString OuterPackagePath = TEXT("/Game/_Test/WBP_OuterUser_Pkg");
    UPackage* OuterPackage = CreatePackage(*OuterPackagePath);
    TestNotNull(TEXT("outer package created"), OuterPackage);
    if (!OuterPackage) return false;
    OuterPackage->SetFlags(RF_Transient);

    UWidgetBlueprint* OuterWBP = NewObject<UWidgetBlueprint>(
        OuterPackage, TEXT("WBP_OuterUser"), RF_Transient | RF_Public | RF_Standalone);
    TestNotNull(TEXT("outer WBP created"), OuterWBP);
    if (!OuterWBP) return false;
    OuterWBP->WidgetTree = NewObject<UWidgetTree>(OuterWBP, NAME_None, RF_Transient);
    OuterWBP->Status = BS_BeingCreated;

    UCanvasPanel* OuterRoot = OuterWBP->WidgetTree->ConstructWidget<UCanvasPanel>(
        UCanvasPanel::StaticClass(), TEXT("OuterRoot"));
    OuterWBP->WidgetTree->RootWidget = OuterRoot;

    UUserWidget* NestedUser = OuterWBP->WidgetTree->ConstructWidget<UUserWidget>(
        InnerBPGC, TEXT("NestedUser"));
    TestNotNull(TEXT("ConstructWidget<UUserWidget> returned a widget for inner BPGC"), NestedUser);
    if (!NestedUser) return false;
    OuterRoot->AddChild(NestedUser);

    // 3. Export and parse.
    const FString Xml = WidgetXmlExporter::BuildWidgetTreeXml(OuterWBP, /*bIncludeDefaults=*/false);
    TestFalse(TEXT("xml not empty"), Xml.IsEmpty());

    FXmlFile Parsed(Xml, EConstructMethod::ConstructFromBuffer);
    TestTrue(TEXT("xml parses without error"), Parsed.IsValid());
    if (!Parsed.IsValid()) return false;

    // Walk to the child node — root is the CanvasPanel "OuterRoot", first child is the nested user widget.
    FXmlNode* RootNode = Parsed.GetRootNode();
    TestNotNull(TEXT("root node present"), RootNode);
    if (!RootNode) return false;

    const TArray<FXmlNode*>& RootChildren = RootNode->GetChildrenNodes();
    TestTrue(TEXT("root has at least one child"), RootChildren.Num() >= 1);
    if (RootChildren.Num() < 1) return false;

    FXmlNode* ChildNode = RootChildren[0];
    TestNotNull(TEXT("child node present"), ChildNode);
    if (!ChildNode) return false;

    // Core assertion: tag is the stripped short name, no "_C" suffix.
    TestEqual(TEXT("child node tag is 'WBP_InnerUser' (no _C suffix)"),
        ChildNode->GetTag(), FString(TEXT("WBP_InnerUser")));

    // Defensive: the tag must not contain "_C" substring at all.
    TestFalse(TEXT("child node tag must not contain '_C' substring"),
        ChildNode->GetTag().Contains(TEXT("_C")));

    // Note: ResolveWidgetClassFromTag lives in an anonymous namespace inside
    // WidgetXmlImportHandler.cpp and is not directly callable from the test
    // target; round-trip via the import handler is covered by FXmlRoundTripTest.

    // RF_Transient packages are reaped by the next GC pass — no manual cleanup needed.
    return true;
}

// ============================================================================
// E-widget-export-xml-token-limit: CollectOverriddenAttributes skip set
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FXmlExportSkipSetDropsParentContentTest,
    "PinWright.widget.export_xml.OmitSlotChainDropsSkipSetProperties",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FXmlExportSkipSetDropsParentContentTest::RunTest(const FString& Parameters)
{
    UWidgetTree* Tree = NewObject<UWidgetTree>(GetTransientPackage());
    UCanvasPanel* Canvas = Tree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass());
    Tree->RootWidget = Canvas;
    UTextBlock* Child = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
    Canvas->AddChild(Child);
    UPanelSlot* Slot = Child->Slot;
    TestNotNull(TEXT("slot exists"), Slot);
    if (!Slot) return false;

    TArray<TPair<FString, FString>> WithoutSkip =
        WidgetXmlExporter::CollectOverriddenAttributes(Slot, /*bIncludeDefaults=*/true, TEXT("Slot."));
    bool bHasParentWithout = false, bHasContentWithout = false;
    for (const auto& P : WithoutSkip)
    {
        if (P.Key.EndsWith(TEXT(".Parent"))) bHasParentWithout = true;
        if (P.Key.EndsWith(TEXT(".Content"))) bHasContentWithout = true;
    }
    TestTrue(TEXT("baseline emits Slot.Parent"), bHasParentWithout);
    TestTrue(TEXT("baseline emits Slot.Content"), bHasContentWithout);

    TSet<FString> SkipSet;
    SkipSet.Add(TEXT("Parent"));
    SkipSet.Add(TEXT("Content"));
    TArray<TPair<FString, FString>> WithSkip =
        WidgetXmlExporter::CollectOverriddenAttributes(Slot, /*bIncludeDefaults=*/true, TEXT("Slot."), &SkipSet);
    for (const auto& P : WithSkip)
    {
        TestFalse(FString::Printf(TEXT("skip set drops %s"), *P.Key),
            P.Key.EndsWith(TEXT(".Parent")) || P.Key.EndsWith(TEXT(".Content")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FXmlExportCompactOmitsRawWidgetSlotChainTest,
    "PinWright.widget.export_xml.CompactOmitsRawWidgetSlotChain",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FXmlExportCompactOmitsRawWidgetSlotChainTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeXmlTestAssetPath(TEXT("WBP_XmlCompactRawSlot"));
    ON_SCOPE_EXIT
    {
        CleanupXmlTestAsset(AssetPath);
    };

    UWidgetBlueprint* WBP = MakeOnDiskShapedWidgetBlueprint(AssetPath);
    TestNotNull(TEXT("blueprint allocated"), WBP);
    if (!WBP || !WBP->WidgetTree) return false;

    UCanvasPanel* Root = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(
        UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
    WBP->WidgetTree->RootWidget = Root;

    UTextBlock* FirstChild = WBP->WidgetTree->ConstructWidget<UTextBlock>(
        UTextBlock::StaticClass(), TEXT("FirstText"));
    UCanvasPanelSlot* FirstSlot = Cast<UCanvasPanelSlot>(Root->AddChild(FirstChild));
    TestNotNull(TEXT("first child has canvas slot"), FirstSlot);
    if (!FirstSlot) return false;
    FirstSlot->SetPosition(FVector2D(42.0f, 84.0f));
    FirstSlot->SetSize(FVector2D(320.0f, 48.0f));

    UTextBlock* SecondChild = WBP->WidgetTree->ConstructWidget<UTextBlock>(
        UTextBlock::StaticClass(), TEXT("SecondText"));
    Root->AddChild(SecondChild);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), AssetPath);
    Payload->SetBoolField(TEXT("compact"), true);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("widget.export_xml"), Payload, Capture);
    TestTrue(TEXT("handler found"), bFound);
    TestTrue(TEXT("export succeeds"), Capture.bSuccess);
    TestTrue(TEXT("result is valid"), Capture.Result.IsValid());
    if (!Capture.Result.IsValid()) return false;

    const FString Xml = Capture.Result->GetStringField(TEXT("xml"));
    TestFalse(TEXT("xml is non-empty"), Xml.IsEmpty());
    TestFalse(TEXT("compact xml omits raw Slot attribute"), Xml.Contains(TEXT(" Slot=\"")));
    TestFalse(TEXT("compact xml omits parent slot chain"), Xml.Contains(TEXT("Parent={Slots=")));
    TestFalse(TEXT("compact xml omits content slot chain"), Xml.Contains(TEXT("Content={")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FXmlExportCompactEmitsFalseOverrideBoolTest,
    "PinWright.widget.export_xml.CompactEmitsFalseOverrideBool",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FXmlExportCompactEmitsFalseOverrideBoolTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeXmlTestAssetPath(TEXT("WBP_XmlCompactOverrideBool"));
    ON_SCOPE_EXIT
    {
        CleanupXmlTestAsset(AssetPath);
    };

    UWidgetBlueprint* WBP = MakeOnDiskShapedWidgetBlueprint(AssetPath);
    TestNotNull(TEXT("blueprint allocated"), WBP);
    if (!WBP || !WBP->WidgetTree) return false;

    USizeBox* Root = WBP->WidgetTree->ConstructWidget<USizeBox>(
        USizeBox::StaticClass(), TEXT("RootSizeBox"));
    WBP->WidgetTree->RootWidget = Root;

    const FString Xml = WidgetXmlExporter::BuildWidgetTreeXmlWithDiagnostic(
        WBP, /*bIncludeDefaults=*/false, /*bOmitSlotChain=*/true).Xml;

    TestFalse(TEXT("xml is non-empty"), Xml.IsEmpty());
    TestTrue(TEXT("compact xml emits explicit false bOverride_WidthOverride"),
        Xml.Contains(TEXT("bOverride_WidthOverride=\"false\"")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FXmlExportBrushResourceObjectUsesFullExportTextPathTest,
    "PinWright.widget.export_xml.BrushResourceObjectUsesFullExportTextPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FXmlExportBrushResourceObjectUsesFullExportTextPathTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeXmlTestAssetPath(TEXT("WBP_XmlBrushResourceObject"));
    ON_SCOPE_EXIT
    {
        CleanupXmlTestAsset(AssetPath);
    };

    UWidgetBlueprint* WBP = MakeOnDiskShapedWidgetBlueprint(AssetPath);
    TestNotNull(TEXT("blueprint allocated"), WBP);
    if (!WBP || !WBP->WidgetTree) return false;

    UImage* Root = WBP->WidgetTree->ConstructWidget<UImage>(
        UImage::StaticClass(), TEXT("RootImage"));
    WBP->WidgetTree->RootWidget = Root;

    UTexture2D* Texture = NewObject<UTexture2D>(
        WBP->GetOutermost(), TEXT("T_XmlBrushResourceObject"), RF_Transient | RF_Public);
    TestNotNull(TEXT("texture allocated"), Texture);
    if (!Texture) return false;

    FSlateBrush Brush;
    Brush.SetResourceObject(Texture);
    Root->SetBrush(Brush);

    const FString Xml = WidgetXmlExporter::BuildWidgetTreeXmlWithDiagnostic(
        WBP, /*bIncludeDefaults=*/false, /*bOmitSlotChain=*/true).Xml;

    const FString ExpectedResourceObject = FString::Printf(
        TEXT("ResourceObject=/Script/Engine.Texture2D'%s'"), *Texture->GetPathName());

    TestFalse(TEXT("xml is non-empty"), Xml.IsEmpty());
    TestTrue(TEXT("brush ResourceObject uses full export-text path"),
        Xml.Contains(ExpectedResourceObject));

    return true;
}
