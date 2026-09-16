// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"

#include "Blueprint/WidgetTree.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"


namespace
{
    FString MakeRequiredNameAssetPath(const FString& Prefix)
    {
        return FString::Printf(TEXT("/Game/_Test/%s_%s"),
            *Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    UWidgetBlueprint* MakeRequiredNameWidgetBlueprint(const FString& PackagePath)
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

    bool ExpectMissingNameFailure(FAutomationTestBase& Test, const FString& MethodName,
        const TSharedPtr<FJsonObject>& Payload, const FString& ExpectedErrorCode,
        const FString& ExpectedMessageFragment)
    {
        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(MethodName, Payload, Capture);
        Test.TestTrue(FString::Printf(TEXT("%s handler found"), *MethodName), bFound);
        Test.TestTrue(FString::Printf(TEXT("%s sent response"), *MethodName), Capture.bWasCalled);
        Test.TestFalse(FString::Printf(TEXT("%s rejected missing name"), *MethodName), Capture.bSuccess);
        Test.TestEqual(FString::Printf(TEXT("%s error code"), *MethodName),
            Capture.ErrorCode, ExpectedErrorCode);
        Test.TestTrue(FString::Printf(TEXT("%s message names required field"), *MethodName),
            Capture.Message.Contains(ExpectedMessageFragment));
        return bFound && Capture.bWasCalled && !Capture.bSuccess &&
            Capture.ErrorCode == ExpectedErrorCode &&
            Capture.Message.Contains(ExpectedMessageFragment);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetAddRejectsMissingNameDirectTest,
    "PinWright.widget.required_names.widget_add.MissingName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetAddRejectsMissingNameDirectTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), TEXT("/Game/_Test/WBP_RequiredName"));
    Payload->SetStringField(TEXT("type"), TEXT("TextBlock"));

    return ExpectMissingNameFailure(*this, TEXT("widget.add"), Payload,
        TEXT("MISSING_PARAMETER"), TEXT("name"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetWrapRejectsMissingWrapperNameDirectTest,
    "PinWright.widget.required_names.widget_wrap.MissingWrapperName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetWrapRejectsMissingWrapperNameDirectTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), TEXT("/Game/_Test/WBP_RequiredName"));
    Payload->SetStringField(TEXT("targetName"), TEXT("Child"));
    Payload->SetStringField(TEXT("wrapperType"), TEXT("Overlay"));

    return ExpectMissingNameFailure(*this, TEXT("widget.wrap"), Payload,
        TEXT("INVALID_PARAMS"), TEXT("wrapperName"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDuplicateRejectsMissingNameDirectTest,
    "PinWright.widget.required_names.widget_duplicate.MissingName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetDuplicateRejectsMissingNameDirectTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), TEXT("/Game/_Test/WBP_RequiredName"));
    Payload->SetStringField(TEXT("sourceName"), TEXT("Source"));

    return ExpectMissingNameFailure(*this, TEXT("widget.duplicate"), Payload,
        TEXT("INVALID_PARAMS"), TEXT("name"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetImportXmlRejectsMissingElementNameDirectTest,
    "PinWright.widget.required_names.widget_import_xml.MissingElementName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetImportXmlRejectsMissingElementNameDirectTest::RunTest(const FString& Parameters)
{
    const FString WidgetPath = MakeRequiredNameAssetPath(TEXT("WBP_RequiredNameXml"));
    // The fixture is RF_Standalone, so the periodic suite GC keeps it alive.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(WidgetPath);
    };

    UWidgetBlueprint* WidgetBP = MakeRequiredNameWidgetBlueprint(WidgetPath);
    TestNotNull(TEXT("widget blueprint allocated"), WidgetBP);
    if (!WidgetBP || !WidgetBP->WidgetTree)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), WidgetPath);
    Payload->SetStringField(TEXT("xml"), TEXT("<CanvasPanel />"));

    return ExpectMissingNameFailure(*this, TEXT("widget.import_xml"), Payload,
        TEXT("VALIDATION_FAILED"), TEXT("missing required name attribute"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetCreateAnimationRejectsMissingNameDirectTest,
    "PinWright.widget.required_names.widget_create_widget_animation.MissingAnimationName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetCreateAnimationRejectsMissingNameDirectTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), TEXT("/Game/_Test/WBP_RequiredName"));

    return ExpectMissingNameFailure(*this, TEXT("widget.create_widget_animation"), Payload,
        TEXT("MISSING_PARAMETER"), TEXT("animationName"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetBindRejectsMissingFunctionNameDirectTest,
    "PinWright.widget.required_names.widget_bind.MissingFunctionName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetBindRejectsMissingFunctionNameDirectTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), TEXT("/Game/_Test/WBP_RequiredName"));
    Payload->SetStringField(TEXT("widgetName"), TEXT("Label"));
    Payload->SetStringField(TEXT("propertyName"), TEXT("Text"));

    return ExpectMissingNameFailure(*this, TEXT("widget.bind"), Payload,
        TEXT("INVALID_PARAMS"), TEXT("functionName"));
}
