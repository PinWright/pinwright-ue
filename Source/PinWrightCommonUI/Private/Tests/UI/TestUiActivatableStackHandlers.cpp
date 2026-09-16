// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Tests/Widget/WidgetTestFixtures.h"
#include "Tests/TestSkipReporting.h"

#include "Dom/JsonObject.h"

#include "WidgetBlueprint.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "CommonActivatableWidget.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiActivatablePushBadClassTest,
    "PinWright.ui.activatable.PushRejectsBadClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUiActivatablePushBadClassTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("host"), TEXT("AnyHost"));
    Payload->SetStringField(TEXT("stack"), TEXT("AnyStack"));
    Payload->SetStringField(TEXT("widgetClass"), TEXT("/Game/_Test/NoSuchClass"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("ui.activatable_push handler found"),
        InvokeHandlerWithCapture(TEXT("ui.activatable_push"), Payload, Capture));
    TestTrue(TEXT("response was sent"), Capture.bWasCalled);
    TestFalse(TEXT("unloadable widget class must not succeed"), Capture.bSuccess);
    TestEqual(TEXT("unloadable widget class returns CLASS_NOT_FOUND"),
        Capture.ErrorCode,
        FString(TEXT("CLASS_NOT_FOUND")));

    return true;
}

// Regression for E-activatable-push-requires-c-suffix: ui.activatable_push must
// accept the bare Blueprint asset path on widgetClass (the form
// widget.create_widget_blueprint returns and ui.create_hud accepts) without the
// caller appending the generated-class _C suffix. The handler routes widgetClass
// through ResolveUClass, which resolves /Game/.../WBP.WBP -> the generated class.
//
// Counterfactual: before the fix the handler did a raw
// LoadClass<UCommonActivatableWidget>(nullptr, *ClassPath), which does not append
// _C and rejects the bare asset path with CLASS_NOT_FOUND *before* the host/stack
// lookup ever runs. With the fix, class resolution succeeds and the IsChildOf
// guard passes, so the handler advances past resolution and fails later at the
// PIE host lookup (HOST_NOT_FOUND) — there is no live PIE world in a headless
// automation run. Asserting the error is NOT CLASS_NOT_FOUND (specifically that
// it reaches HOST_NOT_FOUND) therefore fails if the fix is reverted to raw
// LoadClass, while never depending on a running PIE session.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiActivatablePushBareClassPathResolvesTest,
    "PinWright.ui.activatable.PushBareClassPathResolves",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUiActivatablePushBareClassPathResolvesTest::RunTest(const FString& Parameters)
{
    // 1. Author a real WidgetBlueprint subclassing UCommonActivatableWidget so its
    //    generated class lives at "<Path>.<Asset>_C" and the bare "<Path>.<Asset>"
    //    path only resolves via ResolveUClass's _C/GeneratedClass fallback.
    // GUID-suffixed package path so parallel/leftover runs never collide on a shared name.
    const FString PackagePath = WidgetTestFixtures::MakeWidgetAssetPath(TEXT("WBP_ActivatablePushBarePath"));
    const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);

    UPackage* Package = CreatePackage(*PackagePath);
    if (!TestNotNull(TEXT("package created"), Package))
    {
        return true;
    }

    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        UCommonActivatableWidget::StaticClass(),
        Package,
        FName(*AssetName),
        BPTYPE_Normal,
        UWidgetBlueprint::StaticClass(),
        UWidgetBlueprintGeneratedClass::StaticClass());

    if (!BP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("probe-blueprint-unavailable"),
            TEXT("Skipping: FKismetEditorUtilities::CreateBlueprint returned null"));
        return true;
    }
    FKismetEditorUtilities::CompileBlueprint(BP);

    if (!TestNotNull(TEXT("blueprint has a generated class after compile"), BP->GeneratedClass.Get()))
    {
        return true;
    }

    // The bare asset path (no _C) — the form widget.create_widget_blueprint returns.
    const FString BareClassPath = ToObjectPath(PackagePath);

    // 2. Push with the bare path against a host that does not exist in any live world.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("host"), TEXT("NoSuchHost_ActivatablePushBarePath"));
    Payload->SetStringField(TEXT("stack"), TEXT("NoSuchStack"));
    Payload->SetStringField(TEXT("widgetClass"), BareClassPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("ui.activatable_push handler found"),
        InvokeHandlerWithCapture(TEXT("ui.activatable_push"), Payload, Capture));
    TestTrue(TEXT("response was sent"), Capture.bWasCalled);

    // With no PIE host the handler reaches the host lookup and reports HOST_NOT_FOUND —
    // proof the bare path passed class resolution + the IsChildOf guard. This equality
    // strictly implies != CLASS_NOT_FOUND, so it also catches a revert to raw LoadClass
    // (which would bail at resolution with CLASS_NOT_FOUND before the host lookup runs).
    TestEqual(TEXT("bare class path advances to the host lookup (HOST_NOT_FOUND)"),
        Capture.ErrorCode,
        FString(TEXT("HOST_NOT_FOUND")));

    // 3. Cleanup the throwaway asset via the shared disposal path (carries the 5.4
    //    force-delete crash guard; centralizes the asset-teardown idiom).
    CleanupTestAsset(PackagePath);

    return true;
}
